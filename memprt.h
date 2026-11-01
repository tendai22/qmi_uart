#pragma once

#include <stdint.h>
#include "hardware/uart.h"

extern void memprt_init(struct uart_inst *uart_id);
extern void debug(const char *format, ...);
extern void memprt_flush(void);
extern void memprt_clear(void);
