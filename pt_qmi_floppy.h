#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "conf.h"

extern uint32_t sector_size[NUM_DRIVE];
extern uint32_t drive_size[NUM_DRIVE];

typedef enum {
    PT_FLOPPY_RESULT_OK = 0,
    PT_FLOPPY_RESULT_INVALID = 1,
    PT_FLOPPY_RESULT_ISSUE = 2,
    PT_FLOPPY_RESULT_FLASH = 3
} pt_floppy_result_t;

bool pt_qmi_floppy_init(void);
bool pt_issue_floppy_read(int drive, uint32_t lba, uint8_t *buffer);
bool pt_issue_floppy_write(int drive, uint32_t lba, uint8_t *buffer);
bool pt_floppy_issue_ready(void);
bool pt_floppy_is_done(void);
int pt_floppy_error_code(void);  // read and clear
void pt_qmi_floppy_read_poll(void);
void pt_qmi_floppy_write_poll(void);
void pt_qmi_floppy_poll(void);
