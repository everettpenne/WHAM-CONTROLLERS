/*
 * uart.h -- line-oriented serial link (the SCPI command port).
 *
 * Ported from the sibling PFM-STM32G474 project's serial command
 * architecture. Receive is interrupt-driven, one byte at a time, with
 * line accumulation; transmit is blocking. The instance type is opaque:
 * its HAL handle and buffers live in the BSP (src/bsp/<chip>/uart.c,
 * binding in uart_hw.h).
 */
#ifndef INC_UART_H_
#define INC_UART_H_

#include <stddef.h>
#include <stdint.h>

#define UART_RX_BUF_SIZE    128   /* longest line, including the terminator */

typedef struct uart_instance uart_instance_t;

/* The command link (USART2 on this board). */
extern uart_instance_t uart2;

/* Arm reception. The instance must already be bound to its peripheral
   (uart_bind(), BSP, called from main.c). */
void uart_start(uart_instance_t *inst);

/* Receive errors seen since boot, and how many times reception had to be restarted. An overrun means the
   serial interrupt was served too late (a byte was lost); the partial line is dropped and reception restarts. */
typedef struct { uint32_t overrun, framing, noise, parity, rearmed, lines, clobbered; } uart_rx_errors_t;
void uart_get_rx_errors(const uart_instance_t *inst, uart_rx_errors_t *out);

/* True byte count of the line uart_take_line() last returned (the line is NUL-terminated, but a NUL byte received
   inside it would end the string early). Valid until the next line arrives. */
size_t uart_line_length(const uart_instance_t *inst);

/* Send a NUL-terminated string; returns once it is all on the wire. */
void uart_send(uart_instance_t *inst, const char *str);

/* If a complete line has arrived, return it (NUL-terminated, terminator
   stripped) and mark it consumed; otherwise NULL. The buffer stays valid,
   and may be modified in place, until the next complete line arrives. */
char *uart_take_line(uart_instance_t *inst);

#endif /* INC_UART_H_ */
