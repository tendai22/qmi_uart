//
// pt_qmi_common.c
//

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/flash.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "protothread.h"

#include "conf.h"
#include "memprt.h"
#include "pt_qmi.h"

#include "pt_qmi_common_table.h"

static struct pt_erase {
    int line;
    int command;
    int error;
    int done;
    uint32_t address;
} pt_body;

static struct pt_erase *pt = &pt_body;

// erase 発行

bool pt_issue_erase(uint32_t address) {
    if (pt->command)
        return false; // 他のコマンド実行中、エラー
    pt->command = CMD_ERASE;
    // パラメータ設定
    pt->address = address;
    // protothread状態初期化
    // pt->line = 0;        // protothread start
    pt->error = 0; // clear error flag;
    debug("ISSUE_ERASE: %08lx\n", pt->address);
    return true;
}

bool pt_is_erase_done(void) {
    return pt->done == true;
}

int pt_error_code(void) {
    int errcode = pt->error;
    pt->error = 0;
    return errcode;
}

bool pt_issue_ready(void) {
    return pt->command == 0;
}

bool pt_erase_sr1_ready(void) {
    bool ready;
    if (!qmi_flash_status_ready(&ready)) {
        pt->error = 4;
        return true;
    }
    // sr1 has read and ready has valid data
    return ready;
}
//
// polling functions
//

bool pt_erase_param_check(struct pt_erase *pt) {
    if ((pt->address & (SECTOR_SIZE - 1u)) != 0)
        return false;
    // flash範囲チェック
    if (pt->address >= FLASH_SIZE ||
        SECTOR_SIZE > FLASH_SIZE - pt->address)
        return false;
    return true;
}

#define SAMPLE_COUNT 1000
static uint32_t poll_start[SAMPLE_COUNT];
static uint32_t poll_elapsed[SAMPLE_COUNT];
static uint32_t sample_index = 0;

static void pt_qmi_flash_erase_4k_poll_body(void) {
    PT_BEGIN(pt);
    if (pt->command != CMD_ERASE) {
        return;
    }
    //
    sample_index = 0;
    // start erase
    pt->error = 0;
    if (pt_erase_param_check(pt) == false)
        goto error1;

    if (!qmi_direct_mode_start())
        goto error1;
    // 以後のエラー脱出は error に飛ぶ。
    // Flash が前の処理を実行中でないことを確認する。
    PT_WAIT_UNTIL_L(pt, pt_erase_sr1_ready(), WAIT_PREV_CMD_COMPLETE);
    if (pt->error)
        goto error;

    //  WREN
    if (!qmi_flash_write_enable()) {
        goto error;
    }
    // Sector Erase: 20h + 24-bit address
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_SECTOR_ERASE_4K) ||
        !qmi_flash_write_byte((uint8_t)(pt->address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(pt->address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)pt->address)) {
        goto error;
    }
    if (!qmi_wait_not_busy()) {
        goto error;
    }
    // CS を High にして Erase を開始させる。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    // Erase 完了待ち
    //PT_WAIT_UNTIL_L(pt, !pt_erase_sr1_ready(), WAIT_START_ERASE);
    //if (pt->error)
    //    goto error;
    PT_WAIT_UNTIL_L(pt, pt_erase_sr1_ready(), WAIT_ERASE_COMP);
    if (pt->error)
        goto error;
done:
    if (!qmi_direct_mode_stop()) {
        pt->error = 1;
    } else {
        pt->error = 0;
    }
    goto finalize;

error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    qmi_direct_mode_stop();
error1:
    if (pt->error == 0)
        pt->error = 1;
    goto finalize;

finalize:
    pt->command = 0;
    pt->done = true;
    PT_END(pt);
}

static uint32_t start;
static uint32_t elapsed;
static int count = 0;

void pt_qmi_flash_erase_4k_poll(void) {
    int old_line = pt->line;
    uint32_t poll_start = time_us_32();
    pt_qmi_flash_erase_4k_poll_body();
    uint32_t now = time_us_32();
    uint32_t poll_elapsed = now - poll_start;
    if (old_line == 0 && pt->line != 0)
        start = poll_start;

    if (old_line != 0 || pt->line != 0) {
        if (old_line != pt->line) {
            elapsed = now - start;
            debug("%s -> %s: %ld us, count = %d\n", pt_label(old_line), pt_label(pt->line), elapsed, count);
            start = now;
            count = 0;
        } else {
            count++;
        }
    }
    pt_stats_enter(old_line);
    pt_stats_leave(old_line, poll_elapsed);
}

void pt_dump_polltime(void) {
    for (int i = 0; i < sample_index; ++i) {
        printf("%2d: %8ld %8ld\n", i, poll_start[i], poll_elapsed[i]);
    }
}
