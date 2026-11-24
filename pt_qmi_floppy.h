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

//
// Error code
//
#define FLP_ERR_OK      0   // OK        
#define FLP_ERR_COMMAND 1   // Undefined command
#define FLP_ERR_DRIVE   2   // Illegal drive number
#define FLP_ERR_CHS     3   // CHS out of bound, or LBA convert failure
#define FLP_ERR_BUFFER  4   // buffer out of bound,
#define FLP_ERR_READ    6   // read command execution failure
#define FLP_ERR_WRITE   7   // write command execution failure
#define FLP_ERR_TIMEOUT 8   // timeout in lower functions
#define FLP_ERR_INTERNAL 9  // internal error

bool pt_qmi_floppy_init(void);
bool pt_issue_floppy_read(int drive, uint32_t lba, uint8_t *buffer);
bool pt_issue_floppy_write(int drive, uint32_t lba, uint8_t *buffer);
bool pt_floppy_issue_ready(void);
bool pt_floppy_is_done(void);
int pt_floppy_error_code(void);  // read and clear
void pt_qmi_floppy_read_poll(void);
void pt_qmi_floppy_write_poll(void);
void pt_qmi_floppy_poll(void);
