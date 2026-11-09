//
// cmd.c ... simple command processor
//

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "cmd.h"
#include "conf.h"
#include "memprt.h"
#include "qmi_flash.h"
#include "qmi_flash_async.h"
#include "qmi_flash_sync.h"
#include "qmi_floppy.h"
#include "support.h"

// ------------------------------------------------------------
// QMI Flash functions
//
// Implement these functions using the existing QMI Direct Mode
// routines. Each function must return true on success.
//
// The functions must restore the QMI/XIP state before returning.
// ------------------------------------------------------------

#include "conf.h"
#include "qmi_flash.h"

//
// SECTOR_SIZE should be a multiple of PAGE_SIZE
//
_Static_assert(SECTOR_SIZE % PAGE_SIZE == 0,
               "SECTOR_SIZE must be a multiple of PAGE_SIZE");

// ------------------------------------------------------------
// RAM buffer
// ------------------------------------------------------------

//
// BUFFER_SIZE is one SECTOR_SIZE length
//
static uint8_t flash_buffer[SECTOR_SIZE];

// ------------------------------------------------------------
// Utility functions
// ------------------------------------------------------------

static bool parse_hex(const char *str, uint32_t *value) {
    char *end;
    unsigned long v;

    if (str == NULL || *str == '\0')
        return false;

    v = strtoul(str, &end, 16);

    if (*end != '\0' || v > 0xFFFFFFFFul)
        return false;

    *value = (uint32_t)v;
    return true;
}

static bool check_flash_address(uint32_t address) {
    return address < FLASH_SIZE;
}

static void print_error(const char *message) {
    printf("ERROR: %s\n", message);
}

static void message(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int len = vprintf(format, args);
    va_end(args);
}

// ------------------------------------------------------------
// Raw Flash commands:
//   r/R OFFSET  : byte offset from Drive A base
//   r/R #ADDRESS: absolute Flash byte address
//   e/E/w OFFSET: byte offset from Drive A base
// ------------------------------------------------------------

// ------------------------------------------------------------
// Help
// ------------------------------------------------------------

static void command_help(void) {
    message("\n");
    message("Commands:\n");
    message("  r ADDRESS       Read 4 KiB at Drive A byte offset\n");
    message("  r #ADDRESS      Read 4 KiB at absolute Flash address\n");
    message("  R ADDRESS       Stable Read (same address rules)\n");
    message("  e ADDRESS       Erase 4 KiB sector\n");
    message("  w ADDRESS       Write buffer to Flash (4 KiB)\n");
    message("  fr DRIVE ADDRESS [COUNT]  Floppy read into buffer\n");
    message("  fw DRIVE ADDRESS [COUNT]  Floppy write from buffer\n");
    message("  d [OFF [LEN]]   Dump buffer\n");
    message("  f VALUE         Fill buffer with VALUE (00-FF)\n");
    message("  s OFF VALUE     Set one byte in buffer\n");
    message("  h               Show this help\n");
    message("  x               flush debug out\n");
    message("  y               toggle debug_enable\n");
    message("  c [SEED]        calc CRC-16\n");
    message("  ?               Show this help\n");
    message("\n");
    message("ADDRESS : hexadecimal; r/R/e/E/w use Drive A byte offsets\n");
    message("          r/R #ADDRESS uses absolute Flash byte address\n");
    message("          fr/fw ADDRESS is a logical sector number\n");
    message("DRIVE   : 0=A, 1=B (hexadecimal)\n");
    message("COUNT   : number of floppy sectors (hexadecimal, default 1)\n");
    message("OFF     : buffer offset (hexadecimal)\n");
    message("VALUE   : byte value (hexadecimal)\n");
    message("\n");
}

// ------------------------------------------------------------
// Read
// ------------------------------------------------------------

// Drive A occupies the lower of the two reserved floppy areas.
// This matches qmi_floppy_init()'s confirmed layout.
#define RAW_DRIVE_A_BASE (FLASH_SIZE - DRIVE_A_SIZE - DRIVE_B_SIZE)

_Static_assert(DRIVE_A_SIZE + DRIVE_B_SIZE <= FLASH_SIZE,
               "Floppy drive areas exceed Flash size");

static bool parse_raw_flash_address(const char *arg,
                                    bool allow_absolute,
                                    uint32_t *address)
{
    uint32_t offset;
    bool absolute;

    if (arg == NULL || address == NULL) {
        print_error("Invalid address");
        return false;
    }

    absolute = (arg[0] == '#');
    if (absolute && !allow_absolute) {
        print_error("Absolute address is not supported for this command");
        return false;
    }

    if (!parse_hex(absolute ? arg + 1 : arg, &offset)) {
        print_error("Invalid address");
        return false;
    }

    // Both modes operate on one complete 4 KiB erase sector.
    if ((offset & (SECTOR_SIZE - 1u)) != 0) {
        print_error("Address must be 4 KiB aligned");
        return false;
    }

    if (absolute) {
        if (FLASH_SIZE < SECTOR_SIZE ||
            offset > FLASH_SIZE - SECTOR_SIZE) {
            print_error("Address out of Flash range");
            return false;
        }
        *address = offset;
    } else {
        if (DRIVE_A_SIZE < SECTOR_SIZE ||
            offset > (DRIVE_A_SIZE + DRIVE_B_SIZE) - SECTOR_SIZE) {
            printf("offset = %lx\n", (unsigned long)offset);
            print_error("Address out of Drive A range");
            return false;
        }
        *address = RAW_DRIVE_A_BASE + offset;
    }

    return true;
}

static void command_read(const char *arg, int stable_flag) {
    uint32_t address;
    uint64_t time0, time1, time2;

    if (!parse_raw_flash_address(arg, true, &address))
        return;

    message("Reading Flash 0x%08lX ...\n", (unsigned long)address);

    time0 = time_us_64();
    if (stable_flag) {
#if 1
        // stable mode
        // use: qmi_flash_read_start_stable
        //      qmi_flash_read_poll_stalbe
        if (!qmi_flash_read_start_stable(address, flash_buffer, SECTOR_SIZE)) {
            print_error("Read start failed\n");
            return;
        }
        time1 = time_us_64();

        while (true) {
            qmi_flash_poll_result_t result = qmi_flash_read_poll_stable();
            if (result == QMI_FLASH_POLL_DONE)
                break;
            if (result == QMI_FLASH_POLL_ERROR) {
                print_error("Read failed\n");
                return;
            }
            tight_loop_contents();
        }
        time2 = time_us_64();
        message("time1: %8lld\n", time1 - time0);
        message("time2: %8lld\n", time2 - time1);
#else
        printf_error("read stable not supported");
#endif
    } else {
        // stable mode
        // use: qmi_flash_read_start_stable
        //      qmi_flash_read_poll_stalbe
        if (!qmi_flash_read_start(address, flash_buffer, SECTOR_SIZE)) {
            print_error("Read start failed\n");
            return;
        }
        time1 = time_us_64();

        while (true) {
            qmi_flash_poll_result_t result = qmi_flash_read_poll();
            if (result == QMI_FLASH_POLL_DONE)
                break;
            if (result == QMI_FLASH_POLL_ERROR) {
                print_error("Read failed\n");
                return;
            }
            tight_loop_contents();
        }
        time2 = time_us_64();
        message("time1: %8lld\n", time1 - time0);
        message("time2: %8lld\n", time2 - time1);
    }
    message("Read completed.\n");
}

// ------------------------------------------------------------
// Erase
// ------------------------------------------------------------

static void command_erase(const char *arg, int async_flag) {
    uint32_t address;
    uint64_t time0, time1, time2;

    if (!parse_raw_flash_address(arg, false, &address))
        return;

    message("Erasing sector at 0x%08lX ...\n", (unsigned long)address);

    time0 = time_us_64();
    if (async_flag) {
        if (!qmi_flash_erase_4k_start(address)) {
            print_error("Erase start failed\n");
            return;
        }
        time1 = time_us_64();
        while (true) {
            qmi_flash_poll_result_t result = qmi_flash_erase_4k_poll();

            if (result == QMI_FLASH_POLL_DONE)
                break;

            if (result == QMI_FLASH_POLL_ERROR) {
                print_error("Erase failed or timed out\n");
                return;
            }

            tight_loop_contents();
        }
        time2 = time_us_64();
        message("time1: %8lld\n", time1 - time0);
        message("time2: %8lld\n", time2 - time0);

    } else {
        if (!qmi_flash_erase_4k(address)) {
            print_error("Flash erase failed");
            return;
        }
        time2 = time_us_64();
        message("time1: %8lld\n", time1 - time0);
    }
    message("Erase completed.\n");
}

// ------------------------------------------------------------
// Write
// ------------------------------------------------------------

static void command_write(const char *arg, int async_flag) {
    uint32_t address;
    uint32_t offset;
    uint64_t time0, time1, time2;

    if (!parse_raw_flash_address(arg, false, &address))
        return;

    message("Writing buffer to Flash 0x%08lX ...\n", (unsigned long)address);
    // Page Program must not cross a 256-byte page boundary.
    time0 = time_us_64();
    for (offset = 0; offset < SECTOR_SIZE; offset += PAGE_SIZE) {
        if (async_flag) {
            if (!qmi_flash_program_start(address + offset, &flash_buffer[offset], PAGE_SIZE)) {
                print_error("Write start failed\n");
                return;
            }
            time1 = time_us_64();
            while (true) {
                qmi_flash_poll_result_t result = qmi_flash_program_poll();

                if (result == QMI_FLASH_POLL_DONE)
                    break;

                if (result == QMI_FLASH_POLL_ERROR) {
                    print_error("Write failed or timed out\n");
                    return;
                }
                // message("time1: %8lld\n", time1 - time0);
                tight_loop_contents();
            }
            time2 = time_us_64();
            message("time2: %8lld\n", time2 - time1);
        } else {
            if (!qmi_flash_program_page(address + offset, &flash_buffer[offset], PAGE_SIZE)) {
                message("ERROR: Write failed at 0x%08lX\n", (unsigned long)(address + offset));
                return;
            }
            time2 = time_us_64();
            message("time2: %8lld\n", time2 - time0);
        }
    }
    message("Write completed.\n");
}

// ------------------------------------------------------------
// Floppy synchronous read/write
// ------------------------------------------------------------

// Test configuration. Change these values to 128 if needed.
#define FLOPPY_TEST_SECTOR_SIZE_A 256u
#define FLOPPY_TEST_SECTOR_SIZE_B 256u

static bool floppy_ready = false;

static void command_floppy(const char *drive_arg,
                           const char *address_arg,
                           const char *count_arg,
                           bool write_flag)
{
    uint32_t drive_value, address, count = 1;
    uint32_t bytes;
    int drive;
    uint64_t time0, elapsed;
    qmi_floppy_result_t result;

    if (!floppy_ready) {
        print_error("Floppy initialization failed");
        return;
    }

    if (!parse_hex(drive_arg, &drive_value) ||
        drive_value >= NUM_DRIVE) {
        print_error("Invalid floppy drive (0=A, 1=B)");
        return;
    }
    drive = (int)drive_value;

    if (!parse_hex(address_arg, &address)) {
        print_error("Invalid floppy sector address");
        return;
    }

    if (count_arg != NULL && !parse_hex(count_arg, &count)) {
        print_error("Invalid floppy sector count");
        return;
    }

    if (count == 0 || count > INT_MAX ||
        sector_size[drive] == 0 ||
        count > sizeof(flash_buffer) / sector_size[drive]) {
        print_error("Floppy count exceeds buffer capacity");
        return;
    }

    // Avoid unsigned overflow in address + count.
    if (address >= drive_size[drive] ||
        count > drive_size[drive] - address) {
        print_error("Floppy sector range out of drive");
        return;
    }

    bytes = count * sector_size[drive];
    message("Floppy %s: drive=%lu, sector=0x%08lX, count=0x%lX, bytes=%lu\n",
            write_flag ? "write" : "read",
            (unsigned long)drive_value,
            (unsigned long)address,
            (unsigned long)count,
            (unsigned long)bytes);

    time0 = time_us_64();
    if (write_flag) {
        result = qmi_floppy_write(drive, address, (int)count, flash_buffer);
    } else {
        result = qmi_floppy_read(drive, address, (int)count, flash_buffer);
    }
    elapsed = time_us_64() - time0;

    message("Floppy %s: %s, elapsed=%llu us\n",
            write_flag ? "write" : "read",
            result == QMI_FLOPPY_RESULT_OK ? "OK" : "ERROR",
            (unsigned long long)elapsed);
}

// ------------------------------------------------------------
// Dump
// ------------------------------------------------------------

static void command_dump(const char *arg_offset, const char *arg_length) {
    uint32_t offset = 0;
    uint32_t length = 256;
    uint32_t i;
    static uint32_t next_address = 0;

    offset = next_address;
    if (arg_offset != NULL) {
        if (!parse_hex(arg_offset, &offset)) {
            print_error("Invalid offset");
            return;
        }
    }

    if (arg_length != NULL) {
        if (!parse_hex(arg_length, &length)) {
            print_error("Invalid length");
            return;
        }
    }

    if (offset >= SECTOR_SIZE) {
        print_error("Offset out of buffer range");
        return;
    }

    if (length > SECTOR_SIZE - offset)
        length = SECTOR_SIZE - offset;

    for (i = 0; i < length; i += 16) {
        uint32_t j;
        uint32_t count = length - i;

        if (count > 16)
            count = 16;
        message("%04lX: ", (unsigned long)(offset + i));
        for (j = 0; j < count; j++) {
            message("%02X ", flash_buffer[offset + i + j]);
        }
        message("\n");
    }
    next_address = offset + length;
}

// ------------------------------------------------------------
// Fill
// ------------------------------------------------------------

static void command_fill(const char *arg) {
    uint32_t value;

    if (arg && arg[0] == 'r') {
        fill_random(flash_buffer, SECTOR_SIZE, 12345678u);
        message("Buffer filled randomly\n");
        return;
    }
    if (!parse_hex(arg, &value) || value > 0xFF) {
        print_error("Value must be 00-FF");
        return;
    }
    memset(flash_buffer, (uint8_t)value, SECTOR_SIZE);
    message("Buffer filled with %02lX.\n", (unsigned long)value);
}

// ------------------------------------------------------------
// CRC-16
// ------------------------------------------------------------

static void command_crc16(const char *seed) {
    uint16_t crc = crc16_ccitt_false(flash_buffer, SECTOR_SIZE);
    message("CRC16: %04x\n", crc);
}

// ------------------------------------------------------------
// Set one byte
// ------------------------------------------------------------

static void command_set(const char *arg_offset, const char *arg_value) {
    uint32_t offset;
    uint32_t value;

    if (!parse_hex(arg_offset, &offset)) {
        print_error("Invalid offset");
        return;
    }
    if (!parse_hex(arg_value, &value) || value > 0xFF) {
        print_error("Value must be 00-FF");
        return;
    }
    if (offset >= SECTOR_SIZE) {
        print_error("Offset out of buffer range");
        return;
    }
    flash_buffer[offset] = (uint8_t)value;
    message("Buffer[%04lX] = %02lX\n", (unsigned long)offset, (unsigned long)value);
}

// ------------------------------------------------------------
// Command interpreter
// ------------------------------------------------------------

static void execute_command(char *line) {
    char *argv[4];
    int argc = 0;

    char *token = strtok(line, " \t");

    while (token != NULL && argc < 4) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }

    if (argc == 0)
        return;

    if (strcmp(argv[0], "fr") == 0 || strcmp(argv[0], "fw") == 0) {
        if (argc != 3 && argc != 4) {
            print_error("Usage: fr|fw DRIVE ADDRESS [COUNT]");
            return;
        }
        command_floppy(argv[1], argv[2], argc == 4 ? argv[3] : NULL,
                       strcmp(argv[0], "fw") == 0);
    } else if (strcmp(argv[0], "r") == 0) {
        if (argc != 2) {
            print_error("Usage: r ADDRESS");
            return;
        }
        command_read(argv[1], false);
    } else if (strcmp(argv[0], "R") == 0) {
        if (argc != 2) {
            print_error("Usage: R ADDRESS");
            return;
        }
        command_read(argv[1], true);
    } else if (strcmp(argv[0], "e") == 0) {
        if (argc != 2) {
            print_error("Usage: e ADDRESS");
            return;
        }
        command_erase(argv[1], true);
    } else if (strcmp(argv[0], "E") == 0) {
        if (argc != 2) {
            print_error("Usage: e ADDRESS");
            return;
        }
        command_erase(argv[1], false);
    } else if (strcmp(argv[0], "w") == 0) {
        if (argc != 2) {
            print_error("Usage: w ADDRESS");
            return;
        }
        command_write(argv[1], true);
    } else if (strcmp(argv[0], "d") == 0) {
        uint32_t addr = 0, len = 256;
        if (argc > 3) {
            print_error("Usage: d [OFFSET [LENGTH]]");
            return;
        }
        command_dump(argc >= 2 ? argv[1] : NULL, argc >= 3 ? argv[2] : NULL);
    } else if (strcmp(argv[0], "f") == 0) {
        if (argc != 2) {
            print_error("Usage: f VALUE");
            return;
        }
        command_fill(argv[1]);
    } else if (strcmp(argv[0], "s") == 0) {
        if (argc != 3) {
            print_error("Usage: s OFFSET VALUE");
            return;
        }
        command_set(argv[1], argv[2]);
    } else if (strcmp(argv[0], "h") == 0 ||
               strcmp(argv[0], "?") == 0) {
        command_help();
    } else if (strcmp(argv[0], "x") == 0) {
        if (argc == 2) {
            message("clear buffer");
            memprt_clear();
        } else {
            message("flush_buffer:");
            memprt_flush();
        }
    } else if (strcmp(argv[0], "c") == 0) {
        command_crc16(argv[1]);
    } else if (strcmp(argv[0], "y") == 0) {
        message("toggle debug_error: %s\n", toggle_debug_error() ? "true" : "false");
    } else {
        print_error("Unknown command");
    }
}

// ------------------------------------------------------------
// UART line input
// ------------------------------------------------------------

void command_loop(void) {
    char line[LINE_SIZE];
    size_t length;

    // clear buffer
    memset(flash_buffer, 0xFF, sizeof(flash_buffer));

    // Initialize floppy test configuration before accepting commands.
    sector_size[0] = FLOPPY_TEST_SECTOR_SIZE_A;
    sector_size[1] = FLOPPY_TEST_SECTOR_SIZE_B;
    floppy_ready = (qmi_floppy_init() == QMI_FLOPPY_RESULT_OK);

    message("\nQMI Flash monitor\n");
    if (floppy_ready) {
        message("Floppy initialized: A=%lu x %lu, B=%lu x %lu bytes\n",
                (unsigned long)drive_size[0], (unsigned long)sector_size[0],
                (unsigned long)drive_size[1], (unsigned long)sector_size[1]);
    } else {
        print_error("Floppy initialization failed; fr/fw disabled");
    }
    command_help();
    test_crc16();

    while (true) {

        message("> ");
        fflush(stdout);

        length = 0;

        while (true) {
            char c = uart_getc(UART_ID);
            if (c == '\r' || c == '\n') {
                uart_putc_raw(UART_ID, '\r');
                uart_putc_raw(UART_ID, '\n');
                break;
            }
            // Backspace / Delete
            if (c == '\b' || c == 0x7F) {
                if (length > 0) {
                    length--;
                    uart_putc_raw(UART_ID, '\b');
                    uart_putc_raw(UART_ID, ' ');
                    uart_putc_raw(UART_ID, '\b');
                }
                continue;
            }
            if (length < LINE_SIZE - 1) {
                line[length++] = c;
                uart_putc_raw(UART_ID, c);
            }
        }
        line[length] = '\0';
        execute_command(line);
    }
}
