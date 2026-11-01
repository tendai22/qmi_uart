#pragma once
//
// qmi_flash_sync.h
//

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// 同期 API
extern bool qmi_flash_read(uint32_t address, uint8_t *buffer, size_t length);
extern bool qmi_flash_erase_4k(uint32_t address);
extern bool qmi_flash_program_page(uint32_t address, const uint8_t *buffer, size_t length);
