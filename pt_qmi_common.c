//
// pt_qmi_common.c
//

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "hardware/flash.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"

#include "protothread.h"

#include "conf.h"
#include "pt_qmi.h"

static int pt_command = 0;
static int pt_error = 0;
static int pt_done = 0;

static uint32_t er_address;

// erase 発行

bool pt_issue_erase(uint32_t address)
{
    if (pt_command)
        return false;   // 他のコマンド実行中、エラー
    pt_command = CMD_ERASE;
    // パラメータ設定
    er_address = address;
    // protothread状態初期化
    //pt_line = 0;        // protothread start
    pt_error = 0;       // clear error flag;
    return true;
}

bool is_erase_done(void)
{
    return pt_done != true;
}

int error_code(void)
{
    return pt_error;
}

//
// polling functions
//

bool erase_param_check(void)
{
    if ((er_address & (SECTOR_SIZE - 1u)) != 0)
        return false;
    // flash範囲チェック
    if (er_address >= FLASH_SIZE ||
        SECTOR_SIZE > FLASH_SIZE - er_address)
        return false;
    return true;
}

bool pt_qmi_flash_erase_4k_poll(void) {
    PT_BEGIN();
    if (pt_command != CMD_ERASE)
        return false;
    // start erase
    pt_error = 0;
    if (erase_param_check())
        goto error;

    if (!qmi_direct_mode_start())
        goto error;
    // Flash が前の処理を実行中でないことを確認する。
    if (!qmi_flash_wait_ready(FLASH_PROGRAM_TIMEOUT_US)) {
        goto error;
    }
    // WREN
    if (!qmi_flash_write_enable()) {
        goto error;
    }
    // Sector Erase: 20h + 24-bit address
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_SECTOR_ERASE_4K)) {
        goto error;
    }

    if (!qmi_flash_write_byte((uint8_t)(er_address >> 16))) {
        goto error;
    }

    if (!qmi_flash_write_byte((uint8_t)(er_address >> 8))) {
        goto error;
    }

    if (!qmi_flash_write_byte((uint8_t)er_address)) {
        goto error;
    }

    if (!qmi_wait_not_busy()) {
        goto error;
    }

    // CS を High にして Erase を開始させる。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // Erase 完了待ち
    PT_WAIT_UNTIL(qmi_flash_wait_ready(FLASH_ERASE_TIMEOUT_US));
    goto done;
error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    goto finalize;

done:
    if (!qmi_direct_mode_stop()) {
        pt_error = 1;
    } else {
        pt_error = 0;
    }
finalize:
    pt_command = 0;
    pt_done = true;
    PT_END();
}