//
// pt_qmi_flash.c
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
#include "qmi_flash.h"

#include "pt_qmi_flash_table.h"


//
// pt_context ... erase/program_page/read 共通のコンテキスト
//

static struct pt_context {
    int line;
    int command;
    int error;
    int done;
    // erase/program/read 共通
    uint32_t address;
    // program/read 共通
    uint8_t *buffer;
    uint32_t count;
    uint32_t length;
    // SR1 BUSY待ちのタイムアウト管理
    uint32_t wait_start_us;
    uint32_t wait_timeout_us;
} pt_body;

static struct pt_context *pt = &pt_body;

// erase 発行

bool pt_issue_erase(uint32_t address) {
    if (pt->command)
        return false; // 他のコマンド実行中、エラー
    pt->command = CMD_ERASE;
    // パラメータ設定
    pt->address = address;
    // protothread状態初期化
    // pt->line = 0;        // protothread start
    pt->done = 0;
    pt->error = 0; // clear error flag;
    debug("ISSUE_ERASE: %08lx\n", pt->address);
    return true;
}

bool pt_is_done(void) {
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

//
// pt_sr1_wait_start(timeout_us)
// sr1待ちの開始時に呼び出しタイムアウト値を設定する。
static void pt_sr1_wait_start(uint32_t timeout_us) {
    pt->wait_start_us = time_us_32();
    pt->wait_timeout_us = timeout_us;
}

bool pt_context_sr1_ready(void) {
    if (qmi_flash_status_ready()) {
        pt->error = qmi_error();
        return true;
    }

    if ((uint32_t)(time_us_32() - pt->wait_start_us) >= pt->wait_timeout_us) {
        pt->error = QMI_ERR_TIMEOUT;
        return true;
    }

    return false;
}
//
// polling functions
//

bool pt_context_param_check(struct pt_context *pt) {
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
        pt->error = QMI_ERR_COMMAND;
        return;
    }
    // start erase
    sample_index = 0;
    pt->error = 0;
    if (pt_context_param_check(pt) == false) {
        pt->error = QMI_ERR_PARAM;
        goto error1;
    }

    if (!qmi_direct_mode_start()) {
        pt->error = qmi_error();
        goto error1;
    }
    // 以後のエラー脱出は error に飛ぶ。
    // Flash が前の処理を実行中でないことを確認する。
    pt_sr1_wait_start(FLASH_ERASE_TIMEOUT_US);
    PT_WAIT_UNTIL_L(pt, pt_context_sr1_ready(), WAIT_PREV_CMD_COMPLETE);
    if (pt->error)
        goto error;

    //  WREN
    if (!qmi_flash_write_enable()) {
        pt->error = qmi_error();
        goto error;
    }
    // Sector Erase: 20h + 24-bit address
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_SECTOR_ERASE_4K) ||
        !qmi_flash_write_byte((uint8_t)(pt->address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(pt->address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)pt->address)) {
        pt->error = qmi_error();
        goto error;
    }
    if (!qmi_wait_not_busy()) {
        pt->error = qmi_error();
        goto error;
    }
    // CS を High にして Erase を開始させる。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    // Erase 完了待ち
    pt_sr1_wait_start(FLASH_ERASE_TIMEOUT_US);
    PT_WAIT_UNTIL_L(pt, pt_context_sr1_ready(), WAIT_ERASE_COMP);
    if (pt->error)
        goto error;
done:
    if (!qmi_direct_mode_stop()) {
        pt->error = qmi_error();
    }
    goto finalize;

error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    qmi_direct_mode_stop();
error1:
    if (pt->error == 0)
        pt->error = QMI_ERR_INTERNAL;
    goto finalize;

finalize:
    pt->command = 0;
    pt->done = true;
    PT_END(pt);
}

static uint32_t start;
static uint32_t elapsed;
static int total_count = 0;

typedef void (*pt_body_func_t)(void);

static void pt_qmi_flash_func_poll(pt_body_func_t body_func) {
    int old_line = pt->line;
    uint32_t poll_start = time_us_32();

    // Protothread本体を実行
    body_func();

    uint32_t now = time_us_32();
    uint32_t poll_elapsed = now - poll_start;

    if (old_line == 0 && pt->line != 0)
        start = poll_start;

    if (old_line != 0 || pt->line != 0) {
        if (old_line != pt->line) {
            elapsed = now - start;
            debug("%s -> %s: %lu us, count = %d\n", pt_qmi_flash_label(old_line), pt_qmi_flash_label(pt->line), (unsigned long)elapsed, total_count);
            start = now;
            total_count = 0;
        } else {
            total_count++;
        }
    }
    pt_qmi_flash_stats_enter(old_line);
    pt_qmi_flash_stats_leave(old_line, poll_elapsed);
}

void pt_qmi_flash_erase_4k_poll(void) {
    pt_qmi_flash_func_poll(pt_qmi_flash_erase_4k_poll_body);
}

// ----------------------------------------------------
// program_page ... write a 256-byte chank
// ----------------------------------------------------
bool pt_issue_program(uint32_t address, uint8_t *buffer, uint32_t length) {
    if (pt->command)
        return false; // 他のコマンド実行中、エラー
    pt->command = CMD_PROGRAM;
    // パラメータ設定
    pt->address = address;
    pt->buffer = buffer;
    pt->length = length;
    // protothread状態初期化
    pt->count = 0;
    // pt->line = 0;        // protothread start
    pt->done = 0;
    pt->error = 0; // clear error flag;
    debug("ISSUE_PROGRAM: %08lx, length = %ld\n", pt->address, (unsigned long)pt->length);
    return true;
}

bool pt_program_page_param_check(void) {
    if (pt->buffer == NULL)
        return false;
    if (pt->length == 0 || pt->length > PAGE_SIZE)
        return false;
    if ((pt->address & (PAGE_SIZE - 1u)) != 0)
        return false;
    if (pt->address >= FLASH_SIZE)
        return false;
    // FLASH末尾を超えて書き込もうとする場合はエラーとする。
    if (pt->length > FLASH_SIZE - pt->address)
        return false;
    // page 境界をまたいでの書き込みはエラーとする
    if ((pt->address % PAGE_SIZE) + pt->length > PAGE_SIZE)
        return false;
    return true;
}

// 8 バイトまたは残りを書き出す。書き込みが終わると true を返す
static bool pt_write_chunk_bytes(void) {
    int n = pt->length - pt->count;
    if (n > 8)
        n = 8;
    for (int i = 0; i < n; ++i) {
        if (!qmi_flash_write_byte(pt->buffer[pt->count])) {
            pt->error = qmi_error();
            return true; // PT_WAITを抜けてエラー処理へ
        }
        pt->count++;
    }
    return pt->count >= pt->length;
}

void pt_qmi_flash_program_page_poll_body(void) {
    PT_BEGIN(pt);
    if (pt->command != CMD_PROGRAM) {
        pt->error = QMI_ERR_COMMAND;
        return;
    }
    // start program_page
    sample_index = 0;
    pt->error = 0;
    if (pt_program_page_param_check() == false)
        goto error1;
    if (!qmi_direct_mode_start()) {
        pt->error = qmi_error();
        goto error1;
    }
    // 以後のエラー脱出は error に飛ぶ。
    // Flash が前の処理を実行中でないことを確認する。
    pt_sr1_wait_start(FLASH_ERASE_TIMEOUT_US);
    PT_WAIT_UNTIL_L(pt, pt_context_sr1_ready(), WAIT_PREV_CMD_COMPLETE_IN_PROGRAM);
    if (pt->error)
        goto error;

    //  WREN
    if (!qmi_flash_write_enable()) {
        pt->error = qmi_error();
        goto error;
    }
    // Page Program: 02h + 24-bit address + data
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_PAGE_PROGRAM) ||
        !qmi_flash_write_byte((uint8_t)(pt->address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(pt->address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)pt->address)) {
        pt->error = qmi_error();
        goto error;
    }

    // program data
    pt->count = 0;
    // 8バイト単位で書き出す(pt_write_chunk_bytes())を、データが
    // 尽きるまで繰り返す。
    PT_WAIT_UNTIL_L(pt, pt_write_chunk_bytes(), WRITE_BYTES);
    if (pt->error)
        goto error;

    if (!qmi_wait_not_busy()) {
        pt->error = qmi_error();
        goto error;
    }
    // CS を High にして PROGRAM を開始させる。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // デバイス内部の PROGRAM 完了を待つ
    pt_sr1_wait_start(FLASH_PROGRAM_TIMEOUT_US);
    PT_WAIT_UNTIL_L(pt, pt_context_sr1_ready(), WAIT_PROGRAM_COMP);
    if (pt->error)
        goto error;
done:
    if (!qmi_direct_mode_stop()) {
        pt->error = qmi_error();
    }
    goto finalize;

error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    qmi_direct_mode_stop();
error1:
    if (pt->error == 0)
        pt->error = QMI_ERR_INTERNAL;
    goto finalize;

finalize:
    pt->command = 0;
    pt->done = true;
    PT_END(pt);
}

void pt_qmi_flash_program_page_poll(void) {
    pt_qmi_flash_func_poll(pt_qmi_flash_program_page_poll_body);
}

// ------------------------------------------------
// read
// ------------------------------------------------

bool pt_issue_read(uint32_t address, uint8_t *buffer, uint32_t length) {
    if (pt->command)
        return false; // 他のコマンド実行中、エラー
    pt->command = CMD_READ;
    // パラメータ設定
    pt->address = address;
    pt->buffer = buffer;
    pt->length = length;
    // protothread状態初期化
    pt->count = 0;
    // pt->line = 0;        // protothread start
    pt->done = 0;
    pt->error = 0; // clear error flag;
    debug("ISSUE_READ: %08lx, length = %ld\n", pt->address, (unsigned long)pt->length);
    return true;
}

bool pt_read_param_check(void) {
    if (pt->buffer == NULL)
        return false;
    if (pt->length == 0)
        return false;
    if (pt->address >= FLASH_SIZE)
        return false;
    // FLASH末尾を超えて読み込もうとする場合はエラーとする。
    if (pt->length > FLASH_SIZE - pt->address)
        return false;
    return true;
}

// 8 バイトまたは残りを読み込む。読込みが終わると true を返す
static bool pt_read_chunk_bytes(void) {
    int n = pt->length - pt->count;
    if (n > 8)
        n = 8;
    for (int i = 0; i < n; ++i) {
        /* ダミーデータを送信してクロックを発生させる */
        qmi_hw->direct_tx = 0xFF;

        if (!qmi_wait_busy_clear()) {
            pt->error = qmi_error();
            return true;
        }

        /* 受信FIFOにデータが届くまで待つ */
        uint32_t start = time_us_32();

        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) {
            if ((uint32_t)(time_us_32() - start) >= FLASH_READ_TIMEOUT_US) {
                pt->error = QMI_ERR_TIMEOUT;
                return true;
            }
        }

        pt->buffer[pt->count++] = (uint8_t)qmi_hw->direct_rx;
    }
    return pt->count >= pt->length;
}

static void pt_qmi_flash_read_poll_body(void) {
    PT_BEGIN(pt);
    if (pt->command != CMD_READ) {
        return;
    }
    // start read
    sample_index = 0;
    pt->error = 0;
    if (pt_read_param_check() == false) {
        pt->error = QMI_ERR_PARAM;
        goto error1;
    }
    if (!qmi_direct_mode_start()) {
        pt->error = qmi_error();
        goto error1;
    }
    // 以後のエラー脱出は error に飛ぶ。
    // Flash が前の処理を実行中でないことを確認する。
    pt_sr1_wait_start(FLASH_ERASE_TIMEOUT_US);
    PT_WAIT_UNTIL_L(pt, pt_context_sr1_ready(), WAIT_PREV_CMD_COMPLETE_IN_READ);
    if (pt->error) {
        pt->error = QMI_ERR_PARAM;
        goto error;
    }
    // Read: 03h + 24-bit address + data
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_READ_DATA) ||
        !qmi_flash_write_byte((uint8_t)(pt->address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(pt->address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)pt->address)) {
        goto error;
    }
    // read data
    pt->count = 0;
    // 8バイト単位で読み込む(pt_read_chunk_bytes())を、データが
    // 尽きるまで繰り返す。
    PT_WAIT_UNTIL_L(pt, pt_read_chunk_bytes(), READ_BYTES);
    if (pt->error)
        goto error;
    if (!qmi_wait_not_busy()) {
        goto error;
    }
    // CS を解除する。
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

done:
    if (!qmi_direct_mode_stop()) {
        pt->error = 1;
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

static void pt_qmi_flash_read_poll(void) {
    pt_qmi_flash_func_poll(pt_qmi_flash_read_poll_body);
}

//

// -------------------------------------------------
// overall poll
// -------------------------------------------------
void pt_qmi_flash_poll(void) {
    switch (pt->command) {
    case 0:
        break;
    case CMD_READ:
        pt_qmi_flash_read_poll();
        break;
    case CMD_ERASE:
        pt_qmi_flash_erase_4k_poll();
        break;
    case CMD_PROGRAM:
        pt_qmi_flash_program_page_poll();
        break;
    default:
        printf("unexpected command %d\n", pt->command);
        break;
    }
}

void pt_dump_polltime(void) {
    for (int i = 0; i < sample_index; ++i) {
        printf("%2d: %8ld %8ld\n", i, poll_start[i], poll_elapsed[i]);
    }
}
