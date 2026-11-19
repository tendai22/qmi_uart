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

static struct pt_erase {
    int command;
    int error;
    int done;
    uint32_t address;
} pt_body;

static struct pt_erase *pt = &pt_body;

// erase 発行

bool pt_issue_erase(struct pt_erase *pt, uint32_t address)
{
    if (pt->command)
        return false;   // 他のコマンド実行中、エラー
    pt->command = CMD_ERASE;
    // パラメータ設定
    pt->address = address;
    // protothread状態初期化
    //pt->line = 0;        // protothread start
    pt->error = 0;       // clear error flag;
    return true;
}

bool pt_is_erase_done(struct pt_erase *pt)
{
    return pt->done != true;
}

int pt_error_code(struct pt_erase *pt)
{
    return pt->error;
}

//
// polling functions
//

bool pt_erase_param_check(struct pt_erase *pt)
{
    if ((pt->address & (SECTOR_SIZE - 1u)) != 0)
        return false;
    // flash範囲チェック
    if (pt->address >= FLASH_SIZE ||
        SECTOR_SIZE > FLASH_SIZE - pt->address)
        return false;
    return true;
}



bool pt_qmi_flash_erase_4k_poll(struct pt_erase *pt) {
    PT_BEGIN(pt);
    if (pt->command != CMD_ERASE)
        return false;
    // start erase
    pt->error = 0;
    if (pt_erase_param_check(pt))
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

    if (!qmi_flash_write_byte((uint8_t)(pt->address >> 16))) {
        goto error;
    }

    if (!qmi_flash_write_byte((uint8_t)(pt->address >> 8))) {
        goto error;
    }

    if (!qmi_flash_write_byte((uint8_t)pt->address)) {
        goto error;
    }

    if (!qmi_wait_not_busy()) {
        goto error;
    }

    // CS を High にして Erase を開始させる。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // Erase 完了待ち
    PT_WAIT_UNTIL_L(pt, qmi_flash_wait_ready(FLASH_ERASE_TIMEOUT_US), WAIT_ERASE_COMP);
    goto done;
error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    goto finalize;

done:
    if (!qmi_direct_mode_stop()) {
        pt->error = 1;
    } else {
        pt->error = 0;
    }
finalize:
    pt->command = 0;
    pt->done = true;
    PT_END(pt);
}