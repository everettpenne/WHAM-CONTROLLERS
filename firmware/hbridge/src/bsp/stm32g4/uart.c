/*
 * uart.c
 *
 * Ported verbatim from the sibling PFM-STM32G474 project's serial
 * command architecture -- pin/board-agnostic (buffer/line-accumulation
 * logic only), so it needed no adaptation for this project's pinout.
 *
 * Single-byte interrupt-driven receive with line-buffer accumulation.
 * Each call to HAL_UART_Receive_IT() arms for exactly one byte; the ISR
 * fires uart_rx_callback(), which accumulates into rx_line until a line
 * terminator is seen, then copies to line_buf and sets line_ready.
 *
 * uart_take_line() is polled from the main loop (app's SCPI task), which
 * hands the line to the parser. No RTOS or DMA required.
 */

#include "uart_hw.h"
#include "led_indicators.h"
#include <string.h>

struct uart_instance
{
    UART_HandleTypeDef *huart;
    char                rx_line[UART_RX_BUF_SIZE];   /* accumulation buffer   */
    char                line_buf[UART_RX_BUF_SIZE];  /* stable copy for parse */
    volatile uint8_t    rx_byte;                      /* single-byte IT target */
    uint8_t             rx_index;
    volatile uint8_t    line_ready;
    /* Receive-error counters (DIAGnostic:UART?). Written from the USART2 ISR and the main loop. */
    volatile uint32_t   err_overrun, err_framing, err_noise, err_parity, rearmed;
};

/* Global UART instance for USART2 */
uart_instance_t uart2;

void uart_bind(uart_instance_t *inst, UART_HandleTypeDef *huart)
{
    inst->huart      = huart;
    inst->rx_index   = 0;
    inst->line_ready = 0;
    inst->err_overrun = inst->err_framing = inst->err_noise = inst->err_parity = inst->rearmed = 0U;
}

void uart_start(uart_instance_t *inst)
{
    /* Arm the first receive interrupt */
    if (HAL_UART_Receive_IT(inst->huart, (uint8_t *)&inst->rx_byte, 1) != HAL_OK) {
        /* Handle error - maybe add debug output */
        char err[] = "UART RX IT Failed!\r\n";
        HAL_UART_Transmit(inst->huart, (uint8_t*)err, strlen(err), 100);
    }
}

void uart_send(uart_instance_t *inst, const char *str)
{
    /* Activity LED (led_indicators.h): lit at the start and again when the
       last byte has gone, so a long send never lets it expire mid-way. The
       LED turns itself off; nothing here waits for it. */
    LedIndicators_PulseTx();
    HAL_UART_Transmit(inst->huart, (uint8_t *)str, strlen(str), HAL_MAX_DELAY);
    LedIndicators_PulseTx();
}

/*
 * uart_rx_callback()
 *
 * Called from HAL_UART_RxCpltCallback (stm32g4xx_it.c) after each received
 * byte. Must re-arm the interrupt before returning.
 */
void uart_rx_callback(uart_instance_t *inst)
{
    char c = (char)inst->rx_byte;

    LedIndicators_PulseRx();   /* activity LED, any instance (led_indicators.h) */

    /* Simple accumulation */
    if (c == '\n' || c == '\r') {
        /* End of line */
        if (inst->rx_index > 0) {
            inst->rx_line[inst->rx_index] = '\0';
            strcpy(inst->line_buf, inst->rx_line);
            inst->rx_index = 0;
            inst->line_ready = 1;
        }
    } else if (inst->rx_index < UART_RX_BUF_SIZE - 1) {
        inst->rx_line[inst->rx_index++] = c;
    }

    HAL_UART_Receive_IT(inst->huart, (uint8_t *)&inst->rx_byte, 1);
}


/*
 * uart_error_callback()
 *
 * Called from HAL_UART_ErrorCallback (stm32g4xx_it.c). *** REAL BUG, FOUND 2026-11-09 from the hub's logs of a
 * LabVIEW shot: during FIRING, valid polls started coming back "ERR 1 Unknown command" (bytes lost), then the
 * controller stopped answering altogether while still sending its own !EVT lines -- powered, transmitting,
 * deaf. *** The HAL treats an overrun (ORE: a byte arrived before the last one was read) as a blocking error:
 * it ENDS the interrupt receive (RxState back to READY) and calls this hook. Nothing here ever armed the next
 * receive, so USART2 RX stayed off until a power cycle. The overrun itself comes from the USART2 interrupt
 * waiting more than one byte time (87 us at 115200) behind other interrupts: it ran at priority 3, the same
 * level as the capture-DMA batch ISRs that run every 16 edges on each channel and get busier as a shot's
 * frequency rises (see main.c for the priority fix). Both are fixed: the interrupt is faster to be served, and
 * a receive error can no longer switch reception off.
 *
 * Counts the error, drops the partial line (a byte is missing from it; running a damaged command is worse than
 * ignoring it), and re-arms.
 */
void uart_error_callback(uart_instance_t *inst)
{
    const uint32_t e = inst->huart->ErrorCode;

    if (e & HAL_UART_ERROR_ORE) { inst->err_overrun++; }
    if (e & HAL_UART_ERROR_FE)  { inst->err_framing++; }
    if (e & HAL_UART_ERROR_NE)  { inst->err_noise++; }
    if (e & HAL_UART_ERROR_PE)  { inst->err_parity++; }
    inst->rx_index = 0;
    uart_rx_ensure_armed(inst);
}

/* Re-arms reception if the HAL is not receiving. Called from the error hook and from the main loop
   (uart_take_line), so any path that ends a receive -- not only the overrun above -- cannot leave the port deaf. */
void uart_rx_ensure_armed(uart_instance_t *inst)
{
    if (inst->huart->RxState == HAL_UART_STATE_READY)
    {
        if (HAL_UART_Receive_IT(inst->huart, (uint8_t *)&inst->rx_byte, 1) == HAL_OK)
        {
            inst->rearmed++;
        }
    }
}

void uart_get_rx_errors(const uart_instance_t *inst, uart_rx_errors_t *out)
{
    out->overrun = inst->err_overrun;
    out->framing = inst->err_framing;
    out->noise   = inst->err_noise;
    out->parity  = inst->err_parity;
    out->rearmed = inst->rearmed;
}

char *uart_take_line(uart_instance_t *inst)
{
    uart_rx_ensure_armed(inst);   /* polled every main-loop pass: a receive that was ended is restarted within one */
    if (inst->line_ready) {
        inst->line_ready = 0;
        return inst->line_buf;
    }
    return NULL;
}
