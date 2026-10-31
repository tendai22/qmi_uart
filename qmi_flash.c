//
// qmi_flash.c
//

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

// エラー処理復旧タイムアウト
#define PROGRAM_ERROR_RECOVERY_TIMEOUT_US 100000u // 100ms (tentative)

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

static void qmi_dump_registers(const char *label) {
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

static bool qmi_wait_not_busy(void) {
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) == 0) {
            return true;
        }
        tight_loop_contents();
    }
    return false;
}

static bool qmi_wait_tx_ready(void) {
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) == 0) {
            return true;
        }
        tight_loop_contents();
    }
    return false;
}

static bool qmi_wait_rx_ready(void) {
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

static bool qmi_direct_mode_start(void) {
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

static bool qmi_direct_mode_stop(void) {
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

static bool qmi_flash_read_register(uint8_t command, uint8_t *buffer, size_t length) {
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

static bool qmi_wait_busy_clear(void) {
    uint32_t start = time_us_32();

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        if ((uint32_t)(time_us_32() - start) > FLASH_READ_TIMEOUT_US) {
            return false;
        }
    }
    return true;
}

static bool qmi_flash_read_data(uint32_t address, uint8_t *buffer, size_t length) {
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

void qmi_init(void) {
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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// #undef FLASH_SIZE
// #define FLASH_SIZE      0x20000u   // dummy flash size 128kB
// #define SECTOR_SIZE     0x1000u
// #define PAGE_SIZE       0x100u

// 4 MiB pseudo Flash
static uint8_t pseudo_flash[FLASH_SIZE];

// Initialize pseudo Flash to erased state.
void qmi_stub_init(void) {
    memset(pseudo_flash, 0xFF, sizeof(pseudo_flash));
}

// Read
bool qmi_flash_read(uint32_t address, uint8_t *buffer, size_t length) {
    if (address > FLASH_SIZE || length > FLASH_SIZE - address)
        return false;

    memcpy(buffer, &pseudo_flash[address], length);
    return true;
}

// 4 KiB Sector Erase
bool qmi_flash_erase_4k(uint32_t address) {
    if ((address & (SECTOR_SIZE - 1)) != 0)
        return false;

    if (address > FLASH_SIZE - SECTOR_SIZE)
        return false;

    memset(&pseudo_flash[address], 0xFF, SECTOR_SIZE);
    return true;
}

// Page Program
bool qmi_flash_program_page(uint32_t address, const uint8_t *buffer, size_t length) {
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

#else // USE_STUB

// -----------------------------------------------------------------------------
// W25Q32RV write commands
// -----------------------------------------------------------------------------

#define CMD_WRITE_ENABLE 0x06
#define CMD_SECTOR_ERASE_4K 0x20
#define CMD_PAGE_PROGRAM 0x02

// W25Q32RV の最大処理時間より余裕を持たせたタイムアウト。
// 実際のタイムアウト値はデータシートや実測に応じて調整する。
#define FLASH_ERASE_TIMEOUT_US 5000000u
#define FLASH_PROGRAM_TIMEOUT_US 1000000u

//
// read/erase/program async mode
//
typedef enum {
    FLASH_OP_NONE = 0,
    FLASH_OP_READ,
    FLASH_OP_ERASE,
    FLASH_OP_PROGRAM
} flash_operation_t;

static flash_operation_t flash_operation = FLASH_OP_NONE;

static uint32_t flash_operation_start_us;
static uint32_t flash_operation_timeout_us;

// Read の進行状態
static uint8_t *read_buffer;
static size_t read_length;
static size_t read_position;

// -----------------------------------------------------------------------------
// Direct Mode transmit helper
// -----------------------------------------------------------------------------

// 1 byte を送信する。
// 書き込みコマンドでは受信 FIFO にデータを積まないよう NOPUSH を指定する。
static bool qmi_flash_write_byte(uint8_t data) {
    if (!qmi_wait_tx_ready()) {
        return false;
    }

    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | data;

    return qmi_wait_not_busy();
}

// -----------------------------------------------------------------------------
// Write Enable
// -----------------------------------------------------------------------------

static bool qmi_flash_write_enable(void) {
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

static bool qmi_flash_check_range(uint32_t address, size_t length) {
    return address < FLASH_SIZE && length <= FLASH_SIZE - address;
}

static bool qmi_flash_status_ready(bool *ready) {
    uint8_t sr1 = 0;

    if (ready == NULL)
        return false;

    *ready = false;

    if (!qmi_flash_read_register(CMD_READ_SR1, &sr1, 1))
        return false;

    *ready = ((sr1 & SR1_BUSY_MASK) == 0);
    return true;
}

static bool qmi_flash_begin_write(void) {
    bool ready = false;

    flash_start_xip();

    if (!qmi_direct_mode_start())
        return false;

    if (!qmi_flash_status_ready(&ready) || !ready) {
        (void)qmi_direct_mode_stop();
        return false;
    }

    if (!qmi_flash_write_enable()) {
        (void)qmi_direct_mode_stop();
        return false;
    }

    return true;
}

static void qmi_flash_set_operation(flash_operation_t operation, uint32_t timeout_us) {
    flash_operation = operation;
    flash_operation_start_us = time_us_32();
    flash_operation_timeout_us = timeout_us;
}

// -----------------------------------------------------------------------------
// Wait until WIP/BUSY clears
// -----------------------------------------------------------------------------

static bool qmi_flash_wait_ready(uint32_t timeout_us) {
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

// sync 4k erase
bool qmi_flash_erase_4k(uint32_t address) {
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

// async 4k erase command issue
bool qmi_flash_erase_4k_start(uint32_t address) {
    if ((address & (SECTOR_SIZE - 1u)) != 0)
        return false;

    if (!qmi_flash_check_range(address, SECTOR_SIZE))
        return false;

    if (flash_operation != FLASH_OP_NONE)
        return false;

    if (!qmi_flash_begin_write())
        return false;

    // 20h + 24-bit address
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_SECTOR_ERASE_4K) ||
        !qmi_flash_write_byte((uint8_t)(address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)address) ||
        !qmi_wait_not_busy()) {

        (void)qmi_wait_not_busy();
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        (void)qmi_direct_mode_stop();
        return false;
    }

    // CS High で Erase を開始
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_direct_mode_stop())
        return false;

    qmi_flash_set_operation(
        FLASH_OP_ERASE,
        FLASH_ERASE_TIMEOUT_US);

    return true;
}

// poll 4k erase command completion
qmi_flash_poll_result_t qmi_flash_erase_poll(void) {
    bool ready = false;

    if (flash_operation != FLASH_OP_ERASE)
        return QMI_FLASH_POLL_ERROR;

    if ((uint32_t)(time_us_32() - flash_operation_start_us) >= flash_operation_timeout_us) {
        flash_operation = FLASH_OP_NONE;
        return QMI_FLASH_POLL_ERROR;
    }

    flash_start_xip();

    if (!qmi_direct_mode_start())
        return QMI_FLASH_POLL_ERROR;

    bool read_ok = qmi_flash_status_ready(&ready);
    bool stop_ok = qmi_direct_mode_stop();

    if (!read_ok || !stop_ok) {
        flash_operation = FLASH_OP_NONE;
        return QMI_FLASH_POLL_ERROR;
    }

    if (!ready)
        return QMI_FLASH_POLL_BUSY;

    flash_operation = FLASH_OP_NONE;
    return QMI_FLASH_POLL_DONE;
}

// -----------------------------------------------------------------------------
// Page Program
// -----------------------------------------------------------------------------

//
// async-poll version
//

//
// page-program status
//
typedef enum {
    PROGRAM_STATE_IDLE,

    // WREN
    PROGRAM_STATE_WREN_BEGIN,
    PROGRAM_STATE_WREN_SEND,
    PROGRAM_STATE_WREN_WAIT,

    // Page Program
    PROGRAM_STATE_PP_BEGIN,
    PROGRAM_STATE_PP_SEND_CMD,
    PROGRAM_STATE_PP_SEND_ADDR_H,
    PROGRAM_STATE_PP_SEND_ADDR_M,
    PROGRAM_STATE_PP_SEND_ADDR_L,
    PROGRAM_STATE_PP_SEND_DATA,
    PROGRAM_STATE_PP_WAIT,
    PROGRAM_STATE_PP_WAIT_BYTE,

    // Status Register 1
    PROGRAM_STATE_STATUS_BEGIN,
    PROGRAM_STATE_STATUS_SEND_CMD,
    PROGRAM_STATE_STATUS_WAIT_CMD,
    PROGRAM_STATE_STATUS_SEND_DUMMY,
    PROGRAM_STATE_STATUS_WAIT_RX,
    PROGRAM_STATE_STATUS_WAIT_END,

    PROGRAM_STATE_DONE,

    // Error cleanup
    PROGRAM_STATE_ERROR,
    PROGRAM_STATE_ERROR_WAIT,
    PROGRAM_STATE_ERROR_END,
    PROGRAM_STATE_ERROR_FATAL

} program_state_t;

static program_state_t program_state = PROGRAM_STATE_IDLE;

static const char *program_state_name(program_state_t state) {
    switch (state) {
    case PROGRAM_STATE_IDLE:
        return "IDLE";
    case PROGRAM_STATE_WREN_BEGIN:
        return "WREN_BEGIN";
    case PROGRAM_STATE_WREN_SEND:
        return "WREN_SEND";
    case PROGRAM_STATE_WREN_WAIT:
        return "WREN_WAIT";
    case PROGRAM_STATE_PP_BEGIN:
        return "PP_BEGIN";
    case PROGRAM_STATE_PP_SEND_CMD:
        return "PP_SEND_CMD";
    case PROGRAM_STATE_PP_SEND_ADDR_H:
        return "PP_SEND_ADDR_H";
    case PROGRAM_STATE_PP_SEND_ADDR_M:
        return "PP_SEND_ADDR_M";
    case PROGRAM_STATE_PP_SEND_ADDR_L:
        return "PP_SEND_ADDR_L";
    case PROGRAM_STATE_PP_SEND_DATA:
        return "PP_SEND_DATA";
    case PROGRAM_STATE_PP_WAIT:
        return "PP_WAIT";
    case PROGRAM_STATE_PP_WAIT_BYTE:
        return "PP_WAIT_BYTE";
    case PROGRAM_STATE_STATUS_BEGIN:
        return "STATUS_BEGIN";
    case PROGRAM_STATE_STATUS_SEND_CMD:
        return "STATUS_SEND_CMD";
    case PROGRAM_STATE_STATUS_WAIT_CMD:
        return "STATUS_WAIT_CMD";
    // case PROGRAM_STATE_STATUS_WAIT_DATA:
    //     return "STATUS_WAIT_DATA";
    case PROGRAM_STATE_STATUS_SEND_DUMMY:
        return "STATUS_SEND_DUMMY";
    case PROGRAM_STATE_STATUS_WAIT_RX:
        return "STATUS_WAIT_RX";
    case PROGRAM_STATE_STATUS_WAIT_END:
        return "STATUS_WAIT_END";
    case PROGRAM_STATE_DONE:
        return "DONE";
    case PROGRAM_STATE_ERROR:
        return "ERROR";
    case PROGRAM_STATE_ERROR_WAIT:
        return "ERROR_WAIT";
    case PROGRAM_STATE_ERROR_END:
        return "ERROR_END";
    case PROGRAM_STATE_ERROR_FATAL:
        return "ERROR_FATAL";
    default:
        return "UNKNOWN";
    }
}

#ifdef QMI_DEBUG
#define PROGRAM_SET_STATE(next)                   \
    do {                                          \
        printf("[PROGRAM] %s -> %s\n",            \
               program_state_name(program.state), \
               program_state_name(next));         \
        program.state = (next);                   \
    } while (0)
#else
#define PROGRAM_SET_STATE(next) \
    do {                        \
        program.state = (next); \
    } while (0)
#endif

//
// program globals
//
typedef struct {
    program_state_t state;
    uint32_t address;
    const uint8_t *buffer;
    size_t length;
    size_t position;
    uint8_t sr1;
    uint32_t direct_csr;
    // error handling
    bool direct_mode_active;
    bool cs_asserted;
    uint32_t error_start_us;
} program_context_t;

static program_context_t program;
static program_context_t debug_program;
//
// debug error
//
static void program_debug_error(const char *reason) {
    printf("[PROGRAM] ERROR: %s\n", reason);
    printf("  state      = %s\n", program_state_name(program.state));
    printf("  direct_csr = 0x%08lx\n", (unsigned long)qmi_hw->direct_csr);
    printf("  direct_rx  = 0x%08lx\n", (unsigned long)qmi_hw->direct_rx);
    printf("  position   = %lu / %lu\n", (unsigned long)program.position, (unsigned long)program.length);
    printf("  SR1        = 0x%02x\n", program.sr1);
}

//
// program_start
//
bool qmi_flash_program_start(
    uint32_t address,
    const uint8_t *buffer,
    size_t length) {
    if (buffer == NULL || length == 0)
        return false;

    if (!qmi_flash_check_range(address, length))
        return false;

    if (flash_operation != FLASH_OP_NONE)
        return false;

    // 1ページを超えないことを確認
    if ((address / PAGE_SIZE) !=
        ((address + length - 1) / PAGE_SIZE))
        return false;

    program.address = address;
    program.buffer = buffer;
    program.length = length;
    program.position = 0;
    program.direct_mode_active = false;
    program.cs_asserted = false;

    flash_operation_start_us = time_us_32();

    qmi_flash_set_operation(FLASH_OP_PROGRAM, FLASH_PROGRAM_TIMEOUT_US);
    PROGRAM_SET_STATE(PROGRAM_STATE_WREN_BEGIN);

    return true;
}

//
// qmi_flash_program_poll
//
qmi_flash_poll_result_t qmi_flash_program_poll(void) {
    if (flash_operation != FLASH_OP_PROGRAM)
        return QMI_FLASH_POLL_ERROR;

    if (program.state != PROGRAM_STATE_ERROR &&
        program.state != PROGRAM_STATE_ERROR_WAIT &&
        program.state != PROGRAM_STATE_ERROR_END &&
        program.state != PROGRAM_STATE_ERROR_FATAL) {
        // 通常タイムアウト処理。エラー復旧処理中には適用しない。
        uint32_t elapsed_us = (uint32_t)(time_us_32() - flash_operation_start_us);

        if (elapsed_us >= flash_operation_timeout_us) {
            printf("[PROGRAM] TIMEOUT: elapsed=%lu us, limit=%lu us\n",
                   (unsigned long)elapsed_us,
                   (unsigned long)flash_operation_timeout_us);
            printf("debug_direct_csr: %08lx\n", debug_program.direct_csr);
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
        }
    }

    switch (program.state) {

    case PROGRAM_STATE_WREN_BEGIN:
        /*
         * QMI Direct Mode開始。
         * CSをLowにして06hを送信する。
         */
        if (!qmi_direct_mode_start())
            goto error;

        program.direct_mode_active = true;
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        program.cs_asserted = true;
        PROGRAM_SET_STATE(PROGRAM_STATE_WREN_SEND);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_WREN_SEND:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) {
            return QMI_FLASH_POLL_BUSY;
        }

        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_WRITE_ENABLE;

        PROGRAM_SET_STATE(PROGRAM_STATE_WREN_WAIT);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_WREN_WAIT:
        /*
         * 06hのQMI転送完了待ち。
         */
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
            return QMI_FLASH_POLL_BUSY;

        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        program.cs_asserted = false;

        if (!qmi_direct_mode_stop())
            goto error;
        program.direct_mode_active = false;

        PROGRAM_SET_STATE(PROGRAM_STATE_PP_BEGIN);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_BEGIN:
        /*
         * Page Programトランザクションを開始する。
         */
        if (!qmi_direct_mode_start())
            goto error;

        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

        PROGRAM_SET_STATE(PROGRAM_STATE_PP_SEND_CMD);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_SEND_CMD:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            return QMI_FLASH_POLL_BUSY;

        qmi_hw->direct_tx =
            QMI_DIRECT_TX_NOPUSH_BITS | CMD_PAGE_PROGRAM;

        PROGRAM_SET_STATE(PROGRAM_STATE_PP_SEND_ADDR_H);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_SEND_ADDR_H:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            return QMI_FLASH_POLL_BUSY;

        qmi_hw->direct_tx =
            QMI_DIRECT_TX_NOPUSH_BITS |
            (uint8_t)(program.address >> 16);

        PROGRAM_SET_STATE(PROGRAM_STATE_PP_SEND_ADDR_M);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_SEND_ADDR_M:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            return QMI_FLASH_POLL_BUSY;

        qmi_hw->direct_tx =
            QMI_DIRECT_TX_NOPUSH_BITS |
            (uint8_t)(program.address >> 8);

        PROGRAM_SET_STATE(PROGRAM_STATE_PP_SEND_ADDR_L);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_SEND_ADDR_L:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            return QMI_FLASH_POLL_BUSY;

        qmi_hw->direct_tx =
            QMI_DIRECT_TX_NOPUSH_BITS |
            (uint8_t)program.address;

        program.position = 0;

        PROGRAM_SET_STATE(PROGRAM_STATE_PP_SEND_DATA);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_SEND_DATA:
        /*
         * 1回のpollで最大1バイト送信する。
         */
        if (program.position >= program.length) {
            PROGRAM_SET_STATE(PROGRAM_STATE_PP_WAIT);
            return QMI_FLASH_POLL_BUSY;
        }

        debug_program.position = program.position;
        debug_program.direct_csr = qmi_hw->direct_csr;
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) {
            return QMI_FLASH_POLL_BUSY;
        }

        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | program.buffer[program.position];
        ++program.position;

        // PROGRAM_SET_STATE(PROGRAM_STATE_PP_WAIT_BYTE);
        program.state = PROGRAM_STATE_PP_WAIT_BYTE;
        return QMI_FLASH_POLL_BUSY;
    case PROGRAM_STATE_PP_WAIT_BYTE:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
            return QMI_FLASH_POLL_BUSY;

        if (program.position >= program.length) {
            PROGRAM_SET_STATE(PROGRAM_STATE_PP_WAIT);
        } else {
            // PROGRAM_SET_STATE(PROGRAM_STATE_PP_SEND_DATA);
            program.state = PROGRAM_STATE_PP_SEND_DATA;
        }
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_WAIT:
        /*
         * QMIの送信完了を待つ。
         * ここではFlash内部のProgram完了はまだ待たない。
         */
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
            return QMI_FLASH_POLL_BUSY;

        // CS HighでPage Programを確定させる。
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

        if (!qmi_direct_mode_stop())
            goto error;
        program.direct_mode_active = false;

        PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_BEGIN);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_STATUS_BEGIN:
        if (!qmi_direct_mode_start())
            goto error;

        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_SEND_CMD);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_STATUS_SEND_CMD:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            return QMI_FLASH_POLL_BUSY;

        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_READ_SR1;

        PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_WAIT_CMD);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_STATUS_WAIT_CMD:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
            return QMI_FLASH_POLL_BUSY;

        PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_SEND_DUMMY);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_STATUS_SEND_DUMMY:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            return QMI_FLASH_POLL_BUSY;

        qmi_hw->direct_tx = 0xFF;

        PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_WAIT_RX);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_STATUS_WAIT_RX:
        debug_program.direct_csr = qmi_hw->direct_csr;
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)
            return QMI_FLASH_POLL_BUSY;

        program.sr1 = (uint8_t)qmi_hw->direct_rx;

        PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_WAIT_END);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_STATUS_WAIT_END:
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
            return QMI_FLASH_POLL_BUSY;

        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

        if (!qmi_direct_mode_stop())
            goto error;
        program.direct_mode_active = false;

        if (program.sr1 & SR1_BUSY_MASK) {
            PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_BEGIN);
            return QMI_FLASH_POLL_BUSY;
        }

        PROGRAM_SET_STATE(PROGRAM_STATE_DONE);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_DONE:
        flash_operation = FLASH_OP_NONE;
        PROGRAM_SET_STATE(PROGRAM_STATE_IDLE);
        return QMI_FLASH_POLL_DONE;
#if 0
    case PROGRAM_STATE_STATUS_WAIT_DATA:
        /*
         * SR1を受信するためのクロックを発生させる。
         */
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS)
            return QMI_FLASH_POLL_BUSY;

        qmi_hw->direct_tx = 0xFF;

        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)
            return QMI_FLASH_POLL_BUSY;

        program.sr1 = (uint8_t)qmi_hw->direct_rx;

        PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_WAIT_END);
        return QMI_FLASH_POLL_BUSY;
#endif
    case PROGRAM_STATE_ERROR:
        /*
         * エラー復旧開始。
         * ここではまだCSやDirect Modeを操作しない。
         */
        program.error_start_us = time_us_32();

        PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_WAIT);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_ERROR_WAIT:
        /*
         * QMIの転送完了を待つ。
         */
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
            uint32_t elapsed_us = (uint32_t)(time_us_32() - program.error_start_us);

            if (elapsed_us >= PROGRAM_ERROR_RECOVERY_TIMEOUT_US) {
                program_debug_error("Recovery timeout");
                PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_FATAL);
                return QMI_FLASH_POLL_ERROR;
            }
            return QMI_FLASH_POLL_BUSY;
        }
        PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_END);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_ERROR_END:
        /*
         * QMI BUSY解除後にCSをHighにする。
         */
        if (program.cs_asserted) {
            qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
            program.cs_asserted = false;
        }

        /*
         * Direct Modeを終了する。
         */
        if (program.direct_mode_active) {
            if (!qmi_direct_mode_stop()) {
                program_debug_error("Direct Mode stop failed");
                PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_FATAL);
                return QMI_FLASH_POLL_ERROR;
            }
            program.direct_mode_active = false;
        }

        /*
         * 復旧完了。
         */
        flash_operation = FLASH_OP_NONE;

        PROGRAM_SET_STATE(PROGRAM_STATE_IDLE);
        return QMI_FLASH_POLL_ERROR;

    case PROGRAM_STATE_ERROR_FATAL:
        /*
         * ハードウェア状態を安全に復旧できなかった。
         * 次のFlash操作を許可しない。
         */
        return QMI_FLASH_POLL_ERROR;
    default:
        goto error;
    }

error:
    /*
     * エラー処理も本来はQMI BUSY状態を確認してから
     * CS解除・Direct Mode終了する必要がある。
     * ここでは概略のみ。
     */
    program_debug_error("QMI operation failed");
    PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
    return QMI_FLASH_POLL_BUSY;
}

//
// sync program
//
bool qmi_flash_program_page(uint32_t address, const uint8_t *buffer, size_t length) {
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
    if (!qmi_flash_wait_ready(FLASH_PROGRAM_TIMEOUT_US))
        goto done;

    ok = true;
    goto done;

program_error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

done:
    if (!qmi_direct_mode_stop())
        return false;

    return ok;
}

// async program issue
bool qmi_flash_program_page_start(uint32_t address, const uint8_t *buffer, size_t length) {
    if (buffer == NULL || length == 0 || length > PAGE_SIZE)
        return false;

    if ((address / PAGE_SIZE) != ((address + length - 1u) / PAGE_SIZE))
        return false;

    if (!qmi_flash_check_range(address, length))
        return false;

    if (flash_operation != FLASH_OP_NONE)
        return false;

    if (!qmi_flash_begin_write())
        return false;

    // 02h + 24-bit address + data
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_PAGE_PROGRAM) ||
        !qmi_flash_write_byte((uint8_t)(address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)address)) {
        goto error;
    }

    for (size_t i = 0; i < length; ++i) {
        if (!qmi_flash_write_byte(buffer[i]))
            goto error;
    }

    if (!qmi_wait_not_busy())
        goto error;

    // CS High で Program を開始
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_direct_mode_stop())
        return false;

    qmi_flash_set_operation(FLASH_OP_PROGRAM, FLASH_PROGRAM_TIMEOUT_US);

    return true;

error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    (void)qmi_direct_mode_stop();
    return false;
}

// async program poll

// -----------------------------------------------------------------------------
// Read Data
// -----------------------------------------------------------------------------

// sync function
bool qmi_flash_read(uint32_t address, uint8_t *buffer, size_t length) {
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

// async read
bool qmi_flash_read_start(uint32_t address, uint8_t *buffer, size_t length) {
    if (buffer == NULL || length == 0)
        return false;

    if (!qmi_flash_check_range(address, length))
        return false;

    if (flash_operation != FLASH_OP_NONE)
        return false;

    flash_start_xip();

    if (!qmi_direct_mode_start())
        return false;

    // 残留 RX データを破棄
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) == 0)
        (void)qmi_hw->direct_rx;

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // Read Data: 03h + 24-bit address
    if (!qmi_flash_write_byte(FLASH_CMD_READ_DATA) ||
        !qmi_flash_write_byte((uint8_t)(address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)address)) {

        (void)qmi_wait_not_busy();
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        (void)qmi_direct_mode_stop();
        return false;
    }

    // 最初のデータ受信用クロックを発生させる
    if (!qmi_wait_tx_ready()) {
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        (void)qmi_direct_mode_stop();
        return false;
    }

    qmi_hw->direct_tx = 0xFF;

    read_buffer = buffer;
    read_length = length;
    read_position = 0;

    flash_operation = FLASH_OP_READ;
    return true;
}

qmi_flash_poll_result_t qmi_flash_read_poll(void) {
    if (flash_operation != FLASH_OP_READ)
        return QMI_FLASH_POLL_ERROR;

    // 受信 FIFO にデータがなければ、まだ処理中
    if (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)
        return QMI_FLASH_POLL_BUSY;

    read_buffer[read_position++] = (uint8_t)qmi_hw->direct_rx;

    if (read_position < read_length) {
        // 次の1バイト分のクロックを発生させる
        if (!qmi_wait_tx_ready())
            goto error;

        qmi_hw->direct_tx = 0xFF;
        return QMI_FLASH_POLL_BUSY;
    }

    // 最後のデータ受信後、転送完了を待って CS を解除
    if (!qmi_wait_not_busy())
        goto error;

    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_direct_mode_stop())
        goto error;

    flash_operation = FLASH_OP_NONE;
    return QMI_FLASH_POLL_DONE;

error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    (void)qmi_direct_mode_stop();

    flash_operation = FLASH_OP_NONE;
    return QMI_FLASH_POLL_ERROR;
}

#endif // USE_STUB

// -----------------------------------------------------------------------------
// qmi_flash_test
// -----------------------------------------------------------------------------

int qmi_flash_test(void) {
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
    if (!qmi_direct_mode_start()) {
        printf("[ERROR] Failed to enter QMI Direct Mode.\n");
        qmi_dump_registers("Direct Mode start failed");
        while (true) {
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
