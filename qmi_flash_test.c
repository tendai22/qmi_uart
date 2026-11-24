//
// qmi_flash_test.c
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

extern bool qmi_flash_read_data(uint32_t address, uint8_t *buffer, size_t length);

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
