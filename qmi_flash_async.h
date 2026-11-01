#pragma once
//
// qmi_flash_async.h
//

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//
// qmi_flash_poll_result_t ... async関数pollのリザルトコード
//



// 非同期(コマンド発行/ポーリング分割) API
// Read
extern bool qmi_flash_read_start(uint32_t address, uint8_t *buffer, size_t length);
extern qmi_flash_poll_result_t qmi_flash_read_poll(void);
// Erase
extern bool qmi_flash_erase_4k_start(uint32_t address);
extern qmi_flash_poll_result_t qmi_flash_erase_poll(void);

// Program
extern bool qmi_flash_program_start(uint32_t address, const uint8_t *buffer, size_t length);
qmi_flash_poll_result_t qmi_flash_program_poll(void);

// debug support
bool toggle_debug_error(void);

