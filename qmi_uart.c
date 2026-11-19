#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/qmi.h"
#include "hardware/flash.h"
#include "pico/time.h"

#include "conf.h"
#include "cmd.h"
#include "qmi_flash.h"
#include "memprt.h"

int main(void)
{
    stdio_init_all();

    // UART の初期化（元のプロジェクト設定を維持）。
    uart_init(UART_ID, BAUD_RATE);

    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    uart_puts(UART_ID, "qmi_uart: start!\n");
    memprt_init(UART_ID);

    printf("\n");

    // QMI / Flash の初期化など
    // ...
    qmi_init();

    command_loop();
}
