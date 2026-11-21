//
// qmi_uart.c ... qmi async-poll and pt test program.
//

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"

#include "cmd.h"
#include "conf.h"
#include "memprt.h"
#include "qmi_flash.h"
#include "pt_qmi.h"

//
// core1 のメインループ
// この関数からリターンしない。
__attribute__((noinline)) void __time_critical_func(cdc_test_core1_entry)(void) {
    //
    // core1 (bus read/write loop)
    //
    int count = 0;
    command_loop();
}

__attribute__((noinline)) int __time_critical_func(main)(void) {
    stdio_init_all();

    // UART の初期化（元のプロジェクト設定を維持）。
    uart_init(UART_ID, BAUD_RATE);

    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    uart_puts(UART_ID, "qmi_uart: start!\n");
    memprt_init(UART_ID);
    // TinyUSB CDC が立ち上がるまでの時間を稼ぐ
    sleep_ms(1000);

    printf("\n");
    //
    // core1 start
    //
    multicore_launch_core1(cdc_test_core1_entry);

    // QMI / Flash の初期化など
    // ...
    qmi_init();
    while (1) {
        // tud_task();
        pt_qmi_flash_poll();
        //tight_loop_contents();
    }

    command_loop();
}
