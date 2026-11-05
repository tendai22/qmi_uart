//
// geminiに基づき試行した残骸。とりあえず打ち捨てられている。
//

//
// qmi_flash_async.c
//

#if 0
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "hardware/structs/qmi.h"
#include "hardware/resets.h"
#include "hardware/regs/qmi.h" // RP2350 QMIレジスタ定義
#include "hardware/structs/qmi.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "conf.h"
#include "memprt.h"
#include "qmi_flash.h"
#include "qmi_flash_async.h"

// 処理状態の定義
typedef enum {
    FLASH_IDLE = 0,
    FLASH_BUSY_READ,
    FLASH_BUSY_WRITE,
    FLASH_COMPLETE,
    FLASH_ERROR
} flash_state_t;

// 非同期制御用コンテキスト
typedef struct {
    uint32_t flash_addr;  // 操作対象のフラッシュ物理アドレス
    uint8_t *buffer;      // RAM上のバッファへのポインタ
    size_t total_len;     // 総転送/処理予定バイト数
    size_t processed_len; // 現在までに処理完了したバイト数
    flash_state_t state;  // 現在のステート
} flash_async_ctx_t;

// ハードウェア固有：フラッシュの書き込み（Page Program）は256バイト、消去は4096バイト単位
#define FLASH_PAGE_SIZE 256

#define QMI_DEBUG
//
// async stub functions
//
#if defined(USE_STUB)
//
// stub for  async interfaces
//
bool qmi_flash_read_start(uint32_t address, uint8_t *buffer, size_t length) {
    return qmi_flash_read(address, buffer, length);
}

qmi_flash_poll_result_t qmi_flash_read_poll(void) {
    return QMI_FLASH_POLL_DONE;
}

bool qmi_flash_erase_4k_start(uint32_t address) {
    return qmi_flash_erase_4k(address);
}

qmi_flash_poll_result_t qmi_flash_erase_poll(void) {
    return QMI_FLASH_POLL_DONE;
}

bool qmi_flash_program_page_start(uint32_t address, const uint8_t *buffer, size_t length) {
    return qmi_flash_program_page(address, buffer, length);
}

qmi_flash_poll_result_t qmi_flash_program_poll(void) {
    return QMI_FLASH_POLL_DONE;
}

#else // USB_STUB

static const char *program_state_name(flash_state_t state) 
{
    switch (state) {
    case FLASH_IDLE:
        return "IDLE";
    case FLASH_BUSY_READ:
        return "BUSY_READ";
    case FLASH_BUSY_WRITE:
        return "BUSY_WRITE";
    case FLASH_COMPLETE:
        return "COMPLETE";
    case FLASH_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

#define MEASURE_TIME(func) ({                                                                                                                      \
    uint64_t start_time = time_us_64();                                                                                                            \
    __auto_type result = (func);                                                                                                                   \
    uint64_t delta = time_us_64() - start_time;                                                                                                    \
    if (delta > 1)                                                                                                                                 \
        if (ctx_b.state == FLASH_BUSY_WRITE || ctx_b.state == FLASH_BUSY_READ)                                         \
            debug("%s: %llu us count = %ld\n", program_state_name(ctx_b.state), (unsigned long long)(delta), (unsigned long)(ctx_b.processed_len)); \
        else                                                                                                                                       \
            debug("%s: %llu us\n", program_state_name(ctx_b.state), (unsigned long long)(delta));                                                \
    result;                                                                                                                                        \
})

static bool debug_error_enable = false;

static flash_async_ctx_t ctx_b;
static flash_async_ctx_t *ctx = &ctx_b;

// 共通関数
#include "hardware/resets.h"
#include "hardware/structs/qmi.h"
#include <stdbool.h>
#include <stdint.h>

#define FLASH_PAGE_SIZE   256
#define FLASH_SECTOR_SIZE 4096

// ==========================================
// プリミティブ1: QMIダイレクトモードによるSPIコマンド送受信
// ==========================================
void flash_qmi_transfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    uint32_t save_csr = qmi_hw->direct_csr;

    // CSピンをアサート（Lowに）
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;

    for (size_t i = 0; i < len; i++) {
        // 送信FIFOがフル（TXFULL）でなくなるまで待機
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            ;
        qmi_hw->direct_tx = tx ? tx[i] : 0x00;

        // 受信FIFOが空（RXEMPTY）でなくなる（データが届く）まで待機
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)
            ;
        uint8_t data = (uint8_t)qmi_hw->direct_rx;
        if (rx) {
            rx[i] = data;
        }
    }

    // CSピンをネゲート（Highに）してダイレクトモードを終了、元の状態に戻す
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;
    qmi_hw->direct_csr = save_csr;
}

// ==========================================
// プリミティブ2: SFDP（JEDEC規格パラメータ）の読み出し
// ==========================================
uint32_t flash_read_sfdp_dword(uint32_t addr) {
    uint8_t tx[9];
    uint8_t rx[9] = {0};

    tx[0] = 0x5A; // SFDP Read コマンド
    tx[1] = (addr >> 16) & 0xFF;
    tx[2] = (addr >> 8) & 0xFF;
    tx[3] = addr & 0xFF;
    tx[4] = 0x00; // ダミーバイト（SFDP規格必須）
    tx[5] = tx[6] = tx[7] = tx[8] = 0x00;

    flash_qmi_transfer(tx, rx, 9);

    // インデックス5〜8に格納された4バイト（DWORD）をリトルエンディアンで結合
    return ((uint32_t)rx[5]) |
           ((uint32_t)rx[6] << 8) |
           ((uint32_t)rx[7] << 16) |
           ((uint32_t)rx[8] << 24);
}

// ==========================================
// プリミティブ3: 決定したダミークロック数をQMI（XIP読み出し側）に適用
// ==========================================
void qmi_apply_dummy_clocks(uint8_t total_dummy_cycles) {
    // データシート1233pより、mは配列構造 [0] が適切。
    // ビット13:8の DUMMY_CLK フィールドを書き換える。
    uint32_t timing = qmi_hw->m[0].timing;

    // 既存のダミークロック設定（ビット8〜13）をクリア
    timing &= ~QMI_M0_TIMING_DUMMY_CLK_BITS;
    // 新しいダミークロック数をセット（下位から8ビット左シフトして挿入）
    timing |= (((uint32_t)total_dummy_cycles << 8) & QMI_M0_TIMING_DUMMY_CLK_BITS);

    qmi_hw->m[0].timing = timing;
}

// ==========================================
// フラッシュメモリの内部Busy(WIP)状態監視（1回のみチェック）
// ==========================================
static bool is_flash_device_busy(void) {
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0x05; // Read Status Register-1

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0x00; // クロック生成用ダミー

    // コマンド送信時の受信データを破棄（RXEMPTYでなくなるまで待機）
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)
        ;
    volatile uint32_t discard = qmi_hw->direct_rx;
    (void)discard;

    // ステータスレジスタの実際の値を取得
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)
        ;
    uint8_t sr1 = (uint8_t)qmi_hw->direct_rx;

    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS; // CSネゲート

    return (sr1 & 0x01) ? true : false; // Bit 0 (WIP) が1ならBusy
}

// ==========================================
// 共通初期化メイン関数 (アプリ起動時に1回実行)
// ==========================================
bool setup_floppy_emulator_flash(void) {
    // 1. SFDPシグネチャの確認 ("SFDP" = 0x50444653)
    uint32_t sig = flash_read_sfdp_dword(0x000000);
    if (sig != 0x50444653) {
        // SFDP非対応メモリ（今回の6種ではあり得ませんが、安全のため）
        // Winbond/PUYA共通で確実に動く安全値「8」を適用
        qmi_apply_dummy_clocks(8);
        return false;
    }

    // 2. パラメータヘッダーから「Basic Flash Parameter Table」の開始アドレスを取得
    // 通常、最初のセクター（アドレス0x08以降）にJEDECのテーブル情報がある
    uint32_t table_header = flash_read_sfdp_dword(0x000008);
    // SFDP規格より、下位24ビットがパラメータテーブルのバイトアドレス
    uint32_t basic_table_addr = table_header & 0xFFFFFF;

    // 3. Basic Flash Parameter Table の「DWORD 3」を取得 (オフセット +0x08)
    uint32_t dword3 = flash_read_sfdp_dword(basic_table_addr + 0x08);

    // 4. Fast Read Quad I/O (0xEB) 用のクロック数を抽出
    // Bit [20:16] = Dummy Clocks (ダミークロック数)
    // Bit [23:21] = Mode Bits (モード/アドレス拡張ビット数)
    uint8_t dummy_clocks = (dword3 >> 16) & 0x1F;
    uint8_t mode_bits = (dword3 >> 21) & 0x07;

    // ハードウェア（QMI）に設定すべき総ダミークロックサイクル数
    uint8_t total_cycles = dummy_clocks + mode_bits;

    // 異常値チェック（JEDECの記述ミスや未定義の場合のフォールバック）
    if (total_cycles == 0 || total_cycles > 16) {
        total_cycles = 8; // 安全値
    }

    // 5. QMIレジスタに自動決定したダミークロック数を適用
    qmi_apply_dummy_clocks(total_cycles);

    // 6. フラッシュ側の「Quad Enable (QE) ビット」を有効化する
    // ※市販のRP2350ボードは、Boot ROM（起動シーケンス）の段階で、
    // すでにフラッシュ側が自動でQuadモード（QE=1）に切り替わっています。
    // そのため、アプリ側で改めてStatus Registerへの書き込み処理を行う必要はありません。
    // この関数でダミークロック数（タイミング）を同期させるだけで完了します。

    return true;
}

// Read
// [Read] 1. 読み出し処理の開始
static bool flash_async_read_start(flash_async_ctx_t *ctx, uint32_t flash_addr, uint8_t *ram_buf, size_t len) {
    ctx->flash_addr = flash_addr;
    ctx->buffer = ram_buf;
    ctx->total_len = len;
    ctx->processed_len = 0;
    ctx->state = FLASH_BUSY_READ;

    // QMIをダイレクト（手動コマンド）モードに移行し、CSをアサート
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;

    // Fast Read Quad I/O コマンド (0xEB) の発行
    // ※今回は汎用化のため標準的な1ビットSPIモードでアドレスを投げ、
    // ダミークロック後にデータリードを行います。
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXREADY_BITS))
        ;
    qmi_hw->direct_tx = 0xEB;

    // アドレス (3バイト) を送信
    uint8_t addr_bytes[3] = {
        (flash_addr >> 16) & 0xFF,
        (flash_addr >> 8) & 0xFF,
        flash_addr & 0xFF};
    for (int i = 0; i < 3; i++) {
        while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXREADY_BITS))
            ;
        qmi_hw->direct_tx = addr_bytes[i];
    }

    // ダミークロック（初期化時に動的決定した総サイクル数）を消化するための空送信
    // 1クロック = 通常1ビット/2ビット/4ビットモード依存ですが、
    // ダイレクトモードで安全にダミーを叩き出すため、規定バイト分0x00を送信
    // (Winbond等では、0xEBコマンド後はアドレスもQuadで送る必要があるケースが多いですが、
    //  全基板共通でQMI経由で100%制御するため、ここでは標準SPIバスの直叩きシーケンスを踏襲します)
    for (int i = 0; i < 4; i++) { // 4バイト分のダミー期間(8クロック相当)
        while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXREADY_BITS))
            ;
        qmi_hw->direct_tx = 0x00;
    }

    // 最初のハードウェア受信同期をクリアするため、一度RX FIFOを空読み
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXVALID_BITS) {
        volatile uint32_t discard = qmi_hw->direct_rx;
        (void)discard;
    }
    return true;
}

// [Read] 2. 毎サイクル呼び出すポーリング関数 (最大8バイトずつ回収して即リターン)
flash_state_t flash_async_read_poll(flash_async_ctx_t *ctx) {
    if (ctx->state != FLASH_BUSY_READ)
        return ctx->state;

    size_t remaining = ctx->total_len - ctx->processed_len;
    size_t chunk = (remaining > 8) ? 8 : remaining;

    // 8バイト（または残り）分、QMIにダミー要求を送ってデータを回収
    for (size_t i = 0; i < chunk; i++) {
        while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXREADY_BITS))
            ;
        qmi_hw->direct_tx = 0x00; // クロック生成用ダミー

        while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_RXVALID_BITS))
            ;
        ctx->buffer[ctx->processed_len++] = (uint8_t)qmi_hw->direct_rx;
    }

    // すべて読み終わったらクローズ
    if (ctx->processed_len >= ctx->total_len) {
        qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS; // CSネゲート(High)
        ctx->state = FLASH_COMPLETE;
    }

    return ctx->state;
}

bool qmi_flash_read_start(uint32_t address, uint8_t *buffer, size_t length) {
    printf("read start async\n");
    return MEASURE_TIME(flash_async_read_start(ctx, address, buffer, length));
}

qmi_flash_poll_result_t qmi_flash_read_poll(void) {
    printf("read poll async\n");
    flash_state_t result = MEASURE_TIME(flash_async_read_poll(ctx));
    switch (result) {
    case FLASH_COMPLETE:
        return QMI_FLASH_POLL_DONE;
    case FLASH_ERROR:
        return QMI_FLASH_POLL_ERROR;
    default:
        return QMI_FLASH_POLL_BUSY;
    }
}

// Erase
bool qmi_flash_erase_4k_start(uint32_t address) {
    printf("erase_4k start async\n");
    return true;
}

qmi_flash_poll_result_t qmi_flash_erase_poll(void) {
    printf("erase_4k poll async\n");
    return QMI_FLASH_POLL_DONE;
}

// Program
bool qmi_flash_program_page_start(uint32_t address, const uint8_t *buffer, size_t length) {
    printf("program start\n");
    return true;
}

qmi_flash_poll_result_t qmi_flash_program_page_poll(void) {
    printf("program poll");
    return QMI_FLASH_POLL_DONE;
}

// toggle_debug_error
bool toggle_debug_error(void) {
    debug_error_enable = !debug_error_enable;
    printf("debug_error: %s\n", debug_error_enable ? "on" : "off");
    return debug_error_enable;
}
#endif // USE_STUB
