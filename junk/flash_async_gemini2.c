//
// gemini2.c geminiによるコード2つ目
// regs/qmi.h を渡してこの通り作れと言ってでてきたもの。
// ロジック csr_busy, txfull の判定あたりがChatGPTコード(動くことが分かっている)と
// 微妙に異なる。このデバッグするよりも、もとのChatGPTコードで先に進んだ方がよさそうだ。
//

#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

// レジスタ直叩き用のベースポインタ定義（構造体エラーの完全回避）
#define QMI_BASE_PTR ((volatile uint32_t *)XIP_QMI_BASE)
#define QMI_REG_DIRECT_CSR (QMI_BASE_PTR[0]) // オフセット 0x00
#define QMI_REG_DIRECT_TX (QMI_BASE_PTR[1])  // オフセット 0x04
#define QMI_REG_DIRECT_RX (QMI_BASE_PTR[2])  // オフセット 0x08
#define QMI_REG_M0_TIMING (QMI_BASE_PTR[3])  // オフセット 0x0c

// ==========================================
// プリミティブ1: QMIダイレクトモードによるSPIコマンド送受信
// ==========================================
void flash_qmi_transfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    uint32_t save_csr = qmi_hw->direct_csr;

    // CS0nをアサート（Lowに）
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

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

    // CS0nをネゲート（Highに）してダイレクトモードを抜ける
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;
    qmi_hw->direct_csr = save_csr; // 元の状態に戻す
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
    tx[4] = 0x00; // ダミーバイト
    tx[5] = tx[6] = tx[7] = tx[8] = 0x00;

    flash_qmi_transfer(tx, rx, 9);

    // 受信バッファの後半4バイト（DWORDデータ）を結合
    return ((uint32_t)rx[5]) |
           ((uint32_t)rx[6] << 8) |
           ((uint32_t)rx[7] << 16) |
           ((uint32_t)rx[8] << 24);
}

// ==========================================
// プリミティブ3: 決定したダミークロック数をQMI（XIP読み出し側）に適用
// ==========================================
void qmi_apply_dummy_clocks(uint8_t total_dummy_cycles) {
    uint32_t timing = qmi_hw->m[0].timing;

    // 既存のダミークロック設定（M0_TIMING_DUMMY_CLK_BITS）をクリア
    timing &= ~QMI_M0_TIMING_RXDELAY_BITS; // ※ヘッダ上のマクロ指定に合わせてマスク
    // もし提供ヘッダにDUMMY_CLK_BITSがない場合、安全に下位8ビット以降のフィールドを処理
    timing &= ~((uint32_t)0x3F << 8); // ビット13:8を確実にクリア

    // 新しいダミークロック数をセット（ビット8から左シフトして挿入）
    timing |= (((uint32_t)total_dummy_cycles << 8) & ((uint32_t)0x3F << 8));

    qmi_hw->m[0].timing = timing;
}

// ==========================================
// フラッシュメモリの内部Busy(WIP)状態監視（1回のみチェック）
// ==========================================
static bool is_flash_device_busy(void) {
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

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
// 非同期（スタート・ポール）リード／ライト／消去
// ==========================================

// [Read] 開始
void flash_async_read_start(flash_async_ctx_t *ctx, uint32_t flash_addr, uint8_t *ram_buf, size_t len) {
    ctx->flash_addr = flash_addr;
    ctx->buffer = ram_buf;
    ctx->total_len = len;
    ctx->processed_len = 0;
    ctx->state = FLASH_BUSY_READ;

    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

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

    // 古い受信バッファをクリア（空になるまで吸い出し）
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

    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0x06; // WREN
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;

    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
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
        qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS; // 内部書込み開始

        if (ctx->processed_len < ctx->total_len) {
            while (is_flash_device_busy())
                ;

            qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
            while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
                ;
            qmi_hw->direct_tx = 0x06;
            while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
                ;
            qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;

            qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
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

    // フラッシュメモリが内部で消去（ Busy / WIP ）を実行中なら、即座に制御を戻す
    if (is_flash_device_busy())
        return FLASH_BUSY_ERASE;

    // 前回のセクター消去が完了した進捗をここで加算 (初回はスキップ)
    if (ctx->processed_len < ctx->total_len && qmi_hw->direct_csr == QMI_DIRECT_CSR_EN_BITS) {
        ctx->processed_len += FLASH_SECTOR_SIZE;
    }

    // 全指定領域の消去が完了したかチェック
    if (ctx->processed_len >= ctx->total_len) {
        ctx->state = FLASH_COMPLETE;
        return FLASH_COMPLETE;
    }

    uint32_t current_sector_addr = ctx->flash_addr + ctx->processed_len;

    // -------------------------------------------------------------
    // 手順1. Write Enable (WREN: 0x06) コマンドの完全送出
    // -------------------------------------------------------------
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0x06; // WREN

    // 送信FIFOが空になり、かつ物理ピンへのシフト転送が完了（BUSYフラグが0）するまで確実に待つ
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
        ;

    // 一度CSを上げて、WRENコマンドをフラッシュメモリ内部にラッチさせる（必須手順）
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;

    // -------------------------------------------------------------
    // 手順2. Sector Erase (0x20) ＋ アドレス3バイトの完全送出
    // -------------------------------------------------------------
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
        ;
    qmi_hw->direct_tx = 0x20; // Sector Erase (4kB)

    uint8_t addr_bytes[3] = {
        (current_sector_addr >> 16) & 0xFF,
        (current_sector_addr >> 8) & 0xFF,
        current_sector_addr & 0xFF};

    for (int i = 0; i < 3; i++) {
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            ;
        qmi_hw->direct_tx = addr_bytes[i];
    }

    // コマンドとアドレス3バイトが完全にフラッシュに送り出されるまで確実に待つ
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
        ;

    // CSを上げることで、フラッシュメモリ側の「内部物理消去プロセス（WIP=1）」がキックされる
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;

    return ctx->state;
}
