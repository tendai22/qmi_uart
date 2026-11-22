// qmi_uart.c - Core0: poll, Core1: monitor
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "cmd.h"
#include "conf.h"
#include "memprt.h"
#include "qmi_flash.h"
#include "pt_qmi.h"
#include "pt_qmi_floppy.h"

__attribute__((noinline)) void __time_critical_func(cdc_test_core1_entry)(void) {
    command_loop();
}
__attribute__((noinline)) int __time_critical_func(main)(void) {
    stdio_init_all();
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_puts(UART_ID, "qmi_uart: start!\n");
    memprt_init(UART_ID);
    sleep_ms(1000);
    printf("\n");
    qmi_init();  // initialize before Core1 can issue requests
    multicore_launch_core1(cdc_test_core1_entry);
    while (true) {
        pt_qmi_flash_poll();
        pt_qmi_floppy_poll();
        tight_loop_contents();
    }
}
