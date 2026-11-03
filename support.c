//
// support.c
//

#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#include "support.h"

#define CRC16_CCITT_FALSE_INIT  0xFFFFu
#define CRC16_CCITT_FALSE_POLY  0x1021u

uint16_t crc16_ccitt_false(const uint8_t *buffer, size_t length)
{
    uint16_t crc = CRC16_CCITT_FALSE_INIT;

    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)buffer[i] << 8;

        for (unsigned bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ CRC16_CCITT_FALSE_POLY);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}

//
// test routine
//

static uint8_t test_buffer[4096];

void test_crc16(void)
{
    // 全ゼロ
    for (size_t i = 0; i < sizeof(test_buffer); i++) {
        test_buffer[i] = 0x00;
    }
    assert(crc16_ccitt_false(test_buffer, sizeof(test_buffer)) == 0xEFDFu);

    // 全 FF
    for (size_t i = 0; i < sizeof(test_buffer); i++) {
        test_buffer[i] = 0xFF;
    }
    assert(crc16_ccitt_false(test_buffer, sizeof(test_buffer)) == 0x0FE1u);

    // 全 A5
    for (size_t i = 0; i < sizeof(test_buffer); i++) {
        test_buffer[i] = 0xA5;
    }
    assert(crc16_ccitt_false(test_buffer, sizeof(test_buffer)) == 0x4EE4u);

    // 00〜FF の繰り返し
    for (size_t i = 0; i < sizeof(test_buffer); i++) {
        test_buffer[i] = (uint8_t)i;
    }
    assert(crc16_ccitt_false(test_buffer, sizeof(test_buffer)) == 0x0F69u);
}

//
// fill_random
//

#define RANDOM_SEED_DEFAULT  0x12345678u

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    *state = x;
    return x;
}

void fill_random(uint8_t *buffer, size_t length, uint32_t seed)
{
    // xorshift32では、状態が0だと永久に0になる
    uint32_t state = seed ? seed : RANDOM_SEED_DEFAULT;

    uint32_t random_value = 0;

    for (size_t i = 0; i < length; i++) {
        // 4バイトごとに乱数を生成
        if ((i & 3u) == 0) {
            random_value = xorshift32(&state);
        }

        // 下位バイトから順に格納
        buffer[i] = (uint8_t)random_value;
        random_value >>= 8;
    }
}

// usage:
//   uint8_t buffer[4096];
// 4096バイトを乱数で埋める
//   fill_random(buffer, sizeof(buffer), 0x12345678u);
// CRCを計算
//   uint16_t crc = crc16_ccitt_false(buffer, sizeof(buffer));

// fill_random(buffer, sizeof(buffer), 0x12345678u);
// fill_random(buffer, sizeof(buffer), 0x87654321u);
// fill_random(buffer, sizeof(buffer), 0xA5A5A5A5u);
// fill_random(buffer, sizeof(buffer), 0xDEADBEEFu);

