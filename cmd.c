//
// cmd.c ... simple command processor
//

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "cmd.h"
#include "conf.h"
#include "memprt.h"
#include "qmi_flash.h"
#include "support.h"

#include "protothread.h"
#include "pt_qmi.h"
#include "pt_qmi_floppy.h"

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
static uint8_t sector_buffer[256];

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

static void message(const char *format, ...) {
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
    message("  e ADDRESS       Erase 4 KiB sector\n");
    message("  w ADDRESS       Write buffer to Flash (4 KiB)\n");
    message("  fr DRIVE LBA     Read one floppy sector into sector_buffer\n");
    message("  fw DRIVE LBA     Write one floppy sector from sector_buffer\n");
    message("  d [OFF [LEN]]   Dump flash_buffer\n");
    message("  df [OFF [LEN]]  Dump sector_buffer\n");
    message("  f VALUE         Fill flash_buffer with VALUE (00-FF or rand)\n");
    message("  ff VALUE        Fill sector_buffer with VALUE (00-FF or rand)\n");
    message("  cf              CRC-16 of sector_buffer (256 bytes)\n");
    message("  s OFF VALUE     Set one byte in buffer\n");
    message("  h               Show this help\n");
    message("  x               flush debug out\n");
    message("  c [SEED]        calc CRC-16\n");
    message("  ?               Show this help\n");
    message("\n");
    message("ADDRESS : hexadecimal; r/R/e/E/w use Drive A byte offsets\n");
    message("          r/R #ADDRESS uses absolute Flash byte address\n");
    message("          fr/fw LBA is a logical sector number (not byte offset)\n");
    message("DRIVE   : 0=A, 1=B (hexadecimal)\n");
    
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
                                    uint32_t *address) {
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

static void command_read(const char *arg, int async_flag) {
    uint32_t address;
    uint32_t offset;
    uint32_t time0, time1, time2;

    if (!parse_raw_flash_address(arg, false, &address))
        return;

    message("Reading buffer to Flash (TEST) 0x%08lX ...\n", (unsigned long)address);
    while (!pt_issue_ready())
        tight_loop_contents();
    time1 = time_us_32();
    if (!pt_issue_read(address, &flash_buffer[0], SECTOR_SIZE)) {
        print_error("Write start failed\n");
        return;
    }
    while (!pt_is_done())
        tight_loop_contents();

    time2 = time_us_32();
    message("time2: %8ld\n", time2 - time1);
    message("Write completed.\n");
}

// ------------------------------------------------------------
// Erase
// ------------------------------------------------------------

static void command_erase(const char *arg) {
    uint32_t address;
    uint32_t time0, time1, time2;

    if (!parse_raw_flash_address(arg, false, &address))
        return;

    message("Erasing (TEST) sector at 0x%08lX ...\n", (unsigned long)address);

    time0 = time_us_32();

    while (!pt_issue_ready())
        tight_loop_contents();
    time1 = time_us_32();
    if (!pt_issue_erase(address)) {
        print_error("pt_issue_erase: failed\n");
        return;
    }
    while (!pt_is_done())
        tight_loop_contents();

    time2 = time_us_32();
    int err = pt_error_code();
    message("error: %d\n", err);
    message("time1: %8ld %8ld\n", time1 - time0, time0);
    message("time2: %8ld %8ld\n", time2 - time1, time1);
    message("Erase completed.\n");
    pt_dump_polltime();
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

    message("Writing buffer to Flash (TEST) 0x%08lX ...\n", (unsigned long)address);
    // Page Program must not cross a 256-byte page boundary.
    time0 = time_us_64();
    for (offset = 0; offset < SECTOR_SIZE; offset += PAGE_SIZE) {
        while (!pt_issue_ready())
            tight_loop_contents();
        time1 = time_us_64();
        if (!pt_issue_program(address + offset, &flash_buffer[offset], PAGE_SIZE)) {
            print_error("Write start failed\n");
            return;
        }
        while (!pt_is_done())
            tight_loop_contents();

        time2 = time_us_64();
        message("time2: %8lld\n", time2 - time1);
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

static bool floppy_parse_request(const char *drive_arg, const char *lba_arg,
                                 int *drive, uint32_t *lba) {
    uint32_t d;
    if (!floppy_ready) {
        print_error("Floppy initialization failed");
        return false;
    }
    if (!parse_hex(drive_arg, &d) || d >= NUM_DRIVE) {
        print_error("Invalid floppy drive (0=A, 1=B)");
        return false;
    }
    if (!parse_hex(lba_arg, lba)) {
        print_error("Invalid floppy LBA");
        return false;
    }
    *drive = (int)d;
    if (sector_size[*drive] == 0 || sector_size[*drive] > sizeof(sector_buffer) ||
        *lba >= drive_size[*drive]) {
        print_error("Floppy LBA out of range");
        return false;
    }
    return true;
}

static void command_floppy_read_test(const char *drive_arg, const char *lba_arg) {
    int drive;
    uint32_t lba;
    if (!floppy_parse_request(drive_arg, lba_arg, &drive, &lba)) return;

    while (!pt_floppy_issue_ready()) tight_loop_contents();
    uint64_t time1 = time_us_64();
    if (!pt_issue_floppy_read(drive, lba, &sector_buffer[0])) {
        print_error("Floppy read issue failed");
        return;
    }
    while (!pt_floppy_is_done()) tight_loop_contents();
    uint64_t elapsed = time_us_64() - time1;
    int error = pt_floppy_error_code();
    if (error != PT_FLOPPY_RESULT_OK) {
        message("Floppy read ERROR: drive=%d, lba=%08lX, error=%d, elapsed=%llu us\n",
                drive, (unsigned long)lba, error, (unsigned long long)elapsed);
        return;
    }
    message("Floppy read OK: drive=%d, lba=%08lX, bytes=%lu, elapsed=%llu us\n",
            drive, (unsigned long)lba, (unsigned long)sector_size[drive],
            (unsigned long long)elapsed);
}

static void command_floppy_write_test(const char *drive_arg, const char *lba_arg) {
    int drive;
    uint32_t lba;
    if (!floppy_parse_request(drive_arg, lba_arg, &drive, &lba)) return;

    while (!pt_floppy_issue_ready()) tight_loop_contents();
    uint64_t time1 = time_us_64();
    if (!pt_issue_floppy_write(drive, lba, &sector_buffer[0])) {
        print_error("Floppy write issue failed");
        return;
    }
    while (!pt_floppy_is_done()) tight_loop_contents();
    uint64_t elapsed = time_us_64() - time1;
    int error = pt_floppy_error_code();
    if (error != PT_FLOPPY_RESULT_OK) {
        message("Floppy write ERROR: drive=%d, lba=%08lX, error=%d, elapsed=%llu us\n",
                drive, (unsigned long)lba, error, (unsigned long long)elapsed);
        return;
    }
    message("Floppy write OK: drive=%d, lba=%08lX, bytes=%lu, elapsed=%llu us\n",
            drive, (unsigned long)lba, (unsigned long)sector_size[drive],
            (unsigned long long)elapsed);
}

// Sector-buffer utility commands (the original flash-buffer commands remain unchanged).
static void command_sector_fill(const char *arg) {
    uint32_t value;
    if (arg && strcmp(arg, "rand") == 0) {
        fill_random(sector_buffer, sizeof(sector_buffer), 12345678u);
        message("Sector buffer filled randomly\n");
        return;
    }
    if (!parse_hex(arg, &value) || value > 0xFFu) {
        print_error("Value must be 00-FF or rand");
        return;
    }
    memset(sector_buffer, (uint8_t)value, sizeof(sector_buffer));
    message("Sector buffer filled with %02lX\n", (unsigned long)value);
}

static void command_sector_crc16(void) {
    // The cf command calculates CRC over the entire 256-byte sector buffer.
    uint16_t crc = crc16_ccitt_false(sector_buffer, sizeof(sector_buffer));
    message("Sector buffer CRC16 (256 bytes): %04x\n", crc);
}

static void command_sector_dump(const char *arg_offset, const char *arg_length) {
    static uint32_t next_address = 0;
    uint32_t offset = 0; //next_address;
    uint32_t length = 256;
    if (arg_offset && !parse_hex(arg_offset, &offset)) {
        print_error("Invalid sector buffer offset");
        return;
    }
    if (arg_length && !parse_hex(arg_length, &length)) {
        print_error("Invalid sector buffer length");
        return;
    }
    if (offset >= sizeof(sector_buffer)) {
        print_error("Sector buffer offset out of range");
        return;
    }
    if (length > sizeof(sector_buffer) - offset)
        length = sizeof(sector_buffer) - offset;
    for (uint32_t i = 0; i < length; i += 16) {
        uint32_t n = length - i;
        if (n > 16) n = 16;
        message("%04lX: ", (unsigned long)(offset + i));
        for (uint32_t j = 0; j < n; ++j)
            message("%02X ", sector_buffer[offset + i + j]);
        message("\n");
    }
    //next_address = offset + length;
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
        if (argc != 3) {
            print_error("Usage: fr|fw DRIVE LBA");
            return;
        }
        if (strcmp(argv[0], "fr") == 0)
            command_floppy_read_test(argv[1], argv[2]);
        else
            command_floppy_write_test(argv[1], argv[2]);
    } else if (strcmp(argv[0], "r") == 0) {
        if (argc != 2) {
            print_error("Usage: r ADDRESS");
            return;
        }
        command_read(argv[1], false);
    } else if (strcmp(argv[0], "e") == 0) {
        if (argc != 2) {
            print_error("Usage: e ADDRESS");
            return;
        }
        command_erase(argv[1]);
    } else if (strcmp(argv[0], "w") == 0) {
        if (argc != 2) {
            print_error("Usage: w ADDRESS");
            return;
        }
        command_write(argv[1], true);
    } else if (strcmp(argv[0], "df") == 0) {
        if (argc > 3) {
            print_error("Usage: df [OFFSET [LENGTH]]");
            return;
        }
        command_sector_dump(argc >= 2 ? argv[1] : NULL,
                            argc >= 3 ? argv[2] : NULL);
    } else if (strcmp(argv[0], "ff") == 0) {
        if (argc != 2) {
            print_error("Usage: ff VALUE");
            return;
        }
        command_sector_fill(argv[1]);
    } else if (strcmp(argv[0], "cf") == 0) {
        if (argc != 1) {
            print_error("Usage: cf");
            return;
        }
        command_sector_crc16();
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
        command_crc16(argc >= 2 ? argv[1] : NULL);
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
    memset(sector_buffer, 0xFF, sizeof(sector_buffer));

    // Initialize floppy test configuration before accepting commands.
    sector_size[0] = FLOPPY_TEST_SECTOR_SIZE_A;
    sector_size[1] = FLOPPY_TEST_SECTOR_SIZE_B;
    floppy_ready = pt_qmi_floppy_init();

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
