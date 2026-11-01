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

//
// common globals
//
flash_operation_t flash_operation = FLASH_OP_NONE;

uint32_t flash_operation_start_us;
uint32_t flash_operation_timeout_us;



// -----------------------------------------------------------------------------
// QMI Direct Mode
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// QMI wait helpers
// -----------------------------------------------------------------------------

extern bool qmi_wait_not_busy(void) {
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) == 0) {
            return true;
        }
        tight_loop_contents();
    }
    return false;
}

extern bool qmi_wait_tx_ready(void) {
    for (uint32_t i = 0; i < QMI_TIMEOUT_COUNT; ++i) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) == 0) {
            return true;
        }
        tight_loop_contents();
    }
    return false;
}

extern bool qmi_wait_rx_ready(void) {
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
        return false;
    }
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
    return true;
}

// -----------------------------------------------------------------------------
// W25Q32RV register read
// -----------------------------------------------------------------------------

extern bool qmi_flash_read_register(uint8_t command, uint8_t *buffer, size_t length) {
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

extern bool qmi_wait_busy_clear(void) {
    uint32_t start = time_us_32();

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        if ((uint32_t)(time_us_32() - start) > FLASH_READ_TIMEOUT_US) {
            return false;
        }
    }
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
#if defined(USE_STUB)
    qmi_stub_init();
#endif
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
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    return false;
}

bool qmi_flash_check_range(uint32_t address, size_t length) {
    return address < FLASH_SIZE && length <= FLASH_SIZE - address;
}

bool qmi_flash_status_ready(bool *ready) {
    uint8_t sr1 = 0;

    if (ready == NULL)
        return false;

    *ready = false;

    if (!qmi_flash_read_register(CMD_READ_SR1, &sr1, 1))
        return false;

    *ready = ((sr1 & SR1_BUSY_MASK) == 0);
    return true;
}

bool qmi_flash_begin_write(void) {
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

    return false;
}

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
