
//
// qmi_floppy.c
//

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "pico/stdlib.h"

#include "conf.h"
#include "qmi_flash.h"
#include "qmi_flash_async.h"
#include "qmi_floppy.h"

#define FLOPPY_OP_NONE      0
#define FLOPPY_OP_READ      1
#define FLOPPY_OP_WRITE     2

//
// globals
//
uint32_t sector_size[NUM_DRIVE];
uint32_t drive_size[NUM_DRIVE];

volatile int floppy_operation = FLOPPY_OP_NONE;

static uint32_t drive_base[NUM_DRIVE];

static uint8_t work_buffer[SECTOR_SIZE];

static bool floppy_initialized = false;
static uint32_t initialized_sector_size[NUM_DRIVE];

static bool
wait_poll(
    qmi_flash_poll_result_t (*poll_func)(void))
{
    while (true)
    {
        qmi_flash_poll_result_t result;

        result = poll_func();

        switch (result)
        {
        case QMI_FLASH_POLL_BUSY:
            tight_loop_contents();
            break;

        case QMI_FLASH_POLL_DONE:
            return true;

        case QMI_FLASH_POLL_ERROR:
        default:
            return false;
        }
    }
}

static uint32_t
flash_address_from_sector(
    int drive,
    uint32_t sector)
{
    return drive_base[drive]
        + (sector * sector_size[drive]);
}

static bool
read_4k_sector(
    uint32_t sector_address)
{
    if (!qmi_flash_read_start(
            sector_address,
            work_buffer,
            SECTOR_SIZE))
    {
        return false;
    }

    return wait_poll(qmi_flash_read_poll);
}

static bool
erase_4k_sector(
    uint32_t sector_address)
{
    if (!qmi_flash_erase_4k_start(sector_address))
    {
        return false;
    }

    return wait_poll(qmi_flash_erase_4k_poll);
}

static bool
program_4k_sector(
    uint32_t sector_address)
{
    uint32_t offset;

    for (offset = 0;
         offset < SECTOR_SIZE;
         offset += PAGE_SIZE)
    {
        if (!qmi_flash_program_start(
                sector_address + offset,
                &work_buffer[offset],
                PAGE_SIZE))
        {
            return false;
        }

        if (!wait_poll(qmi_flash_program_poll))
        {
            return false;
        }
    }

    return true;
}

static bool
write_one_sector(
    int drive,
    uint32_t sector,
    const uint8_t *source)
{
    uint32_t flash_address;
    uint32_t erase_address;
    uint32_t overwrite_offset;

    flash_address =
        flash_address_from_sector(
            drive,
            sector);

    erase_address =
        flash_address &
        ~(SECTOR_SIZE - 1u);

    overwrite_offset =
        flash_address -
        erase_address;

    if (!read_4k_sector(erase_address))
    {
        return false;
    }

    memcpy(
        &work_buffer[overwrite_offset],
        source,
        sector_size[drive]);

    if (!erase_4k_sector(erase_address))
    {
        return false;
    }

    if (!program_4k_sector(erase_address))
    {
        return false;
    }

    return true;
}

//
// Validate a floppy read/write request.
//
static bool
check_request(
    int drive,
    uint32_t address,
    int count,
    const uint8_t *buffer)
{
    if (!floppy_initialized)
        return false;

    if (buffer == NULL)
        return false;

    if (count <= 0)
        return false;

    if (drive < 0 ||
        drive >= NUM_DRIVE)
        return false;

    // Reject changes to the sector size after initialization.
    if (sector_size[drive] != initialized_sector_size[drive])
        return false;

    // Overflow-safe sector range check.
    if (address >= drive_size[drive] ||
        (uint32_t)count > drive_size[drive] - address)
    {
        return false;
    }

    return true;
}

//
// Initialize floppy drive layout.
//
qmi_floppy_result_t
qmi_floppy_init(void)
{
    const uint32_t capacity[NUM_DRIVE] = {
        DRIVE_A_SIZE,
        DRIVE_B_SIZE
    };

    uint32_t new_drive_size[NUM_DRIVE];
    uint32_t new_drive_base[NUM_DRIVE];
    uint32_t cursor;
    int drive;

    if (floppy_operation != FLOPPY_OP_NONE)
        return QMI_FLOPPY_RESULT_ERROR;

    // Initialization must succeed before read/write is permitted.
    floppy_initialized = false;

    // Check the configuration.
    for (drive = 0;
         drive < NUM_DRIVE;
         drive++)
    {
        if (sector_size[drive] != 128 &&
            sector_size[drive] != 256)
        {
            return QMI_FLOPPY_RESULT_ERROR;
        }

        if (capacity[drive] == 0 ||
            capacity[drive] % sector_size[drive] != 0 ||
            capacity[drive] % SECTOR_SIZE != 0)
        {
            return QMI_FLOPPY_RESULT_ERROR;
        }

        new_drive_size[drive] =
            capacity[drive] / sector_size[drive];
    }

    // Allocate Drive B at the end of Flash,
    // with Drive A immediately before it.
    cursor = FLASH_SIZE;

    for (drive = NUM_DRIVE - 1;
         drive >= 0;
         drive--)
    {
        if (capacity[drive] > cursor)
            return QMI_FLOPPY_RESULT_ERROR;

        cursor -= capacity[drive];

        new_drive_base[drive] = cursor;
    }

    // Check 4KB alignment.
    for (drive = 0;
         drive < NUM_DRIVE;
         drive++)
    {
        if (new_drive_base[drive] % SECTOR_SIZE != 0)
            return QMI_FLOPPY_RESULT_ERROR;

        if (!qmi_flash_check_range(
                new_drive_base[drive],
                capacity[drive]))
        {
            return QMI_FLOPPY_RESULT_ERROR;
        }
    }

    // Commit the validated configuration.
    for (drive = 0;
         drive < NUM_DRIVE;
         drive++)
    {
        drive_base[drive] = new_drive_base[drive];
        drive_size[drive] = new_drive_size[drive];
        initialized_sector_size[drive] = sector_size[drive];
    }

    floppy_initialized = true;

    return QMI_FLOPPY_RESULT_OK;
}

qmi_floppy_result_t
qmi_floppy_read(
    int drive,
    uint32_t address,
    int count,
    uint8_t *buffer)
{
    uint32_t flash_address;
    int i;

    if (floppy_operation != FLOPPY_OP_NONE)
        return QMI_FLOPPY_RESULT_ERROR;

    if (!check_request(drive, address, count, buffer))
        return QMI_FLOPPY_RESULT_ERROR;

    floppy_operation = FLOPPY_OP_READ;

    for (i = 0; i < count; i++)
    {
        flash_address =
            flash_address_from_sector(
                drive,
                address + (uint32_t)i);

        if (!qmi_flash_read_start(
                flash_address,
                buffer,
                sector_size[drive]))
        {
            floppy_operation = FLOPPY_OP_NONE;
            return QMI_FLOPPY_RESULT_ERROR;
        }

        if (!wait_poll(qmi_flash_read_poll))
        {
            floppy_operation = FLOPPY_OP_NONE;
            return QMI_FLOPPY_RESULT_ERROR;
        }

        buffer += sector_size[drive];
    }

    floppy_operation = FLOPPY_OP_NONE;

    return QMI_FLOPPY_RESULT_OK;
}

qmi_floppy_result_t
qmi_floppy_write(
    int drive,
    uint32_t address,
    int count,
    uint8_t *buffer)
{
    int i;

    if (floppy_operation != FLOPPY_OP_NONE)
        return QMI_FLOPPY_RESULT_ERROR;

    if (!check_request(drive, address, count, buffer))
        return QMI_FLOPPY_RESULT_ERROR;

    floppy_operation = FLOPPY_OP_WRITE;

    for (i = 0; i < count; i++)
    {
        if (!write_one_sector(
                drive,
                address + (uint32_t)i,
                buffer))
        {
            floppy_operation = FLOPPY_OP_NONE;
            return QMI_FLOPPY_RESULT_ERROR;
        }

        buffer += sector_size[drive];
    }

    floppy_operation = FLOPPY_OP_NONE;

    return QMI_FLOPPY_RESULT_OK;
}
