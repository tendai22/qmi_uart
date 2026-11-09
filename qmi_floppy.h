#pragma once
//
// qmi_floppy.h
//

#include <stdint.h>

#define NUM_DRIVE 2

typedef enum
{
    QMI_FLOPPY_RESULT_OK = 0,
    QMI_FLOPPY_RESULT_ERROR
} qmi_floppy_result_t;

extern uint32_t sector_size[NUM_DRIVE];
extern uint32_t drive_size[NUM_DRIVE];

extern volatile int floppy_operation;

extern qmi_floppy_result_t qmi_floppy_init(void);

extern qmi_floppy_result_t
qmi_floppy_read(
    int drive,
    uint32_t address,
    int count,
    uint8_t *buffer);

extern qmi_floppy_result_t
qmi_floppy_write(
    int drive,
    uint32_t address,
    int count,
    uint8_t *buffer);
