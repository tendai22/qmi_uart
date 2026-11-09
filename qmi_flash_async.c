//
// qmi_flash_async.c
//

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "conf.h"
#include "memprt.h"
#include "qmi_flash.h"
#include "qmi_flash_async.h"

#define QMI_DEBUG
//
// async stub functions
//
#if defined(USE_STUB)
//
// stub for  async interfaces
//
bool qmi_flash_read_start(uint32_t address, uint8_t *buffer, size_t length) {
    return qmi_flash_read(address, buffer, length);
}

qmi_flash_poll_result_t qmi_flash_read_poll(void) {
    return QMI_FLASH_POLL_DONE;
}

bool qmi_flash_erase_4k_start(uint32_t address) {
    return qmi_flash_erase_4k(address);
}

qmi_flash_poll_result_t qmi_flash_erase_poll(void) {
    return QMI_FLASH_POLL_DONE;
}

bool qmi_flash_program_page_start(uint32_t address, const uint8_t *buffer, size_t length) {
    return qmi_flash_program_page(address, buffer, length);
}

qmi_flash_poll_result_t qmi_flash_program_poll(void) {
    return QMI_FLASH_POLL_DONE;
}

#else // USB_STUB

// -----------------------------------------------------------------------------
// W25Q32RV write commands
// -----------------------------------------------------------------------------

//
// read/erase/program async mode
//

// --------------------------------------------
// async-poll program(write)
// --------------------------------------------

//
// program status
//
typedef enum {
    PROGRAM_STATE_IDLE,

    // WREN
    PROGRAM_STATE_WREN_BEGIN,
    PROGRAM_STATE_WREN_SEND,
    PROGRAM_STATE_WREN_WAIT,

    // Page Program
    PROGRAM_STATE_PP_BEGIN,
    PROGRAM_STATE_PP_SEND_CMD,
    PROGRAM_STATE_PP_SEND_ADDR_H,
    PROGRAM_STATE_PP_SEND_ADDR_M,
    PROGRAM_STATE_PP_SEND_ADDR_L,
    PROGRAM_STATE_PP_SEND_DATA,
    PROGRAM_STATE_PP_WAIT,
    PROGRAM_STATE_PP_WAIT_BYTE,

    // Read
    PROGRAM_STATE_RD_RECEIVE_DATA,

    // Erase
    PROGRAM_STATE_ER_BEGIN,
    PROGRAM_STATE_ER_WAIT_SR1,

    // Status Register 1
    PROGRAM_STATE_STATUS_BEGIN,
    PROGRAM_STATE_STATUS_SEND_CMD,
    PROGRAM_STATE_STATUS_WAIT_CMD,
    PROGRAM_STATE_STATUS_SEND_DUMMY,
    PROGRAM_STATE_STATUS_WAIT_RX,
    PROGRAM_STATE_STATUS_WAIT_END,

    PROGRAM_STATE_DONE,

    // Error cleanup
    PROGRAM_STATE_ERROR,
    PROGRAM_STATE_ERROR_WAIT,
    PROGRAM_STATE_ERROR_END,
    PROGRAM_STATE_ERROR_FATAL

} program_state_t;

// static program_state_t program_state = PROGRAM_STATE_IDLE;

static const char *program_state_name(program_state_t state) {
    switch (state) {
    case PROGRAM_STATE_IDLE:
        return "IDLE";
    case PROGRAM_STATE_WREN_BEGIN:
        return "WREN_BEGIN";
    case PROGRAM_STATE_WREN_SEND:
        return "WREN_SEND";
    case PROGRAM_STATE_WREN_WAIT:
        return "WREN_WAIT";
    case PROGRAM_STATE_PP_BEGIN:
        return "PP_BEGIN";
    case PROGRAM_STATE_PP_SEND_CMD:
        return "PP_SEND_CMD";
    case PROGRAM_STATE_PP_SEND_ADDR_H:
        return "PP_SEND_ADDR_H";
    case PROGRAM_STATE_PP_SEND_ADDR_M:
        return "PP_SEND_ADDR_M";
    case PROGRAM_STATE_PP_SEND_ADDR_L:
        return "PP_SEND_ADDR_L";
    case PROGRAM_STATE_PP_SEND_DATA:
        return "PP_SEND_DATA";
    case PROGRAM_STATE_PP_WAIT:
        return "PP_WAIT";
    case PROGRAM_STATE_PP_WAIT_BYTE:
        return "PP_WAIT_BYTE";
    case PROGRAM_STATE_RD_RECEIVE_DATA:
        return "RD_RECEIVE_DATA";
    case PROGRAM_STATE_ER_BEGIN:
        return "ER_BEGIN";
    case PROGRAM_STATE_ER_WAIT_SR1:
        return "ER_WAIT_SR1";
    case PROGRAM_STATE_STATUS_BEGIN:
        return "STATUS_BEGIN";
    case PROGRAM_STATE_STATUS_SEND_CMD:
        return "STATUS_SEND_CMD";
    case PROGRAM_STATE_STATUS_WAIT_CMD:
        return "STATUS_WAIT_CMD";
    case PROGRAM_STATE_STATUS_SEND_DUMMY:
        return "STATUS_SEND_DUMMY";
    case PROGRAM_STATE_STATUS_WAIT_RX:
        return "STATUS_WAIT_RX";
    case PROGRAM_STATE_STATUS_WAIT_END:
        return "STATUS_WAIT_END";
    case PROGRAM_STATE_DONE:
        return "DONE";
    case PROGRAM_STATE_ERROR:
        return "ERROR";
    case PROGRAM_STATE_ERROR_WAIT:
        return "ERROR_WAIT";
    case PROGRAM_STATE_ERROR_END:
        return "ERROR_END";
    case PROGRAM_STATE_ERROR_FATAL:
        return "ERROR_FATAL";
    default:
        return "UNKNOWN";
    }
}

#ifdef QMI_DEBUG
#define PROGRAM_SET_STATE(next)                  \
    do {                                         \
        debug("[PROGRAM] %s -> %s\n",            \
              program_state_name(program.state), \
              program_state_name(next));         \
        program.state = (next);                  \
    } while (0)
#else
#define PROGRAM_SET_STATE(next) \
    do {                        \
        program.state = (next); \
    } while (0)
#endif

//
// program globals
//
typedef struct {
    program_state_t state;
    uint32_t address;
    uint8_t *buffer;
    size_t length;
    size_t position;
    uint8_t sr1;
    uint32_t direct_csr;
    // error handling
    bool direct_mode_active;
    bool cs_asserted;
    uint32_t error_start_us;
} program_context_t;

static program_context_t program;
static program_context_t debug_program;
//
// debug error
//
static void program_debug_error(const char *reason) {
    printf("[PROGRAM] ERROR: %s\n", reason);
    printf("  state      = %s\n", program_state_name(program.state));
    printf("  direct_csr = 0x%08lx\n", (unsigned long)qmi_hw->direct_csr);
    printf("  direct_rx  = 0x%08lx\n", (unsigned long)qmi_hw->direct_rx);
    printf("  position   = %lu / %lu\n", (unsigned long)program.position, (unsigned long)program.length);
    printf("  SR1        = 0x%02x\n", program.sr1);
}

//
// debug_error ... DUMMY_ERROR_OCCUR forses to change the state to ERROR
//
bool dummy_error_enable = false;

void DUMMY_ERROR_OCCUR(void) {
    if (dummy_error_enable) {
        debug("dummy error occur");
        PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
        debug("[PROGRAM] CS asserted: %s, Direct Mode active: %s\n",
              program.cs_asserted ? "true" : "false",
              program.direct_mode_active ? "true" : "false");
    }
}

bool toggle_debug_error(void) {
    return dummy_error_enable = !dummy_error_enable;
}

#if defined(QMI_DEBUG)
#define MEASURE_TIME(func) ({                                                                                                                \
    uint32_t start_time = time_us_32();                                                                                                      \
    __auto_type result = (func);                                                                                                             \
    uint32_t delta = time_us_32() - start_time;                                                                                              \
    if (delta > 1)                                                                                                                           \
        if (program.state == PROGRAM_STATE_PP_SEND_DATA || program.state == PROGRAM_STATE_RD_RECEIVE_DATA)                                   \
            debug("%s: %lu us count = %ld\n", program_state_name(program.state), (unsigned long)(delta), (unsigned long)(program.position)); \
        else                                                                                                                                 \
            debug("%s: %lu us\n", program_state_name(program.state), (unsigned long)(delta));                                                \
    result;                                                                                                                                  \
})
#else
#define MEASURE_TIME(func) func;

#endif

// ----------------------------------------------------------------
// program(write)
// ----------------------------------------------------------------

bool qmi_flash_program_start(uint32_t address, const uint8_t *buffer, size_t length) {
    uint32_t time0 = time_us_32();

    if (buffer == NULL || length == 0)
        return false;

    if (!qmi_flash_check_range(address, length))
        return false;

    if (flash_operation != FLASH_OP_NONE)
        return false;
    // 1ページを超えないことを確認
    if ((address / PAGE_SIZE) != ((address + length - 1) / PAGE_SIZE))
        return false;

    program.address = address;
    program.buffer = (uint8_t *)buffer;
    program.length = length;
    program.position = 0;
    program.direct_mode_active = false;
    program.cs_asserted = false;

    flash_operation_start_us = time_us_32();

    qmi_flash_set_operation(FLASH_OP_PROGRAM, FLASH_PROGRAM_TIMEOUT_US);
    PROGRAM_SET_STATE(PROGRAM_STATE_WREN_BEGIN);
    return true;
}

//
// qmi_flash_program_poll
//
qmi_flash_poll_result_t qmi_flash_program_poll_body(void) {
    if (flash_operation != FLASH_OP_PROGRAM)
        return QMI_FLASH_POLL_ERROR;

    if (program.state != PROGRAM_STATE_ERROR &&
        program.state != PROGRAM_STATE_ERROR_WAIT &&
        program.state != PROGRAM_STATE_ERROR_END &&
        program.state != PROGRAM_STATE_ERROR_FATAL) {
        // 通常タイムアウト処理。エラー復旧処理中には適用しない。
        uint32_t elapsed_us = (uint32_t)(time_us_32() - flash_operation_start_us);

        if (elapsed_us >= flash_operation_timeout_us) {
            printf("[PROGRAM] TIMEOUT: elapsed=%lu us, limit=%lu us\n",
                   (unsigned long)elapsed_us,
                   (unsigned long)flash_operation_timeout_us);
            printf("debug_direct_csr: %08lx\n", debug_program.direct_csr);
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
        }
    }

    switch (program.state) {

    case PROGRAM_STATE_WREN_BEGIN:
        /*
         * QMI Direct Mode開始。
         * CSをLowにして06hを送信する。(ASSERT_CS0N High)
         */
        if (!qmi_direct_mode_start())
            goto error;
        program.direct_mode_active = true;

        // ASSERT_CS0N Low (CS assert)
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        program.cs_asserted = true;
        // WREN_SEND
        if (!qmi_wait_tx_ready()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_WRITE_ENABLE;

        // WREN_WAIT
        /*
         * 06hのQMI転送完了待ち。
         */
        if (!qmi_wait_not_busy()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        // CS High (Deassert)
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        program.cs_asserted = false;

        if (!qmi_direct_mode_stop())
            goto error;
        program.direct_mode_active = false;

        PROGRAM_SET_STATE(PROGRAM_STATE_PP_BEGIN);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_BEGIN:
        /*
         * Page Programトランザクションを開始する。
         */
        if (!qmi_direct_mode_start())
            goto error;
        program.direct_mode_active = true;

        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        program.cs_asserted = true;

        if (!qmi_wait_tx_ready()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_PAGE_PROGRAM;

        if (!qmi_wait_tx_ready()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | (uint8_t)(program.address >> 16);

        if (!qmi_wait_tx_ready()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | (uint8_t)(program.address >> 8);

        if (!qmi_wait_tx_ready()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | (uint8_t)program.address;

        program.position = 0;

        PROGRAM_SET_STATE(PROGRAM_STATE_PP_SEND_DATA);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_SEND_DATA:
        /*
         * 1回のpollで最大8バイト送信する。
         */
        if (program.position >= program.length) {
            PROGRAM_SET_STATE(PROGRAM_STATE_PP_WAIT);
            return QMI_FLASH_POLL_BUSY;
        }

        debug_program.position = program.position;
        debug_program.direct_csr = qmi_hw->direct_csr;
        for (int rest = 8; rest > 0 && program.position < program.length; rest--) {
            if (!qmi_wait_tx_ready()) {
                PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
                return QMI_FLASH_POLL_BUSY;
            }
            qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | program.buffer[program.position];
            ++program.position;
            if (!qmi_wait_not_busy()) {
                PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
                return QMI_FLASH_POLL_BUSY;
            }
        }

        if (program.position >= program.length) {
            PROGRAM_SET_STATE(PROGRAM_STATE_PP_WAIT);
        } else {
            // PROGRAM_SET_STATE(PROGRAM_STATE_PP_SEND_DATA);
            program.state = PROGRAM_STATE_PP_SEND_DATA;
        }
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_PP_WAIT:
        /*
         * QMIの送信完了を待つ。
         * ここではFlash内部のProgram完了はまだ待たない。
         */
        if (!qmi_wait_not_busy()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        // CS HighでPage Programを確定させる。
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        program.cs_asserted = false;

        if (!qmi_direct_mode_stop())
            goto error;
        program.direct_mode_active = false;

        PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_BEGIN);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_STATUS_BEGIN:
        // SR1読み取りトランザクションを開始
        // 1. SR1を受信
        // 2. CSをHighにしてトランザクション終了
        // 3. SR1のBUSYビットを判定
        // 4. BUSYなら次のSR1読み取りへ戻る
        // 5. BUSYでなければDONEへ進む
        if (!qmi_direct_mode_start())
            goto error;
        program.direct_mode_active = true;

        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        program.cs_asserted = true;
        PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_SEND_CMD);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_STATUS_SEND_CMD:
        if (!qmi_wait_tx_ready()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }
        qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_READ_SR1;
        if (!qmi_wait_not_busy()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }
        if (!qmi_wait_tx_ready()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }
        qmi_hw->direct_tx = 0xFF;

        debug_program.direct_csr = qmi_hw->direct_csr;
        if (!qmi_wait_rx_ready()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        program.sr1 = (uint8_t)qmi_hw->direct_rx;

        if (!qmi_wait_not_busy()) {
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        // CS High (ASSERT_CS0N = Low)
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        program.cs_asserted = false;

        if (!qmi_direct_mode_stop())
            goto error;
        program.direct_mode_active = false;

        if (program.sr1 & SR1_BUSY_MASK) {
            PROGRAM_SET_STATE(PROGRAM_STATE_STATUS_BEGIN);
            return QMI_FLASH_POLL_BUSY;
        }

        PROGRAM_SET_STATE(PROGRAM_STATE_DONE);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_DONE:
        flash_operation = FLASH_OP_NONE;
        PROGRAM_SET_STATE(PROGRAM_STATE_IDLE);
        return QMI_FLASH_POLL_DONE;

    case PROGRAM_STATE_ERROR:
        /*
         * エラー復旧開始。
         * ここではまだCSやDirect Modeを操作しない。
         */
        program.error_start_us = time_us_32();

        PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_WAIT);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_ERROR_WAIT:
        /*
         * QMIの転送完了を待つ。
         */
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
            uint32_t elapsed_us = (uint32_t)(time_us_32() - program.error_start_us);

            if (elapsed_us >= PROGRAM_ERROR_RECOVERY_TIMEOUT_US) {
                program_debug_error("Recovery timeout");
                PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_FATAL);
                return QMI_FLASH_POLL_ERROR;
            }
            return QMI_FLASH_POLL_BUSY;
        }
        PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_END);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_ERROR_END:
        /*
         * QMI BUSY解除後にCSをHighにする。
         */
        if (program.cs_asserted) {
            qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
            program.cs_asserted = false;
        }

        /*
         * Direct Modeを終了する。
         */
        if (program.direct_mode_active) {
            if (!qmi_direct_mode_stop()) {
                program_debug_error("Direct Mode stop failed");
                PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_FATAL);
                return QMI_FLASH_POLL_ERROR;
            }
            program.direct_mode_active = false;
        }

        /*
         * 復旧完了。
         */
        flash_operation = FLASH_OP_NONE;

        PROGRAM_SET_STATE(PROGRAM_STATE_IDLE);
        return QMI_FLASH_POLL_ERROR;

    case PROGRAM_STATE_ERROR_FATAL:
        /*
         * ハードウェア状態を安全に復旧できなかった。
         * 次のFlash操作を許可しない。
         */
        return QMI_FLASH_POLL_ERROR;
    default:
        goto error;
    }

error:
    /*
     * エラー処理も本来はQMI BUSY状態を確認してから
     * CS解除・Direct Mode終了する必要がある。
     * ここでは概略のみ。
     */
    program_debug_error("QMI operation failed");
    PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
    return QMI_FLASH_POLL_BUSY;
}

qmi_flash_poll_result_t qmi_flash_program_poll(void) {
    return MEASURE_TIME(qmi_flash_program_poll_body());
}

// -------------------------------------------------
// async erase 4k
// -------------------------------------------------

//
// qmi_flash_erase_4k_start
//
bool qmi_flash_erase_4k_start_body(uint32_t address) {
    if ((address & (SECTOR_SIZE - 1u)) != 0)
        return false;

    if (!qmi_flash_check_range(address, SECTOR_SIZE))
        return false;

    if (flash_operation != FLASH_OP_NONE)
        return false;

    if (!qmi_flash_begin_write())
        return false;

    // 20h + 24-bit address
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_flash_write_byte(CMD_SECTOR_ERASE_4K) ||
        !qmi_flash_write_byte((uint8_t)(address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)address) ||
        !qmi_wait_not_busy()) {

        (void)qmi_wait_not_busy();
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        (void)qmi_direct_mode_stop();
        return false;
    }

    // CS High で Erase を開始
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_direct_mode_stop())
        return false;

    // start time
    flash_operation_start_us = time_us_32();

    qmi_flash_set_operation(FLASH_OP_ERASE, FLASH_ERASE_TIMEOUT_US);
    PROGRAM_SET_STATE(PROGRAM_STATE_ER_BEGIN);
    return true;
}

// poll 4k erase command completion
qmi_flash_poll_result_t qmi_flash_erase_4k_poll_body(void) {
    bool ready = false;

    if (flash_operation != FLASH_OP_ERASE)
        return QMI_FLASH_POLL_ERROR;
    if ((uint32_t)(time_us_32() - flash_operation_start_us) >= flash_operation_timeout_us) {
        flash_operation = FLASH_OP_NONE;
        return QMI_FLASH_POLL_ERROR;
    }
    switch (program.state) {
    case PROGRAM_STATE_ER_BEGIN:
        PROGRAM_SET_STATE(PROGRAM_STATE_ER_WAIT_SR1);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_ER_WAIT_SR1:
        if (!qmi_direct_mode_start()) {
            return QMI_FLASH_POLL_ERROR;
        }
        bool read_ok = qmi_flash_status_ready(&ready);
        bool stop_ok = qmi_direct_mode_stop();
        if (!read_ok || !stop_ok) {
            flash_operation = FLASH_OP_NONE;
            return QMI_FLASH_POLL_ERROR;
        }

        if (!ready) {
            return QMI_FLASH_POLL_BUSY;
        }
        PROGRAM_SET_STATE(PROGRAM_STATE_DONE);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_DONE:
        flash_operation = FLASH_OP_NONE;
        PROGRAM_SET_STATE(PROGRAM_STATE_IDLE);
        return QMI_FLASH_POLL_DONE;

    default:
        debug("unknown state\n");
        PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
        goto error;
    }
error:
    /*
     * エラー処理も本来はQMI BUSY状態を確認してから
     * CS解除・Direct Mode終了する必要がある。
     * ここでは概略のみ。
     */
    program_debug_error("QMI operation failed");
    PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
    return QMI_FLASH_POLL_ERROR;
}

bool qmi_flash_erase_4k_start(uint32_t address) {
    return MEASURE_TIME(qmi_flash_erase_4k_start_body(address));
}

qmi_flash_poll_result_t qmi_flash_erase_4k_poll(void) {
    return MEASURE_TIME(qmi_flash_erase_4k_poll_body());
}

// -----------------------------------------------------------
// async read
// -----------------------------------------------------------

//
// qmi_flash_read_start
//

bool qmi_flash_read_start_body(uint32_t address, uint8_t *buffer, size_t length) {
    uint32_t time0 = time_us_32();
    if (buffer == NULL || length == 0)
        return false;

    if (!qmi_flash_check_range(address, length))
        return false;

    if (flash_operation != FLASH_OP_NONE)
        return false;

    // start time
    flash_operation_start_us = time_us_32();

    if (!qmi_direct_mode_start())
        return false;

    // 残留 RX データを破棄
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) == 0) {
        (void)qmi_hw->direct_rx;
    }

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // Read Data: 03h + 24-bit address
    if (!qmi_flash_write_byte(CMD_READ_DATA) ||
        !qmi_flash_write_byte((uint8_t)(address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)address) ||
        !qmi_wait_not_busy()) {
        (void)qmi_wait_not_busy();
        TIME_CHECK("6");
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        (void)qmi_direct_mode_stop();
        TIME_CHECK("7");
        return false;
    }

    // 最初のデータ受信用クロックを発生させる
    if (!qmi_wait_tx_ready()) {
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        (void)qmi_direct_mode_stop();
        return false;
    }
    TIME_CHECK("6");

    qmi_hw->direct_tx = 0xFF;

    program.address = address;
    program.buffer = buffer;
    program.length = length;
    program.position = 0;
    program.direct_mode_active = false;
    program.cs_asserted = false;
    TIME_CHECK("7");

    qmi_flash_set_operation(FLASH_OP_READ, FLASH_PROGRAM_TIMEOUT_US);

    program.state = PROGRAM_STATE_STATUS_BEGIN;
    PROGRAM_SET_STATE(PROGRAM_STATE_RD_RECEIVE_DATA);
    return true;
}

//
// qmi_start_read_poll
//
qmi_flash_poll_result_t qmi_flash_read_poll_body(void) {
    uint32_t time0 = time_us_32();
    if (flash_operation != FLASH_OP_READ) {
        debug("flash_operation NOT_OP_READ");
        return QMI_FLASH_POLL_ERROR;
    }
    TIME_CHECK("1");

    if (program.state != PROGRAM_STATE_ERROR &&
        program.state != PROGRAM_STATE_ERROR_WAIT &&
        program.state != PROGRAM_STATE_ERROR_END &&
        program.state != PROGRAM_STATE_ERROR_FATAL) {
        // 通常タイムアウト処理。エラー復旧処理中には適用しない。
        uint32_t elapsed_us = (uint32_t)(time_us_32() - flash_operation_start_us);

        if (elapsed_us >= flash_operation_timeout_us) {
            printf("[READ] TIMEOUT: elapsed=%lu us, limit=%lu us\n",
                   (unsigned long)elapsed_us,
                   (unsigned long)flash_operation_timeout_us);
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
        }
    }

    switch (program.state) {
    case PROGRAM_STATE_RD_RECEIVE_DATA:
        // 受信開始
        for (int rest = 8; rest > 0 && program.position < program.length; rest--) {
            // 受信 FIFO にデータがなければ、まだ処理中
            if (!qmi_wait_rx_ready()) {
                TIME_CHECK("2");
                PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
                return QMI_FLASH_POLL_BUSY;
            }
            // データ到着
            program.buffer[program.position++] = (uint8_t)qmi_hw->direct_rx;
            if (program.position < program.length) {
                if (!qmi_wait_tx_ready()) {
                    TIME_CHECK("3");

                    PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
                    return QMI_FLASH_POLL_BUSY;
                }
                qmi_hw->direct_tx = 0xFF;
            }
        }

        if (program.position < program.length) {
            // 受信データはまだ残っている。状態を維持しながら戻る。
            return QMI_FLASH_POLL_BUSY;
        }

        // 最後のデータ受信後、転送完了を待って CS を解除
        if (!qmi_wait_not_busy()) {
            TIME_CHECK("4");
            PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
            return QMI_FLASH_POLL_BUSY;
        }

        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        program.cs_asserted = false;
        TIME_CHECK("5");

        if (!qmi_direct_mode_stop())
            goto error;
        program.direct_mode_active = false;
        TIME_CHECK("6");

        PROGRAM_SET_STATE(PROGRAM_STATE_DONE);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_DONE:
        flash_operation = FLASH_OP_NONE;
        PROGRAM_SET_STATE(PROGRAM_STATE_IDLE);
        return QMI_FLASH_POLL_DONE;

    case PROGRAM_STATE_ERROR:
        /*
         * エラー復旧開始。
         * ここではまだCSやDirect Modeを操作しない。
         */
        program.error_start_us = time_us_32();

        PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_WAIT);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_ERROR_WAIT:
        /*
         * QMIの転送完了を待つ。
         */
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
            uint32_t elapsed_us = (uint32_t)(time_us_32() - program.error_start_us);

            if (elapsed_us >= PROGRAM_ERROR_RECOVERY_TIMEOUT_US) {
                program_debug_error("Recovery timeout");
                PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_FATAL);
                return QMI_FLASH_POLL_ERROR;
            }
            return QMI_FLASH_POLL_BUSY;
        }
        PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_END);
        return QMI_FLASH_POLL_BUSY;

    case PROGRAM_STATE_ERROR_END:
        /*
         * QMI BUSY解除後にCSをHighにする。
         */
        if (program.cs_asserted) {
            qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
            program.cs_asserted = false;
        }

        /*
         * Direct Modeを終了する。
         */
        if (program.direct_mode_active) {
            if (!qmi_direct_mode_stop()) {
                program_debug_error("Direct Mode stop failed");
                PROGRAM_SET_STATE(PROGRAM_STATE_ERROR_FATAL);
                return QMI_FLASH_POLL_ERROR;
            }
            program.direct_mode_active = false;
        }

        /*
         * 復旧完了。
         */
        flash_operation = FLASH_OP_NONE;

        PROGRAM_SET_STATE(PROGRAM_STATE_IDLE);
        return QMI_FLASH_POLL_ERROR;

    case PROGRAM_STATE_ERROR_FATAL:
        /*
         * ハードウェア状態を安全に復旧できなかった。
         * 次のFlash操作を許可しない。
         */
        return QMI_FLASH_POLL_ERROR;
    default:
        goto error;
    }

error:
    /*
     * エラー処理も本来はQMI BUSY状態を確認してから
     * CS解除・Direct Mode終了する必要がある。
     * ここでは概略のみ。
     */
    program_debug_error("QMI operation failed");
    PROGRAM_SET_STATE(PROGRAM_STATE_ERROR);
    return QMI_FLASH_POLL_BUSY;
    // error:
    //     (void)qmi_wait_not_busy();
    //     qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    //     (void)qmi_direct_mode_stop();

    //    flash_operation = FLASH_OP_NONE;
    //    return QMI_FLASH_POLL_ERROR;
}

//
// qmi_flash_read_start_stable
//
#if 1
bool qmi_flash_read_start_stable(uint32_t address, uint8_t *buffer, size_t length) {
    if (buffer == NULL || length == 0)
        return false;

    if (!qmi_flash_check_range(address, length))
        return false;

    if (flash_operation != FLASH_OP_NONE)
        return false;

    if (!qmi_direct_mode_start())
        return false;

    // 残留 RX データを破棄
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) == 0) {
        (void)qmi_hw->direct_rx;
    }

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    // Read Data: 03h + 24-bit address
    if (!qmi_flash_write_byte(CMD_READ_DATA) ||
        !qmi_flash_write_byte((uint8_t)(address >> 16)) ||
        !qmi_flash_write_byte((uint8_t)(address >> 8)) ||
        !qmi_flash_write_byte((uint8_t)address)) {

        (void)qmi_wait_not_busy();
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        (void)qmi_direct_mode_stop();
        return false;
    }

    // 最初のデータ受信用クロックを発生させる
    if (!qmi_wait_tx_ready()) {
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        (void)qmi_direct_mode_stop();
        return false;
    }

    qmi_hw->direct_tx = 0xFF;

    program.buffer = buffer;
    program.length = length;
    program.position = 0;

    flash_operation = FLASH_OP_READ;
    return true;
}

//
// qmi_start_read_poll_stable
//
qmi_flash_poll_result_t qmi_flash_read_poll_stable(void) {
    if (flash_operation != FLASH_OP_READ) {
        return QMI_FLASH_POLL_ERROR;
    }

    // 受信 FIFO にデータがなければ、まだ処理中
    if (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) {
        return QMI_FLASH_POLL_BUSY;
    }

    program.buffer[program.position++] = (uint8_t)qmi_hw->direct_rx;

    if (program.position < program.length) {
        // 次の1バイト分のクロックを発生させる
        if (!qmi_wait_tx_ready())
            goto error;

        qmi_hw->direct_tx = 0xFF;
        return QMI_FLASH_POLL_BUSY;
    }

    // 最後のデータ受信後、転送完了を待って CS を解除
    if (!qmi_wait_not_busy())
        goto error;

    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

    if (!qmi_direct_mode_stop())
        goto error;

    flash_operation = FLASH_OP_NONE;
    return QMI_FLASH_POLL_DONE;

error:
    (void)qmi_wait_not_busy();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    (void)qmi_direct_mode_stop();
    flash_operation = FLASH_OP_NONE;
    return QMI_FLASH_POLL_ERROR;
}
#endif

//
// read wrapper
//
bool qmi_flash_read_start(uint32_t address, uint8_t *buffer, size_t length) {
#if defined(QMI_DEBUG)
    uint64_t start_time = time_us_64();
    bool result = qmi_flash_read_start_body(address, buffer, length);
    uint64_t delta = time_us_64() - start_time;
    if (delta > 1) {
        debug("%s: %llu us\n", "program_start", (unsigned long long)(delta));
    }
#else
    bool result = qmi_flash_read_start_body(address, buffer, length);
#endif
    return result;
}

qmi_flash_poll_result_t qmi_flash_read_poll(void) {
    static int count = 0;
    return MEASURE_TIME(qmi_flash_read_poll_body());
}

#endif // USE_STUB
