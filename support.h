#pragma once
//
// support.h
//

#include <stdint.h>

// usage:
//   uint16_t crc = crc16_ccitt_false(buffer, 4096);
// test:
//   static const uint8_t test1[] = "123456789";
//   uint16_t crc = crc16_ccitt_false(test1, 9);
//   期待値: 0x29B1
//
extern uint16_t crc16_ccitt_false(const uint8_t *buffer, size_t length);
extern void test_crc16(void);

//
//
//
void fill_random(uint8_t *buffer, size_t length, uint32_t seed);