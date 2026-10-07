/*
 * board_io.h -- the board's own pins that are not a peripheral's, behind a
 * HAL-free interface. Implemented in src/bsp/<chip>/board_io.c.
 */
#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdint.h>

/* Configures the GateDriverStatus_01..12 fault inputs (PE0..PE11): digital
   inputs with an interrupt on both edges, no pull, and the seven EXTI
   vectors that serve them. Called from main.c's MX_GPIO_Init() (USER CODE
   block). */
void BoardIo_Init(void);

/* Raw bitwise read of GateDriverStatus_01..12 (bit 0 = _01 = PE0). No
   debounce and no polarity interpretation. */
uint16_t BoardIo_ReadGateDriverStatus(void);

#endif /* BOARD_IO_H */
