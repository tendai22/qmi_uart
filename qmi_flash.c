//
// qmi_flash.c
//

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/qmi.h"
#include "hardware/flash.h"
#include "pico/time.h"

#include "conf.h"
#include "qmi_flash.h"

#if defined(USE_STUB)
extern void qmi_stub_init(void);
#endif
// -----------------------------------------------------------------------------
// W25Q32RV commands
// -----------------------------------------------------------------------------

#define CMD_READ_JEDEC_ID 0x9F
#define CMD_READ_SR1 0x05
#define CMD_READ_SR2 0x35
#define CMD_READ_SR3 0x15

#define EXPECTED_JEDEC_ID_0 0xEF
#define EXPECTED_JEDEC_ID_1 0x40
#define EXPECTED_JEDEC_ID_2 0x16

#define SR1_BUSY_MASK 0x01
#define SR1_WEL_MASK 0x02
#define SR2_QE_MASK 0x02

// -----------------------------------------------------------------------------
// QMI Direct Mode
// -----------------------------------------------------------------------------

// FIFO 待ちのタイムアウト。
// 1 回の待ちループで tight_loop_contents() を実行する。
#define QMI_TIMEOUT_COUNT 10000000u

// 目標 SCK 周波数。
// clk_sys が 150 MHz の場合、CLKDIV=30 となり約 5 MHz。
#define QMI_TARGET_SCK_HZ 5000000u

static uint32_t sys_hz;
static uint32_t qmi_clkdiv;

typedef struct
{
    uint8_t command;
    uint32_t csr_cs_assert;
    uint32_t csr_command_done;
    uint32_t csr_data_done;
} qmi_diag_t;

static qmi_diag_t qmi_diag[4];
static unsigned int qmi_diag_count = 0;

// -----------------------------------------------------------------------------
// QMI diagnostic
// -----------------------------------------------------------------------------

static void qmi_dump_registers(const char *label)
{
    uint32_t csr = qmi_hw->direct_csr;

    printf("\n[QMI DUMP] %s\n", label);
    printf("  DIRECT_CSR = 0x%08lx\n", (unsigned long)csr);
    printf("    EN       = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_EN_BITS) != 0));
    printf("    BUSY     = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_BUSY_BITS) != 0));
    printf("    RXEMPTY  = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_RXEMPTY_BITS) != 0));
    printf("    RXFULL   = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_RXFULL_BITS) != 0));
    printf("    TXEMPTY  = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_TXEMPTY_BITS) != 0));
    printf("    TXFULL   = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_TXFULL_BITS) != 0));
    printf("    RXLEVEL  = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_RXLEVEL_BITS) >> QMI_DIRECT_CSR_RXLEVEL_LSB));
    printf("    TXLEVEL  = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_TXLEVEL_BITS) >> QMI_DIRECT_CSR_TXLEVEL_LSB));
    printf("    CLKDIV   = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_CLKDIV_BITS) >> QMI_DIRECT_CSR_CLKDIV_LSB));
    printf("    RXDELAY  = %lu\n", (unsigned long)((csr & QMI_DIRECT_CSR_RXDELAY_BITS) >> QMI_DIRECT_CSR_RXDELAY_LSB));
}

// -----------------------------------------------------------------------------
// QMI wait helpers
// -----------------------------------------------------------------------------

static bool qmi_wait_not_busy(void)
{
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) == 0) {
            return true;
        }
        tight_loop_contents();
    }
    return false;
}

static bool qmi_wait_tx_ready(void)
{
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) == 0) {
            return true;
        }
        tight_loop_contents();
    }
    return false;
}

static bool qmi_wait_rx_ready(void)
{
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) == 0) {
            return true;
        }
        tight_loop_contents();
    }
    return false;
}

// -----------------------------------------------------------------------------
// QMI Direct Mode start / stop
// -----------------------------------------------------------------------------

static bool qmi_direct_mode_start(void)
{
    // Direct Mode を有効にする。
    //
    // AUTO_CS0N は使わず、ASSERT_CS0N によって CS を明示的に制御する。
    // CLKDIV は clk_sys に応じて計算した値を設定する。

    uint32_t csr = ((qmi_clkdiv & 0xFFu) << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    qmi_hw->direct_csr = csr;

    // Direct Mode 有効化時点ですでに進行中だった XIP 転送が
    // 完了するのを待つ。
    if (!qmi_wait_not_busy()) {
        return false;
    }
    return true;
}

static bool qmi_direct_mode_stop(void)
{
    // 転送完了を待つ。
    if (!qmi_wait_not_busy()) {
        return false;
    }
    // CS0 を High に戻す（ASSERT_CS0N をクリア）。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    // Direct Mode を解除する。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
    return true;
}

// -----------------------------------------------------------------------------
// W25Q32RV register read
// -----------------------------------------------------------------------------

static bool qmi_flash_read_register(uint8_t command, uint8_t *buffer, size_t length)
{
    if (buffer == NULL || length == 0) {
        return false;
    }
    if (!qmi_wait_not_busy()) {
        return false;
    }

    // 診断情報の保存先
    unsigned int diag_index = qmi_diag_count;

    if (diag_index < 4) {
        qmi_diag[diag_index].command = command;
    }

    ++qmi_diag_count;

    // RX FIFO の残留データを排除
    for (unsigned int i = 0; i < 16; ++i) {
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) {
            break;
        }
        (void)qmi_hw->direct_rx;
    }

    // CS0n を Low にする
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (diag_index < 4) {
        qmi_diag[diag_index].csr_cs_assert = qmi_hw->direct_csr;
    }

    // CS0n が実際に Assert されたか確認
    if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_ASSERT_CS0N_BITS) == 0) {
        goto error;
    }

    // コマンド送信
    if (!qmi_wait_tx_ready()) {
        goto error;
    }
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | command;

    // コマンドの送信完了を待つ。
    // CS は ASSERT_CS0N によって Low のまま維持される。
    if (!qmi_wait_not_busy()) {
        goto error;
    }

    if (diag_index < 4) {
        qmi_diag[diag_index].csr_command_done = qmi_hw->direct_csr;
    }

    // データ読み出し
    for (size_t i = 0; i < length; ++i) {
        if (!qmi_wait_tx_ready()) {
            goto error;
        }

        qmi_hw->direct_tx = 0xFF;

        if (!qmi_wait_rx_ready()) {
            goto error;
        }
        buffer[i] = (uint8_t)qmi_hw->direct_rx;
    }

    if (!qmi_wait_not_busy()) {
        goto error;
    }

    if (diag_index < 4) {
        qmi_diag[diag_index].csr_data_done = qmi_hw->direct_csr;
    }

    // CS0n を High にする
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    return true;
error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    return false;
}

#define FLASH_CMD_READ_DATA 0x03
#define FLASH_READ_TIMEOUT_US 100000

static bool qmi_wait_busy_clear(void)
{
    uint32_t start = time_us_32();

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        if ((uint32_t)(time_us_32() - start) > FLASH_READ_TIMEOUT_US) {
            return false;
        }
    }
    return true;
}

static bool qmi_flash_read_data(uint32_t address, uint8_t *buffer, size_t length)
{
    uint8_t header[4];

    if (buffer == NULL || length == 0) {
        return false;
    }

    header[0] = FLASH_CMD_READ_DATA;
    header[1] = (uint8_t)(address >> 16);
    header[2] = (uint8_t)(address >> 8);
    header[3] = (uint8_t)address;

    /* 念のため、前回の受信データを破棄 */
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) == 0) {
        (void)qmi_hw->direct_rx;
    }

    /* CSをアサート */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_wait_busy_clear()) {
        goto error;
    }

    /* コマンド + 24-bitアドレスを送信 */
    for (size_t i = 0; i < sizeof(header); i++) {
        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | header[i];
        if (!qmi_wait_busy_clear()) {
            goto error;
        }
    }

    /* データを読み出す */
    for (size_t i = 0; i < length; i++) {
        /* ダミーデータを送信してクロックを発生させる */
        qmi_hw->direct_tx = 0xFF;

        if (!qmi_wait_busy_clear()) {
            goto error;
        }

        /* 受信FIFOにデータが届くまで待つ */
        uint32_t start = time_us_32();

        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) {
            if ((uint32_t)(time_us_32() - start) > FLASH_READ_TIMEOUT_US) {
                goto error;
            }
        }

        buffer[i] = (uint8_t)qmi_hw->direct_rx;
    }

    if (!qmi_wait_busy_clear()) {
        goto error;
    }

    /* CSを解除 */
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    return true;

error:
    /* エラー時もCSを解除 */
    (void)qmi_wait_busy_clear();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    return false;
}

//
// qmi_init
//

void qmi_init(void)
{
    // clk_sys に応じて CLKDIV を計算する。
    sys_hz = clock_get_hz(clk_sys);

    qmi_clkdiv = (sys_hz + QMI_TARGET_SCK_HZ - 1u) / QMI_TARGET_SCK_HZ;

    if (qmi_clkdiv < 1u) {
        qmi_clkdiv = 1u;
    }

    if (qmi_clkdiv > 256u) {
        qmi_clkdiv = 256u;
    }
#if defined(USE_STUB)
    qmi_stub_init();
#endif
}

//
// sync flash command functions
//
#if defined(USE_STUB)

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

//#undef FLASH_SIZE
//#define FLASH_SIZE      0x20000u   // dummy flash size 128kB
//#define SECTOR_SIZE     0x1000u
//#define PAGE_SIZE       0x100u

// 4 MiB pseudo Flash
static uint8_t pseudo_flash[FLASH_SIZE];

// Initialize pseudo Flash to erased state.
void qmi_stub_init(void)
{
    memset(pseudo_flash, 0xFF, sizeof(pseudo_flash));
}

// Read
bool qmi_flash_read(uint32_t address, uint8_t *buffer, size_t length)
{
    if (address > FLASH_SIZE || length > FLASH_SIZE - address)
        return false;

    memcpy(buffer, &pseudo_flash[address], length);
    return true;
}

// 4 KiB Sector Erase
bool qmi_flash_erase_4k(uint32_t address)
{
    if ((address & (SECTOR_SIZE - 1)) != 0)
        return false;

    if (address > FLASH_SIZE - SECTOR_SIZE)
        return false;

    memset(&pseudo_flash[address], 0xFF, SECTOR_SIZE);
    return true;
}

// Page Program
bool qmi_flash_program_page(uint32_t address, const uint8_t *buffer, size_t length)
{
    size_t i;

    if (length == 0 || length > PAGE_SIZE)
        return false;

    if (address / PAGE_SIZE != (address + length - 1) / PAGE_SIZE)
        return false;

    if (address > FLASH_SIZE || length > FLASH_SIZE - address)
        return false;

    // NOR Flash: programming can only change 1 -> 0.
    for (i = 0; i < length; i++) {
        if ((pseudo_flash[address + i] & buffer[i]) != buffer[i]) {
            return false;
        }
    }
    for (i = 0; i < length; i++) {
        pseudo_flash[address + i] &= buffer[i];
    }
    return true;
}

#else //USE_STUB

// -----------------------------------------------------------------------------
// W25Q32RV write commands
// -----------------------------------------------------------------------------

#define CMD_WRITE_ENABLE       0x06
#define CMD_SECTOR_ERASE_4K    0x20
#define CMD_PAGE_PROGRAM       0x02

// W25Q32RV の最大処理時間より余裕を持たせたタイムアウト。
// 実際のタイムアウト値はデータシートや実測に応じて調整する。
#define FLASH_ERASE_TIMEOUT_US    5000000u
#define FLASH_PROGRAM_TIMEOUT_US  1000000u

// -----------------------------------------------------------------------------
// Direct Mode transmit helper
// -----------------------------------------------------------------------------

// 1 byte を送信する。
// 書き込みコマンドでは受信 FIFO にデータを積まないよう NOPUSH を指定する。
static bool qmi_flash_write_byte(uint8_t data)
{
    if (!qmi_wait_tx_ready()) {
        return false;
    }

    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | data;

    return qmi_wait_not_busy();
}

// -----------------------------------------------------------------------------
// Write Enable
// -----------------------------------------------------------------------------

static bool qmi_flash_write_enable(void)
{
    // WREN は独立した CS トランザクションにする。
    if (!qmi_wait_not_busy()) {
        return false;
    }

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_WRITE_ENABLE)) {
        goto error;
    }

    if (!qmi_wait_not_busy()) {
        goto error;
    }

    // CS を High にして WREN コマンドを確定する。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    return true;

error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    return false;
}

// -----------------------------------------------------------------------------
// Wait until WIP/BUSY clears
// -----------------------------------------------------------------------------

static bool qmi_flash_wait_ready(uint32_t timeout_us)
{
    uint32_t start = time_us_32();

    while ((uint32_t)(time_us_32() - start) < timeout_us) {
        uint8_t sr1 = 0;

        // SR1 Read (05h)
        if (!qmi_flash_read_register(CMD_READ_SR1, &sr1, 1)) {
            return false;
        }

        if ((sr1 & SR1_BUSY_MASK) == 0) {
            return true;
        }
    }

    return false;
}

// -----------------------------------------------------------------------------
// Sector Erase 4 KiB
// -----------------------------------------------------------------------------

bool qmi_flash_erase_4k(uint32_t address)
{
    // 4 KiB セクタ境界チェック
    if ((address & (SECTOR_SIZE - 1u)) != 0) {
        return false;
    }

    // Flash 範囲チェック
    if (address >= FLASH_SIZE ||
        SECTOR_SIZE > FLASH_SIZE - address) {
        return false;
    }

    // QMI Direct Mode に入る前に XIP を初期化する。
    flash_start_xip();

    if (!qmi_direct_mode_start()) {
        return false;
    }

    bool ok = false;

    // Flash が前の処理を実行中でないことを確認する。
    if (!qmi_flash_wait_ready(FLASH_PROGRAM_TIMEOUT_US)) {
        goto done;
    }

    // WREN
    if (!qmi_flash_write_enable()) {
        goto done;
    }

    // Sector Erase: 20h + 24-bit address
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_SECTOR_ERASE_4K)) {
        goto erase_error;
    }

    if (!qmi_flash_write_byte((uint8_t)(address >> 16))) {
        goto erase_error;
    }

    if (!qmi_flash_write_byte((uint8_t)(address >> 8))) {
        goto erase_error;
    }

    if (!qmi_flash_write_byte((uint8_t)address)) {
        goto erase_error;
    }

    if (!qmi_wait_not_busy()) {
        goto erase_error;
    }

    // CS を High にして Erase を開始させる。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // Erase 完了待ち
    if (!qmi_flash_wait_ready(FLASH_ERASE_TIMEOUT_US)) {
        goto done;
    }

    ok = true;
    goto done;

erase_error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

done:
    if (!qmi_direct_mode_stop()) {
        return false;
    }

    return ok;
}

// -----------------------------------------------------------------------------
// Page Program
// -----------------------------------------------------------------------------

bool qmi_flash_program_page(
    uint32_t address,
    const uint8_t *buffer,
    size_t length)
{
    if (buffer == NULL || length == 0 || length > PAGE_SIZE) {
        return false;
    }

    // 1 回の Page Program でページ境界をまたがないこと。
    if ((address / PAGE_SIZE) !=
        ((address + length - 1u) / PAGE_SIZE)) {
        return false;
    }

    // Flash 範囲チェック
    if (address >= FLASH_SIZE ||
        length > FLASH_SIZE - address) {
        return false;
    }

    flash_start_xip();

    if (!qmi_direct_mode_start()) {
        return false;
    }

    bool ok = false;

    // Flash が前の処理を実行中でないことを確認する。
    if (!qmi_flash_wait_ready(FLASH_PROGRAM_TIMEOUT_US)) {
        goto done;
    }

    // WREN
    if (!qmi_flash_write_enable()) {
        goto done;
    }

    // Page Program: 02h + 24-bit address + data
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_PAGE_PROGRAM)) {
        goto program_error;
    }

    if (!qmi_flash_write_byte((uint8_t)(address >> 16))) {
        goto program_error;
    }

    if (!qmi_flash_write_byte((uint8_t)(address >> 8))) {
        goto program_error;
    }

    if (!qmi_flash_write_byte((uint8_t)address)) {
        goto program_error;
    }

    // Program data
    for (size_t i = 0; i < length; ++i) {
        if (!qmi_flash_write_byte(buffer[i])) {
            goto program_error;
        }
    }

    if (!qmi_wait_not_busy()) {
        goto program_error;
    }

    // CS を High にして Page Program を開始させる。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // Program 完了待ち
    if (!qmi_flash_wait_ready(FLASH_PROGRAM_TIMEOUT_US)) {
        goto done;
    }

    ok = true;
    goto done;

program_error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

done:
    if (!qmi_direct_mode_stop()) {
        return false;
    }

    return ok;
}

// -----------------------------------------------------------------------------
// Read Data
// -----------------------------------------------------------------------------

bool qmi_flash_read(uint32_t address, uint8_t *buffer, size_t length)
{
    if (buffer == NULL || length == 0) {
        return false;
    }

    if (address >= FLASH_SIZE ||
        length > FLASH_SIZE - address) {
        return false;
    }

    flash_start_xip();

    if (!qmi_direct_mode_start()) {
        return false;
    }

    bool ok = qmi_flash_read_data(address, buffer, length);

    if (!qmi_direct_mode_stop()) {
        return false;
    }

    return ok;
}

#endif //USE_STUB

// -----------------------------------------------------------------------------
// qmi_flash_test
// -----------------------------------------------------------------------------

int qmi_flash_test(void)
{
    uint8_t flash_data[32] = {0};
    bool flash_read_ok = false;

    printf("========================================\n");
    printf(" RP2350 QMI Direct Mode test\n");
    printf(" W25Q32RV - Single SPI\n");
    printf("========================================\n");

    uint32_t actual_sck_hz = sys_hz / qmi_clkdiv;

    printf("clk_sys       : %lu Hz\n", (unsigned long)sys_hz);
    printf("QMI CLKDIV    : %lu\n", (unsigned long)qmi_clkdiv);
    printf("QMI SCK       : %lu Hz (approx.)\n",
           (unsigned long)actual_sck_hz);

    // QSPIパッド、フラッシュデバイス、QMIを初期化する。
    flash_start_xip();

    // Direct Mode を有効にする。
    if (!qmi_direct_mode_start())
    {
        printf("[ERROR] Failed to enter QMI Direct Mode.\n");
        qmi_dump_registers("Direct Mode start failed");
        while (true)
        {
            sleep_ms(1000);
        }
    }

    // -------------------------------------------------------------------------
    // JEDEC ID
    // -------------------------------------------------------------------------

    uint8_t jedec_id[3] = {0};

    bool jedec_ok = qmi_flash_read_register(CMD_READ_JEDEC_ID, jedec_id, sizeof(jedec_id));

    // -------------------------------------------------------------------------
    // Status Registers
    // -------------------------------------------------------------------------

    uint8_t sr1 = 0;
    uint8_t sr2 = 0;
    uint8_t sr3 = 0;

    bool sr1_ok = qmi_flash_read_register(CMD_READ_SR1, &sr1, 1);
    bool sr2_ok = qmi_flash_read_register(CMD_READ_SR2, &sr2, 1);
    bool sr3_ok = qmi_flash_read_register(CMD_READ_SR3, &sr3, 1);

    /* Read Data: address 0x000000, 32 bytes */
    flash_read_ok = qmi_flash_read_data(0x000000, flash_data, sizeof(flash_data));

    // -------------------------------------------------------------------------
    // Direct Mode 終了
    // -------------------------------------------------------------------------

    bool stop_ok = qmi_direct_mode_stop();

    // ここから printf してよい。
    // Direct Mode 中は XIP メモリアクセスができないため、
    // printf や通常のライブラリ関数を呼ばないこと。

    printf("\n[QMI Transaction Diagnostics]\n");

    for (unsigned int i = 0; i < qmi_diag_count && i < 4; ++i) {
        printf("\n  Command : 0x%02X\n", qmi_diag[i].command);
        printf("    CS asserted : 0x%08lX\n", (unsigned long)qmi_diag[i].csr_cs_assert);
        printf("    Command done: 0x%08lX\n", (unsigned long)qmi_diag[i].csr_command_done);
        printf("    Data done   : 0x%08lX\n", (unsigned long)qmi_diag[i].csr_data_done);
        printf("    CS0 asserted: %s\n", (qmi_diag[i].csr_cs_assert & QMI_DIRECT_CSR_ASSERT_CS0N_BITS) ? "YES" : "NO");
    }

    printf("\n");

    qmi_dump_registers("After Direct Mode");

    printf("\n");
    printf("========================================\n");
    printf(" Read results\n");
    printf("========================================\n");

    // JEDEC ID
    printf("\n[JEDEC ID]\n");

    printf("  Read result : %s\n", jedec_ok ? "OK" : "FAILED");

    if (jedec_ok) {
        printf("  ID          : %02X %02X %02X\n", 
               jedec_id[0],
               jedec_id[1],
               jedec_id[2]);

        bool id_match =
            jedec_id[0] == EXPECTED_JEDEC_ID_0 &&
            jedec_id[1] == EXPECTED_JEDEC_ID_1 &&
            jedec_id[2] == EXPECTED_JEDEC_ID_2;

        printf("  Expected    : EF 40 16\n");
        printf("  Match       : %s\n", id_match ? "YES" : "NO");
    }

    // Status Register 1
    printf("\n[Status Register 1 - 05h]\n");
    printf("  Read result : %s\n", sr1_ok ? "OK" : "FAILED");

    if (sr1_ok) {
        printf("  SR1         : 0x%02X\n", sr1);
        printf("  BUSY        : %u\n", (unsigned int)((sr1 & SR1_BUSY_MASK) != 0));
        printf("  WEL         : %u\n", (unsigned int)((sr1 & SR1_WEL_MASK) != 0));
    }

    // Status Register 2
    printf("\n[Status Register 2 - 35h]\n");
    printf("  Read result : %s\n", sr2_ok ? "OK" : "FAILED");

    if (sr2_ok) {
        printf("  SR2         : 0x%02X\n", sr2);
        printf("  QE          : %u\n", (unsigned int)((sr2 & SR2_QE_MASK) != 0));
    }

    // Status Register 3
    printf("\n[Status Register 3 - 15h]\n");
    printf("  Read result : %s\n", sr3_ok ? "OK" : "FAILED");

    if (sr3_ok) {
        printf("  SR3         : 0x%02X\n", sr3);
    }

    printf("\n[Direct Mode]\n");
    printf("  Stop result : %s\n", stop_ok ? "OK" : "FAILED");

    printf("\n[Read Data - 03h]\n");
    printf("  Address     : 0x000000\n");
    printf("  Length      : %u bytes\n", (unsigned)sizeof(flash_data));
    printf("  Read result : %s\n", flash_read_ok ? "OK" : "FAILED");

    if (flash_read_ok) {
        for (size_t i = 0; i < sizeof(flash_data); i++) {
            if (i % 16 == 0) {
                printf("  %06X      :", (unsigned)i);
            }
            printf(" %02X", flash_data[i]);

            if (i % 16 == 15 || i == sizeof(flash_data) - 1) {
                printf("\n");
            }
        }
    }

    printf("\n========================================\n");
    printf(" Test completed\n");
    printf("========================================\n");
}
