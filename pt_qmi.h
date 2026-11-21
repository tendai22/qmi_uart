#pragma once
//
// pt_qmi.h
//

//
// start/poll command enums
//

#define CMD_ERASE 1
#define CMD_READ 2
#define CMD_PROGRAM 3

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
// W25Q32RV の最大処理時間より余裕を持たせたタイムアウト。
// 実際のタイムアウト値はデータシートや実測に応じて調整する。

#define FLASH_ERASE_TIMEOUT_US 5000000u
#define FLASH_PROGRAM_TIMEOUT_US 1000000u
#define FLASH_READ_TIMEOUT_US 100000u

//
// common service function
//
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
//extern void qmi_flash_set_operation(flash_operation_t operation, uint32_t timeout_us);
extern bool qmi_flash_wait_ready(uint32_t timeout_us);

//
// poll and start
//
extern bool pt_issue_erase(uint32_t address);
extern bool pt_issue_program(uint32_t address, uint8_t *buffer, uint32_t length);
extern bool pt_is_done(void);
extern bool pt_issue_ready(void);
extern int  pt_error_code(void);
//extern void pt_qmi_flash_erase_4k_poll(void);
//extern void pt_qmi_flash_program_page_poll(void);
extern void pt_qmi_flash_poll(void);
extern void pt_dump_polltime(void);
