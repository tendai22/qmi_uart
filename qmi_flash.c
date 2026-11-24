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
#include "memprt.h"

#define QMI_DEBUG

#include "qmi_flash.h"

#if defined(USE_STUB)
extern void qmi_stub_init(void);
#endif

//
//
//
static uint32_t sys_hz;
static uint32_t qmi_clkdiv;

//
//
//
typedef struct
{
    uint8_t command;
    uint32_t csr_cs_assert;
    uint32_t csr_command_done;
    uint32_t csr_data_done;
} qmi_diag_t;

static qmi_diag_t qmi_diag[4];
static unsigned int qmi_diag_count = 0;

//
// error code
//
static int qmi_lower_error = 0;

//
// common globals
//
flash_operation_t flash_operation = FLASH_OP_NONE;

uint32_t flash_operation_start_us;
uint32_t flash_operation_timeout_us;



// -----------------------------------------------------------------------------
// QMI diagnostic
// -----------------------------------------------------------------------------

extern void qmi_dump_registers(const char *label) {
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

//
// error code
//
int qmi_error(void) {
    return qmi_lower_error;
}

// -----------------------------------------------------------------------------
// QMI wait helpers
// -----------------------------------------------------------------------------

// return value:
// true:  normal done
// talse: error timeout
extern bool qmi_wait_not_busy(void) {
    uint32_t time0 = time_us_32();
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) == 0) {
            qmi_lower_error = 0;
            return true;
        }
        tight_loop_contents();
    }
    debug("not_busy timeout: %ld\n", (unsigned long)(time_us_32() - time0));
    qmi_lower_error = QMI_ERR_TIMEOUT;
    return false;
}

extern bool qmi_wait_tx_ready(void) {
    uint32_t time0 = time_us_32();
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) == 0) {
            qmi_lower_error = 0;
            return true;
        }
        tight_loop_contents();
    }
    debug("tx_ready timeout: %ld\n", (unsigned long)(time_us_32() - time0));
    qmi_lower_error = QMI_ERR_TIMEOUT;
    return false;
}

extern bool qmi_wait_rx_ready(void) {
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) == 0) {
            qmi_lower_error = 0;
            return true;
        }
        tight_loop_contents();
    }
    qmi_lower_error = QMI_ERR_TIMEOUT;
    return false;
}

// -----------------------------------------------------------------------------
// QMI Direct Mode start / stop
// -----------------------------------------------------------------------------

extern bool qmi_direct_mode_start(void) {
    // Direct Mode を有効にする。
    //
    // AUTO_CS0N は使わず、ASSERT_CS0N によって CS を明示的に制御する。
    // CLKDIV は clk_sys に応じて計算した値を設定する。

    uint32_t csr = ((qmi_clkdiv & 0xFFu) << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    qmi_hw->direct_csr = csr;

    // Direct Mode 有効化時点ですでに進行中だった XIP 転送が
    // 完了するのを待つ。
    if (!qmi_wait_not_busy()) {
        qmi_lower_error = QMI_ERR_TIMEOUT;
        return false;
    }
    qmi_lower_error = 0;
    return true;
}

extern bool qmi_direct_mode_stop(void) {
    // 転送完了を待つ。
    if (!qmi_wait_not_busy()) {
        return false;
    }
    // CS0 を High に戻す（ASSERT_CS0N をクリア）。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    // Direct Mode を解除する。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
    qmi_lower_error = 0;
    return true;
}

// -----------------------------------------------------------------------------
// W25Q32RV register read
// -----------------------------------------------------------------------------

extern bool qmi_flash_read_register(uint8_t command, uint8_t *buffer, size_t length) {
    uint32_t time0 = time_us_32();
    if (buffer == NULL || length == 0) {
        qmi_lower_error = QMI_ERR_PARAM;
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
        qmi_lower_error = QMI_ERR_INTERNAL;
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
    int saved_error = qmi_error();
    (void)qmi_wait_not_busy();
    qmi_lower_error = saved_error;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    return false;
}

extern bool qmi_wait_busy_clear(void) {
    uint32_t start = time_us_32();

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        if ((uint32_t)(time_us_32() - start) > FLASH_READ_TIMEOUT_US) {
            qmi_lower_error = QMI_ERR_TIMEOUT;
            return false;
        }
    }
    qmi_lower_error = 0;
    return true;
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
    flash_start_xip();
#if defined(USE_STUB)
    qmi_stub_init();
#endif
    qmi_lower_error = 0;
}

// -----------------------------------------------------------------------------
// Direct Mode transmit helper
// -----------------------------------------------------------------------------

// 1 byte を送信する。
// 書き込みコマンドでは受信 FIFO にデータを積まないよう NOPUSH を指定する。
bool qmi_flash_write_byte(uint8_t data) {
    if (!qmi_wait_tx_ready()) {
        return false;
    }

    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | data;

    return qmi_wait_not_busy();
}

// -----------------------------------------------------------------------------
// Write Enable
// -----------------------------------------------------------------------------

bool qmi_flash_write_enable(void) {
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
    int saved_error = qmi_error();
    (void)qmi_wait_not_busy();
    qmi_lower_error = saved_error;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    return false;
}

bool qmi_flash_check_range(uint32_t address, size_t length) {
    return address < FLASH_SIZE && length <= FLASH_SIZE - address;
}

bool qmi_flash_status_ready(void) {
    uint8_t sr1 = 0;
    qmi_lower_error = 0;
    if (!qmi_flash_read_register(CMD_READ_SR1, &sr1, 1)) {
        // read_register が false で戻るのは、デバイスステータス
        // 読み出しエラーを現わす。status_ready 関数は
        // 上位層が SR1 BUSY が解除されるまで待つ際の判定関数として
        // 呼び出されるので、false, false, ..., 最後に true を返して
        // 来ることを期待している。異常終了の場合も
        // 上位層では待ちを終えるべきなので、true を返して、qmi_error()
        // 呼び出しでエラーかどうかを判定する。
        return true;
    }

    // SR1 BUSY の間は false, BUSYでなくなると true を返す。
    return (sr1 & SR1_BUSY_MASK) == 0;
}

bool qmi_flash_begin_write(void) {

    flash_start_xip();

    if (!qmi_direct_mode_start())
        return false;

    if (!qmi_flash_wait_ready(FLASH_ERASE_TIMEOUT_US))
        goto error;

    if (!qmi_flash_write_enable()) {
        goto error;
    }
    return true;
error:
    int saved_error = qmi_error();
    (void)qmi_direct_mode_stop();
    qmi_lower_error = saved_error;
    return false;
}

void qmi_flash_set_operation(flash_operation_t operation, uint32_t timeout_us) {
    flash_operation = operation;
    flash_operation_start_us = time_us_32();
    flash_operation_timeout_us = timeout_us;
}

// -----------------------------------------------------------------------------
// Wait until WIP/BUSY clears
// -----------------------------------------------------------------------------

bool qmi_flash_wait_ready(uint32_t timeout_us) {
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
    // timeout occurs
    qmi_lower_error = QMI_ERR_TIMEOUT;
    return false;
}
