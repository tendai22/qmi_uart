//
// qmi_flash_sync.c
//
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "pico/stdlib.h"
#include "pico/time.h"


#include "conf.h"
#include "qmi_flash.h"
#include "qmi_flash_sync.h"


//
// sync flash command functions
//
#if defined(USE_STUB)

// #undef FLASH_SIZE
// #define FLASH_SIZE      0x20000u   // dummy flash size 128kB
// #define SECTOR_SIZE     0x1000u
// #define PAGE_SIZE       0x100u

// 4 MiB pseudo Flash
static uint8_t pseudo_flash[FLASH_SIZE];

// Initialize pseudo Flash to erased state.
void qmi_stub_init(void) {
    memset(pseudo_flash, 0xFF, sizeof(pseudo_flash));
}

// Read
bool qmi_flash_read(uint32_t address, uint8_t *buffer, size_t length) {
    if (address > FLASH_SIZE || length > FLASH_SIZE - address)
        return false;

    memcpy(buffer, &pseudo_flash[address], length);
    return true;
}

// 4 KiB Sector Erase
bool qmi_flash_erase_4k(uint32_t address) {
    if ((address & (SECTOR_SIZE - 1)) != 0)
        return false;

    if (address > FLASH_SIZE - SECTOR_SIZE)
        return false;

    memset(&pseudo_flash[address], 0xFF, SECTOR_SIZE);
    return true;
}

// Page Program
bool qmi_flash_program_page(uint32_t address, const uint8_t *buffer, size_t length) {
    size_t i;

    if (length == 0 || length > PAGE_SIZE)
        return false;

    if (address / PAGE_SIZE != (address + length - 1) / PAGE_SIZE)
        return false;

    if (address > FLASH_SIZE || length > FLASH_SIZE - address)
        return false;

    // NOR Flash: programming can only change 1 -> 0.
    for (i = 0; i < length; i++) {
        if ((pseudo_flash[address + i] & buffer[i]) != buffer[i]) {
            return false;
        }
    }
    for (i = 0; i < length; i++) {
        pseudo_flash[address + i] &= buffer[i];
    }
    return true;
}


#else // USE_STUB

// -----------------------------------------------------------------------------
// Sector Erase 4 KiB
// -----------------------------------------------------------------------------

// sync 4k erase
bool qmi_flash_erase_4k(uint32_t address) {
    // 4 KiB セクタ境界チェック
    if ((address & (SECTOR_SIZE - 1u)) != 0) {
        return false;
    }

    // Flash 範囲チェック
    if (address >= FLASH_SIZE ||
        SECTOR_SIZE > FLASH_SIZE - address) {
        return false;
    }

    // QMI Direct Mode に入る前に XIP を初期化する。
    flash_start_xip();

    if (!qmi_direct_mode_start()) {
        return false;
    }

    bool ok = false;

    // Flash が前の処理を実行中でないことを確認する。
    if (!qmi_flash_wait_ready(FLASH_PROGRAM_TIMEOUT_US)) {
        goto done;
    }

    // WREN
    if (!qmi_flash_write_enable()) {
        goto done;
    }

    // Sector Erase: 20h + 24-bit address
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_SECTOR_ERASE_4K)) {
        goto erase_error;
    }

    if (!qmi_flash_write_byte((uint8_t)(address >> 16))) {
        goto erase_error;
    }

    if (!qmi_flash_write_byte((uint8_t)(address >> 8))) {
        goto erase_error;
    }

    if (!qmi_flash_write_byte((uint8_t)address)) {
        goto erase_error;
    }

    if (!qmi_wait_not_busy()) {
        goto erase_error;
    }

    // CS を High にして Erase を開始させる。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // Erase 完了待ち
    if (!qmi_flash_wait_ready(FLASH_ERASE_TIMEOUT_US)) {
        goto done;
    }

    ok = true;
    goto done;

erase_error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

done:
    if (!qmi_direct_mode_stop()) {
        return false;
    }

    return ok;
}

// -----------------------------------------------------------------------------
// Page Program
// -----------------------------------------------------------------------------


//
// sync program
//
bool qmi_flash_program_page(uint32_t address, const uint8_t *buffer, size_t length) {
    if (buffer == NULL || length == 0 || length > PAGE_SIZE) {
        return false;
    }

    // 1 回の Page Program でページ境界をまたがないこと。
    if ((address / PAGE_SIZE) !=
        ((address + length - 1u) / PAGE_SIZE)) {
        return false;
    }

    // Flash 範囲チェック
    if (address >= FLASH_SIZE ||
        length > FLASH_SIZE - address) {
        return false;
    }

    flash_start_xip();

    if (!qmi_direct_mode_start()) {
        return false;
    }

    bool ok = false;

    // Flash が前の処理を実行中でないことを確認する。
    if (!qmi_flash_wait_ready(FLASH_PROGRAM_TIMEOUT_US)) {
        goto done;
    }

    // WREN
    if (!qmi_flash_write_enable()) {
        goto done;
    }

    // Page Program: 02h + 24-bit address + data
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_PAGE_PROGRAM)) {
        goto program_error;
    }

    if (!qmi_flash_write_byte((uint8_t)(address >> 16))) {
        goto program_error;
    }

    if (!qmi_flash_write_byte((uint8_t)(address >> 8))) {
        goto program_error;
    }

    if (!qmi_flash_write_byte((uint8_t)address)) {
        goto program_error;
    }

    // Program data
    for (size_t i = 0; i < length; ++i) {
        if (!qmi_flash_write_byte(buffer[i])) {
            goto program_error;
        }
    }

    if (!qmi_wait_not_busy()) {
        goto program_error;
    }

    // CS を High にして Page Program を開始させる。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // Program 完了待ち
    if (!qmi_flash_wait_ready(FLASH_PROGRAM_TIMEOUT_US))
        goto done;

    ok = true;
    goto done;

program_error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

done:
    if (!qmi_direct_mode_stop())
        return false;

    return ok;
}


// -----------------------------------------------------------------------------
// Read Data
// -----------------------------------------------------------------------------

// async read
bool qmi_flash_read_data(uint32_t address, uint8_t *buffer, size_t length) {
    uint8_t header[4];

    if (buffer == NULL || length == 0) {
        return false;
    }

    header[0] = CMD_READ_DATA;
    header[1] = (uint8_t)(address >> 16);
    header[2] = (uint8_t)(address >> 8);
    header[3] = (uint8_t)address;

    /* 念のため、前回の受信データを破棄 */
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) == 0) {
        (void)qmi_hw->direct_rx;
    }

    /* CSをアサート */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_wait_busy_clear()) {
        goto error;
    }

    /* コマンド + 24-bitアドレスを送信 */
    for (size_t i = 0; i < sizeof(header); i++) {
        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | header[i];
        if (!qmi_wait_busy_clear()) {
            goto error;
        }
    }

    /* データを読み出す */
    for (size_t i = 0; i < length; i++) {
        /* ダミーデータを送信してクロックを発生させる */
        qmi_hw->direct_tx = 0xFF;

        if (!qmi_wait_busy_clear()) {
            goto error;
        }

        /* 受信FIFOにデータが届くまで待つ */
        uint32_t start = time_us_32();

        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) {
            if ((uint32_t)(time_us_32() - start) > FLASH_READ_TIMEOUT_US) {
                goto error;
            }
        }

        buffer[i] = (uint8_t)qmi_hw->direct_rx;
    }

    if (!qmi_wait_busy_clear()) {
        goto error;
    }

    /* CSを解除 */
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    return true;

error:
    /* エラー時もCSを解除 */
    (void)qmi_wait_busy_clear();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    return false;
}

// sync function
bool qmi_flash_read(uint32_t address, uint8_t *buffer, size_t length) {
    if (buffer == NULL || length == 0) {
        return false;
    }

    if (address >= FLASH_SIZE ||
        length > FLASH_SIZE - address) {
        return false;
    }

    flash_start_xip();

    if (!qmi_direct_mode_start()) {
        return false;
    }

    bool ok = qmi_flash_read_data(address, buffer, length);

    if (!qmi_direct_mode_stop()) {
        return false;
    }

    return ok;
}

#endif //USE_STUB
