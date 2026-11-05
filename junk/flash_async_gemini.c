//
// geminiによるコード1, コンパイルが通らない。
// regs/qmi.h のマクロ定義を見ずに自分で適当なマクロを作っていた。 
//


#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"

// 処理状態の定義
typedef enum {
    FLASH_IDLE = 0,
    FLASH_BUSY_READ,
    FLASH_BUSY_WRITE,
    FLASH_BUSY_ERASE,
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

#define FLASH_PAGE_SIZE 256
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
// 非同期（スタート・ポール）各種ロジック
// ==========================================

// [Read] 開始
void flash_async_read_start(flash_async_ctx_t *ctx, uint32_t flash_addr, uint8_t *ram_buf, size_t len) {
    ctx->flash_addr = flash_addr;
    ctx->buffer = ram_buf;
    ctx->total_len = len;
    ctx->processed_len = 0;
    ctx->state = FLASH_BUSY_READ;

    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0xEB; // Fast Read Quad I/O

    uint8_t addr_bytes[3] = {(flash_addr >> 16) & 0xFF, (flash_addr >> 8) & 0xFF, flash_addr & 0xFF};
    for (int i = 0; i < 3; i++) {
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            ;
        qmi_hw->direct_tx = addr_bytes[i];
    }

    for (int i = 0; i < 4; i++) { // 4バイト分のダミー期間(8クロック相当)
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            ;
        qmi_hw->direct_tx = 0x00;
    }

    // コマンドとアドレス送信によって溜まった古いRXデータをクリア（RXEMPTYになるまで吸い出す）
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)) {
        volatile uint32_t discard = qmi_hw->direct_rx;
        (void)discard;
    }
}

// [Read] ポーリング
flash_state_t flash_async_read_poll(flash_async_ctx_t *ctx) {
    if (ctx->state != FLASH_BUSY_READ)
        return ctx->state;

    size_t remaining = ctx->total_len - ctx->processed_len;
    size_t chunk = (remaining > 8) ? 8 : remaining;

    for (size_t i = 0; i < chunk; i++) {
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            ;
        qmi_hw->direct_tx = 0x00;

        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)
            ;
        ctx->buffer[ctx->processed_len++] = (uint8_t)qmi_hw->direct_rx;
    }

    if (ctx->processed_len >= ctx->total_len) {
        qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;
        ctx->state = FLASH_COMPLETE;
    }
    return ctx->state;
}

// [Write] 開始
void flash_async_write_start(flash_async_ctx_t *ctx, uint32_t flash_addr, const uint8_t *ram_buf, size_t len) {
    ctx->flash_addr = flash_addr;
    ctx->buffer = (uint8_t *)ram_buf;
    ctx->total_len = len;
    ctx->processed_len = 0;
    ctx->state = FLASH_BUSY_WRITE;

    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0x06; // WREN
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;

    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0x02; // Page Program

    uint8_t addr_bytes[3] = {(flash_addr >> 16) & 0xFF, (flash_addr >> 8) & 0xFF, flash_addr & 0xFF};
    for (int i = 0; i < 3; i++) {
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            ;
        qmi_hw->direct_tx = addr_bytes[i];
    }
}

// [Write] ポーリング
flash_state_t flash_async_write_poll(flash_async_ctx_t *ctx) {
    if (ctx->state != FLASH_BUSY_WRITE)
        return ctx->state;

    if (is_flash_device_busy())
        return FLASH_BUSY_WRITE;

    size_t remaining = ctx->total_len - ctx->processed_len;
    if (remaining == 0) {
        ctx->state = FLASH_COMPLETE;
        return FLASH_COMPLETE;
    }

    size_t current_addr = ctx->flash_addr + ctx->processed_len;
    size_t bytes_to_page_boundary = FLASH_PAGE_SIZE - (current_addr % FLASH_PAGE_SIZE);

    size_t chunk = (remaining > 8) ? 8 : remaining;
    if (chunk > bytes_to_page_boundary) {
        chunk = bytes_to_page_boundary;
    }

    for (size_t i = 0; i < chunk; i++) {
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            ;
        qmi_hw->direct_tx = ctx->buffer[ctx->processed_len++];
    }

    current_addr = ctx->flash_addr + ctx->processed_len;
    if (current_addr % FLASH_PAGE_SIZE == 0 || ctx->processed_len >= ctx->total_len) {
        qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS; // CSを上げて内部書き込み開始

        if (ctx->processed_len < ctx->total_len) {
            while (is_flash_device_busy())
                ; // ページ跨ぎ境界のみ極短時間の同期

            qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;
            while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
                ;
            qmi_hw->direct_tx = 0x06;
            while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
                ;
            qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;

            qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;
            while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
                ;
            qmi_hw->direct_tx = 0x02;

            uint8_t next_addr[3] = {(current_addr >> 16) & 0xFF, (current_addr >> 8) & 0xFF, current_addr & 0xFF};
            for (int i = 0; i < 3; i++) {
                while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
                    ;
                qmi_hw->direct_tx = next_addr[i];
            }
        }
    }
    return ctx->state;
}

// [Erase] 開始
void flash_async_erase_start(flash_async_ctx_t *ctx, uint32_t flash_addr, size_t len) {
    ctx->flash_addr = flash_addr;
    ctx->buffer = NULL;
    ctx->total_len = len;
    ctx->processed_len = 0;
    ctx->state = FLASH_BUSY_ERASE;
}

// [Erase] ポーリング
flash_state_t flash_async_erase_poll(flash_async_ctx_t *ctx) {
    if (ctx->state != FLASH_BUSY_ERASE)
        return ctx->state;

    if (is_flash_device_busy())
        return FLASH_BUSY_ERASE;

    if (ctx->processed_len < ctx->total_len && qmi_hw->direct_csr == QMI_DIRECT_CSR_EN_BITS) {
        ctx->processed_len += FLASH_SECTOR_SIZE;
    }

    if (ctx->processed_len >= ctx->total_len) {
        ctx->state = FLASH_COMPLETE;
        return FLASH_COMPLETE;
    }

    uint32_t current_sector_addr = ctx->flash_addr + ctx->processed_len;

    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0x06; // WREN
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;

    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0x20; // Sector Erase (4kB)

    uint8_t addr_bytes[3] = {(current_sector_addr >> 16) & 0xFF, (current_sector_addr >> 8) & 0xFF, current_sector_addr & 0xFF};
    for (int i = 0; i < 3; i++) {
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            ;
        qmi_hw->direct_tx = addr_bytes[i];
    }
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS; // CSを上げて内部消去を開始
    return ctx->state;
}