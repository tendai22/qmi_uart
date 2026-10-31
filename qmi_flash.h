#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum
{
    QMI_FLASH_POLL_BUSY = 0,
    QMI_FLASH_POLL_DONE,
    QMI_FLASH_POLL_ERROR
} qmi_flash_poll_result_t;

// 共通の初期化
extern void qmi_init(void);

// 非同期(コマンド発行/ポーリング分割) API
// Read
extern bool qmi_flash_read_start(uint32_t address, uint8_t *buffer, size_t length);
extern qmi_flash_poll_result_t qmi_flash_read_poll(void);
// Erase
extern bool qmi_flash_erase_4k_start(uint32_t address);
extern qmi_flash_poll_result_t qmi_flash_erase_poll(void);

// Program
extern bool qmi_flash_program_page_start(uint32_t address, const uint8_t *buffer, size_t length);
qmi_flash_poll_result_t qmi_flash_program_poll(void);

// 既存の同期 API を残す場合
extern bool qmi_flash_read(uint32_t address, uint8_t *buffer, size_t length);
extern bool qmi_flash_erase_4k(uint32_t address);
extern bool qmi_flash_program_page(uint32_t address, const uint8_t *buffer, size_t length);

