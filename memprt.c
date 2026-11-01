//
// memory printf
//
// 組み込み系でシリアルポート出力による時間消費を避けるために、
// 専用の printf関数(void debug(uint8_t *format, ...))を定義し、
// debugの呼び出しごとに書式出力された文字列を、バッファbufferの末尾に
// 追加してゆくようにする。
// 測定期間が終わり、出力時間に余裕ができると、flush_buffer(void)を呼び出し、
// これまでbufferにためた文字列をまとめてシリアルポートUART_ID に出力する。
// 以下にコード断片があるので、これを完成させてください。

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "hardware/uart.h"

#include "conf.h"   // UART_ID definition here

#define BUFSIZE 0x8000u // 32kB

static uint8_t buffer[BUFSIZE];
static uint8_t *current;

#define RESTSIZE ((size_t)(&buffer[BUFSIZE] - current))

//
// UART output
//
//#define UART_ID 0

//extern void uart_write_blocking(uint32_t uart_id, const uint8_t *src, size_t len);

struct uart_inst *uart_id = 0;
//
// initialize
//
void memprt_init(struct uart_inst *uartid)
{
    uart_id = uartid;
    current = &buffer[0];
    buffer[0] = '\0';
}

void memprt_clear(void)
{
    current = &buffer[0];
    buffer[0] = '\0';
}

//
// memory printf
//
void debug(const char *format, ...)
{
    if (current == NULL)
        return;

    size_t rest = RESTSIZE;
    // 終端文字 '\0' のため、空き容量が 1 byte 以下なら書き込まない
    if (rest <= 1)
        return;

    va_list args;
    va_start(args, format);
    int len = vsnprintf((char *)current, rest,
                        (const char *)format, args);

    va_end(args);
    if (len < 0)
        return;

    // vsnprintf() が必要とした容量が残容量以上なら、
    // バッファ末尾まで書き込まれたため、それ以上追記しない
    if ((size_t)len >= rest) {
        current = &buffer[BUFSIZE - 1];
    } else {
        current += len;
    }
}

//
// flush buffer
//
void memprt_flush(void)
{
    size_t len = (size_t)(current - buffer);

    if (len == 0) {
        return;
    }

    uint8_t *p = buffer;
    uint8_t *end = current;

    while (p < end) {
        // 次の '\n' を検索
        uint8_t *lf = p;
        while (lf < end && *lf != '\n')
            lf++;
        if (lf == end) {
            // 残りのデータをまとめて送信
            uart_write_blocking(UART_ID, p, (size_t)(end - p));
            break;
        }

        // '\n' の直前まで送信
        if (lf > p) {
            uart_write_blocking(UART_ID, p, (size_t)(lf - p));
        }

        // 直前が '\r' でなければ '\r' を送信
        if (lf == p || *(lf - 1) != '\r') {
            const uint8_t cr = '\r';
            uart_write_blocking(UART_ID, &cr, 1);
        }
        // '\n' を送信
        uart_write_blocking(UART_ID, lf, 1);
        // 次の行へ
        p = lf + 1;
    }

    // バッファをリセット
    current = &buffer[0];
    buffer[0] = '\0';
}
