//
// cmd.c ... simple command processor
//

#include <ctype.h>
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
#include "qmi_flash_async.h"
#include "qmi_flash_sync.h"

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

// --------------------------------------------
// FLASH_SIZE - 128kB を r/w/e コマンドのベースオフセットとする。
// rコマンドのアドレス先頭文字が #の時は絶対アドレス(使用領域からも読み込める)と
// する。
// --------------------------------------------

// ------------------------------------------------------------
// Help
// ------------------------------------------------------------

static void command_help(void) {
    printf("\n");
    printf("Commands:\n");
    printf("  r ADDRESS       Read 4 KiB sector into buffer\n");
    printf("  e ADDRESS       Erase 4 KiB sector\n");
    printf("  w ADDRESS       Write buffer to Flash (4 KiB)\n");
    printf("  d [OFF [LEN]]   Dump buffer\n");
    printf("  f VALUE         Fill buffer with VALUE (00-FF)\n");
    printf("  s OFF VALUE     Set one byte in buffer\n");
    printf("  h               Show this help\n");
    printf("  x               flush debug out\n");
    printf("  y               toggle debug_enable\n");
    printf("  ?               Show this help\n");
    printf("\n");
    printf("ADDRESS : 6-digit or 8-digit hexadecimal\n");
    printf("OFF     : buffer offset (hexadecimal)\n");
    printf("VALUE   : byte value (hexadecimal)\n");
    printf("\n");
}

// ------------------------------------------------------------
// Read
// ------------------------------------------------------------

#define USER_FLASH_SIZE 0x20000u

static uint32_t base_offset = FLASH_SIZE - USER_FLASH_SIZE;

static bool parse_user_address(const char *arg, uint32_t *address) {
    uint32_t offset;

    if (arg == NULL) {
        print_error("Invalid arg pointer");
        return false;
    }

    if (!parse_hex(arg, &offset)) {
        print_error("Invalid address");
        return false;
    }

    if (offset > USER_FLASH_SIZE - SECTOR_SIZE) {
        print_error("Address out of usable range");
        return false;
    }

    *address = base_offset + offset;
    return true;
}

static void command_read(const char *arg, int async_flag) {
    uint32_t address;
    const char *start = arg;
    int absolute_flag = false;
    uint64_t time0, time1, time2;

    if (arg == NULL) {
        print_error("Bad arg pointer");
        return;
    }
    if (arg[0] == '#') {
        // absolute address read
        if (parse_user_address(arg + 1, &address) == false) {
            print_error("Invalid address");
            return;
        }
        address -= base_offset;
    } else {
        if (parse_user_address(arg, &address) == false) {
            print_error("Invalid address");
            return;
        }
    }

    if ((address & (SECTOR_SIZE - 1)) != 0) {
        print_error("Address must be 4 KiB aligned");
        return;
    }

    printf("Reading Flash 0x%08lX ...\n", (unsigned long)address);

    time0 = time_us_64();
    if (async_flag) {
        // async mode
        // use: qmi_flash_read_start
        //      qmi_flash_read_poll
        if (!qmi_flash_read_start(address, flash_buffer, SECTOR_SIZE)) {
            printf("Read start failed\n");
            return;
        }
        time1 = time_us_64();

        while (true) {
            qmi_flash_poll_result_t result = qmi_flash_read_poll();
            if (result == QMI_FLASH_POLL_DONE)
                break;
            if (result == QMI_FLASH_POLL_ERROR) {
                printf("Read failed\n");
                return;
            }
            tight_loop_contents();
        }
        time2 = time_us_64();
        printf("time1: %8lld\n", time1 - time0);
        printf("time2: %8lld\n", time2 - time0);
    } else {
        // sync mode
        // use: qmi_flash_read
        if (!qmi_flash_read(address, flash_buffer, SECTOR_SIZE)) {
            print_error("Flash read failed");
            return;
        }
        time2 = time_us_64();
        printf("time2: %8lld\n", time2 - time0);
    }
    printf("Read completed.\n");
}

// ------------------------------------------------------------
// Erase
// ------------------------------------------------------------

static void command_erase(const char *arg, int async_flag) {
    uint32_t address;
    uint64_t time0, time1, time2;

    if (!parse_hex(arg, &address)) {
        print_error("Invalid address");
        return;
    }

    address += base_offset;

    if ((address & (SECTOR_SIZE - 1)) != 0) {
        print_error("Address must be 4 KiB aligned");
        return;
    }

    if (address > FLASH_SIZE - SECTOR_SIZE) {
        print_error("Address out of Flash range");
        return;
    }

    printf("Erasing sector at 0x%08lX ...\n", (unsigned long)address);

    time0 = time_us_64();
    if (async_flag) {
        if (!qmi_flash_erase_4k_start(address)) {
            printf("Erase start failed\n");
            return;
        }
        time1 = time_us_64();
        while (true) {
            qmi_flash_poll_result_t result = qmi_flash_erase_poll();

            if (result == QMI_FLASH_POLL_DONE)
                break;

            if (result == QMI_FLASH_POLL_ERROR) {
                printf("Erase failed or timed out\n");
                return;
            }

            tight_loop_contents();
        }
        time2 = time_us_64();
        printf("time1: %8lld\n", time1 - time0);
        printf("time2: %8lld\n", time2 - time0);

    } else {
        if (!qmi_flash_erase_4k(address)) {
            print_error("Flash erase failed");
            return;
        }
        time2 = time_us_64();
        printf("time1: %8lld\n", time1 - time0);
    }
    printf("Erase completed.\n");
}

// ------------------------------------------------------------
// Write
// ------------------------------------------------------------

static void command_write(const char *arg, int async_flag) {
    uint32_t address;
    uint32_t offset;
    uint64_t time0, time1, time2;

    if (!parse_hex(arg, &address)) {
        print_error("Invalid address");
        return;
    }

    address += base_offset;

    if ((address & (SECTOR_SIZE - 1)) != 0) {
        print_error("Address must be 4 KiB aligned");
        return;
    }
    if (address > FLASH_SIZE - SECTOR_SIZE) {
        print_error("Address out of Flash range");
        return;
    }
    printf("Writing buffer to Flash 0x%08lX ...\n", (unsigned long)address);
    // Page Program must not cross a 256-byte page boundary.
    time0 = time_us_64();
    for (offset = 0; offset < SECTOR_SIZE; offset += PAGE_SIZE) {
        if (async_flag) {
            if (!qmi_flash_program_start(address + offset, &flash_buffer[offset], PAGE_SIZE)) {
                printf("Write start failed\n");
                return;
            }
            while (true) {
                qmi_flash_poll_result_t result = qmi_flash_program_poll();

                if (result == QMI_FLASH_POLL_DONE)
                    break;

                if (result == QMI_FLASH_POLL_ERROR) {
                    printf("Write failed or timed out\n");
                    return;
                }
                time1 = time_us_64();
                // printf("time1: %8lld\n", time1 - time0);
                tight_loop_contents();
            }
            time2 = time_us_64();
            printf("time2: %8lld\n", time2 - time0);
        } else {
            if (!qmi_flash_program_page(address + offset, &flash_buffer[offset], PAGE_SIZE)) {
                printf("Write failed at 0x%08lX\n", (unsigned long)(address + offset));
                return;
            }
            time2 = time_us_64();
            printf("time2: %8lld\n", time2 - time0);
        }
    }
    printf("Write completed.\n");
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
        printf("%04lX: ", (unsigned long)(offset + i));
        for (j = 0; j < count; j++) {
            printf("%02X ", flash_buffer[offset + i + j]);
        }
        printf("\n");
    }
    next_address = offset + length;
}

// ------------------------------------------------------------
// Fill
// ------------------------------------------------------------

static void command_fill(const char *arg) {
    uint32_t value;

    if (!parse_hex(arg, &value) || value > 0xFF) {
        print_error("Value must be 00-FF");
        return;
    }
    memset(flash_buffer, (uint8_t)value, SECTOR_SIZE);
    printf("Buffer filled with %02lX.\n", (unsigned long)value);
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
    printf("Buffer[%04lX] = %02lX\n", (unsigned long)offset, (unsigned long)value);
}

// ------------------------------------------------------------
// Command interpreter
// ------------------------------------------------------------

static void execute_command(char *line) {
    char *argv[4];
    int argc = 0;
    int async_mode = 1;

    printf("start: async_mode: %d\n", async_mode);

    char *token = strtok(line, " \t");

    while (token != NULL && argc < 4) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }

    if (argc == 0)
        return;

    if (strcmp(argv[0], "r") == 0) {
        if (argc != 2) {
            print_error("Usage: r ADDRESS");
            return;
        }
        command_read(argv[1], async_mode);
    } else if (strcmp(argv[0], "e") == 0) {
        if (argc != 2) {
            print_error("Usage: e ADDRESS");
            return;
        }
        command_erase(argv[1], async_mode);
    } else if (strcmp(argv[0], "w") == 0) {
        if (argc != 2) {
            print_error("Usage: w ADDRESS");
            return;
        }
        command_write(argv[1], async_mode);
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
            print_error("clear buffer");
            memprt_clear();
        } else {
            print_error("flush_buffer:");
            memprt_flush();
        }
    } else if (strcmp(argv[0], "y") == 0) {
        printf("toggle debug_error: %s\n", toggle_debug_error() ? "true" : "false");
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

    printf("\nQMI Flash monitor\n");
    command_help();

    while (true) {

        printf("> ");
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
