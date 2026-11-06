#pragma once
//
// qmi_flash.h ... 共通定義
//

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// --------------------------------------------
// QMI clock/timeout constant parameters
// --------------------------------------------

// FIFO 待ちのタイムアウト。
// 1 回の待ちループで tight_loop_contents() を実行する。
#define QMI_TIMEOUT_COUNT 10000000u

// エラー処理復旧タイムアウト
#define PROGRAM_ERROR_RECOVERY_TIMEOUT_US 100000u // 100ms (tentative)

// 目標 SCK 周波数。
// clk_sys が 150 MHz の場合、CLKDIV=30 となり約 5 MHz。
#define QMI_TARGET_SCK_HZ 5000000u

// -----------------------------------------------------------------------------
// W25Q32RV commands
// -----------------------------------------------------------------------------

#define CMD_READ_JEDEC_ID 0x9F
#define CMD_READ_SR1 0x05
#define CMD_READ_SR2 0x35
#define CMD_READ_SR3 0x15
#define CMD_READ_DATA 0x03

#define CMD_WRITE_ENABLE 0x06
#define CMD_SECTOR_ERASE_4K 0x20
#define CMD_PAGE_PROGRAM 0x02

#define EXPECTED_JEDEC_ID_0 0xEF
#define EXPECTED_JEDEC_ID_1 0x40
#define EXPECTED_JEDEC_ID_2 0x16

#define SR1_BUSY_MASK 0x01
#define SR1_WEL_MASK 0x02
#define SR2_QE_MASK 0x02

//
//
//
// W25Q32RV の最大処理時間より余裕を持たせたタイムアウト。
// 実際のタイムアウト値はデータシートや実測に応じて調整する。
#define FLASH_ERASE_TIMEOUT_US 5000000u
#define FLASH_PROGRAM_TIMEOUT_US 1000000u
#define FLASH_READ_TIMEOUT_US 100000u

//
// common emum definition
//
typedef enum {
    FLASH_OP_NONE = 0,
    FLASH_OP_READ,
    FLASH_OP_ERASE,
    FLASH_OP_PROGRAM
} flash_operation_t;

typedef enum
{
    QMI_FLASH_POLL_BUSY = 0,
    QMI_FLASH_POLL_DONE,
    QMI_FLASH_POLL_ERROR
} qmi_flash_poll_result_t;

// global variables
extern flash_operation_t flash_operation;

extern uint32_t flash_operation_start_us;
extern uint32_t flash_operation_timeout_us;

// 共通の初期化
extern void qmi_init(void);

// デバッグ用マクロ
#if defined(QMI_DEBUG)
#define TIME_CHECK(str)                                 \
    debug("%s: %ld us\n", (str), time_us_32() - time0); \
    time0 = time_us_32();
#else
#define TIME_CHECK(str)
#endif

// サポート関数
extern void qmi_dump_registers(const char *label);
extern bool qmi_wait_not_busy(void);
extern bool qmi_wait_tx_ready(void);
extern bool qmi_wait_rx_ready(void);
extern bool qmi_direct_mode_start(void);
extern bool qmi_direct_mode_stop(void);
extern bool qmi_flash_read_register(uint8_t command, uint8_t *buffer, size_t length);
extern bool qmi_wait_busy_clear(void);
extern bool qmi_flash_read_data(uint32_t address, uint8_t *buffer, size_t length);
extern bool qmi_flash_write_byte(uint8_t data);
extern bool qmi_flash_write_enable(void);
extern bool qmi_flash_check_range(uint32_t address, size_t length);
extern bool qmi_flash_status_ready(bool *ready);
extern bool qmi_flash_begin_write(void);
extern void qmi_flash_set_operation(flash_operation_t operation, uint32_t timeout_us);
extern bool qmi_flash_wait_ready(uint32_t timeout_us);


