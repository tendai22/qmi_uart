#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "conf.h"
#include "memprt.h"
#include "protothread.h"
#include "pt_qmi.h"
#include "pt_qmi_floppy.h"
#include "pt_qmi_floppy_table.h"

#define FLOPPY_CMD_NONE 0
#define FLOPPY_CMD_READ 1
#define FLOPPY_CMD_WRITE 2

_Static_assert(NUM_DRIVE == 2, "layout expects two drives");
_Static_assert(SECTOR_SIZE == 4096u, "work buffer must be 4 KiB");
_Static_assert(PAGE_SIZE == 256u, "page size must be 256 bytes");
_Static_assert((SECTOR_SIZE % PAGE_SIZE) == 0, "invalid geometry");
_Static_assert(DRIVE_A_SIZE + DRIVE_B_SIZE <= FLASH_SIZE, "floppy area exceeds flash");

uint32_t sector_size[NUM_DRIVE];
uint32_t drive_size[NUM_DRIVE];
static uint32_t drive_base[NUM_DRIVE];
static uint32_t initialized_sector_size[NUM_DRIVE];
static uint8_t work_buffer[SECTOR_SIZE];
static bool initialized;

struct pt_context {
    int line;
    int command;  // atomically accessed between cores
    int error;
    int done;     // atomically accessed between cores
    int drive;
    uint32_t lba;
    uint8_t *buffer;
    uint32_t address;
    uint32_t erase_address;
    uint32_t offset;
    uint32_t page_offset;
    int lower_issued;
};
static struct pt_context pt_body;
static struct pt_context *pt = &pt_body;

// Only one Core1 producer and one Core0 consumer are supported.
// All command/done/error cross-core handoffs use atomic operations.
static inline int load_command(void) { return __atomic_load_n(&pt->command, __ATOMIC_ACQUIRE); }
static inline void store_command(int value) { __atomic_store_n(&pt->command, value, __ATOMIC_RELEASE); }

bool pt_qmi_floppy_init(void) {
    const uint32_t capacity[NUM_DRIVE] = {DRIVE_A_SIZE, DRIVE_B_SIZE};
    uint32_t bases[NUM_DRIVE], sizes[NUM_DRIVE];
    uint32_t cursor = FLASH_SIZE;
    if (load_command() != FLOPPY_CMD_NONE) return false;
    initialized = false;
    for (int i = 0; i < NUM_DRIVE; ++i) {
        if ((sector_size[i] != 128u && sector_size[i] != 256u) ||
            capacity[i] == 0 || capacity[i] % sector_size[i] ||
            capacity[i] % SECTOR_SIZE) return false;
        sizes[i] = capacity[i] / sector_size[i];
    }
    for (int i = NUM_DRIVE - 1; i >= 0; --i) {
        if (capacity[i] > cursor) return false;
        cursor -= capacity[i];
        bases[i] = cursor;
    }
    for (int i = 0; i < NUM_DRIVE; ++i) {
        if (bases[i] % SECTOR_SIZE || !qmi_flash_check_range(bases[i], capacity[i])) return false;
    }
    for (int i = 0; i < NUM_DRIVE; ++i) {
        drive_base[i] = bases[i];
        drive_size[i] = sizes[i];
        initialized_sector_size[i] = sector_size[i];
    }
    __atomic_store_n(&initialized, true, __ATOMIC_RELEASE);
    return true;
}

static bool valid_request(int drive, uint32_t lba, const uint8_t *buffer) {
    return __atomic_load_n(&initialized, __ATOMIC_ACQUIRE) && buffer &&
           drive >= 0 && drive < NUM_DRIVE &&
           sector_size[drive] == initialized_sector_size[drive] &&
           lba < drive_size[drive];
}

static bool issue(int command, int drive, uint32_t lba, uint8_t *buffer) {
    if (!valid_request(drive, lba, buffer)) return false;
    if (load_command() != FLOPPY_CMD_NONE) return false;
    pt->drive = drive;
    pt->lba = lba;
    pt->buffer = buffer;
    pt->address = drive_base[drive] + lba * sector_size[drive];
    pt->erase_address = pt->address & ~(SECTOR_SIZE - 1u);
    pt->offset = pt->address - pt->erase_address;
    pt->page_offset = 0;
    pt->lower_issued = 0;
    pt->line = 0;
    __atomic_store_n(&pt->error, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pt->done, 0, __ATOMIC_RELEASE);
    store_command(command);
    return true;
}

bool pt_issue_floppy_read(int drive, uint32_t lba, uint8_t *buffer) {
    return issue(FLOPPY_CMD_READ, drive, lba, buffer);
}
bool pt_issue_floppy_write(int drive, uint32_t lba, uint8_t *buffer) {
    return issue(FLOPPY_CMD_WRITE, drive, lba, buffer);
}
bool pt_floppy_issue_ready(void) { return load_command() == FLOPPY_CMD_NONE; }
bool pt_floppy_is_done(void) { return __atomic_load_n(&pt->done, __ATOMIC_ACQUIRE) != 0; }
int pt_floppy_error_code(void) { return __atomic_exchange_n(&pt->error, 0, __ATOMIC_ACQ_REL); }

// The helper starts the lower operation once, then waits for completion.
// On failure it records an error and returns true, so the PT can exit.
static bool lower_read(uint32_t address) {
    if (!pt->lower_issued) {
        if (!pt_issue_ready()) return false;
        if (!pt_issue_read(address, work_buffer, SECTOR_SIZE)) {
            pt->error = PT_FLOPPY_RESULT_ISSUE;
            return true;
        }
        pt->lower_issued = 1;
        return false;
    }
    if (!pt_is_done()) return false;
    if (pt_error_code()) pt->error = PT_FLOPPY_RESULT_FLASH;
    pt->lower_issued = 0;
    return true;
}
static bool lower_erase(uint32_t address) {
    if (!pt->lower_issued) {
        if (!pt_issue_ready()) return false;
        if (!pt_issue_erase(address)) {
            pt->error = PT_FLOPPY_RESULT_ISSUE;
            return true;
        }
        pt->lower_issued = 1;
        return false;
    }
    if (!pt_is_done()) return false;
    if (pt_error_code()) pt->error = PT_FLOPPY_RESULT_FLASH;
    pt->lower_issued = 0;
    return true;
}
static bool lower_program(uint32_t address, uint32_t offset) {
    if (!pt->lower_issued) {
        if (!pt_issue_ready()) return false;
        if (!pt_issue_program(address + offset, &work_buffer[offset], PAGE_SIZE)) {
            pt->error = PT_FLOPPY_RESULT_ISSUE;
            return true;
        }
        pt->lower_issued = 1;
        return false;
    }
    if (!pt_is_done()) return false;
    if (pt_error_code()) pt->error = PT_FLOPPY_RESULT_FLASH;
    pt->lower_issued = 0;
    return true;
}

static void finish(void) {
    // Publish data/error completion; command is retired by the poll wrapper
    // only after PT_END has returned and no more context writes remain.
    __atomic_store_n(&pt->done, 1, __ATOMIC_RELEASE);
}

static void pt_qmi_floppy_read_poll_body(void) {
    PT_BEGIN(pt);
    if (load_command() != FLOPPY_CMD_READ) return;
    pt->error = 0;
    PT_WAIT_UNTIL_L(pt, lower_read(pt->erase_address), FLOPPY_READ_4K);
    if (pt->error) goto finalize;
    memcpy(pt->buffer, &work_buffer[pt->offset], sector_size[pt->drive]);
finalize:
    finish();
    PT_END(pt);
}

static void pt_qmi_floppy_write_poll_body(void) {
    PT_BEGIN(pt);
    if (load_command() != FLOPPY_CMD_WRITE) return;
    pt->error = 0;
    PT_WAIT_UNTIL_L(pt, lower_read(pt->erase_address), FLOPPY_WRITE_READ_4K);
    if (pt->error) goto finalize;
    memcpy(&work_buffer[pt->offset], pt->buffer, sector_size[pt->drive]);
    PT_WAIT_UNTIL_L(pt, lower_erase(pt->erase_address), FLOPPY_WRITE_ERASE_4K);
    if (pt->error) goto finalize;
    pt->page_offset = 0;
    while (pt->page_offset < SECTOR_SIZE) {
        PT_WAIT_UNTIL_L(pt, lower_program(pt->erase_address, pt->page_offset), FLOPPY_WRITE_PROGRAM_PAGE);
        if (pt->error) goto finalize;
        pt->page_offset += PAGE_SIZE;
    }
finalize:
    finish();
    PT_END(pt);
}

typedef void (*pt_body_func_t)(void);
static uint32_t stage_start;
static int stage_count;
static void pt_qmi_floppy_func_poll(pt_body_func_t body_func) {
    int old_line = pt->line;
    uint32_t start = time_us_32();
    body_func();
    uint32_t now = time_us_32();
    uint32_t duration = now - start;
    if (old_line == 0 && pt->line != 0) stage_start = start;
    if (old_line != 0 || pt->line != 0) {
        if (old_line != pt->line) {
            debug("FLOPPY %s -> %s: %lu us, count=%d\n",
                  pt_qmi_floppy_label(old_line), pt_qmi_floppy_label(pt->line),
                  (unsigned long)(now - stage_start), stage_count);
            stage_start = now;
            stage_count = 0;
        } else {
            ++stage_count;
        }
    }
    pt_qmi_floppy_stats_enter(old_line);
    pt_qmi_floppy_stats_leave(old_line, duration);
    // The producer must not reuse pt_context until the entire body and
    // bookkeeping have finished.  The command release is the final step.
    if (__atomic_load_n(&pt->done, __ATOMIC_ACQUIRE)) {
        store_command(FLOPPY_CMD_NONE);
    }
}
void pt_qmi_floppy_read_poll(void) { pt_qmi_floppy_func_poll(pt_qmi_floppy_read_poll_body); }
void pt_qmi_floppy_write_poll(void) { pt_qmi_floppy_func_poll(pt_qmi_floppy_write_poll_body); }
void pt_qmi_floppy_poll(void) {
    switch (load_command()) {
    case FLOPPY_CMD_READ: pt_qmi_floppy_read_poll(); break;
    case FLOPPY_CMD_WRITE: pt_qmi_floppy_write_poll(); break;
    default: break;
    }
}
