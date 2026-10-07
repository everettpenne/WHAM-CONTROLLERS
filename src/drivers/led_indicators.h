/*
 * led_indicators.h -- the six front-panel LEDs (added 2026-10-07): UART
 * transmit and receive activity, and the operating state. Purely
 * cosmetic: nothing in the controller's behavior depends on them, and
 * nothing here blocks, waits or raises an interrupt of its own.
 *
 *   UART_TX_LED  PD2    lit while the controller is sending
 *   UART_RX_LED  PD3    lit while the controller is receiving
 *   STATE_IDLE   PF14   lit if and only if the state machine is IDLE
 *   STATE_ARMED  PF2    ... ARMED
 *   STATE_FIRING PC3    ... FIRING
 *   STATE_FAULT  PC2    ... FAULT
 *
 * Active high (pin -> resistor -> LED -> ground), docs/pin_mapping_v4.csv.
 *
 * State LEDs follow the state machine: state_machine.c calls
 * LedIndicators_ShowState() from the one place that assigns the state, so
 * the LED can never disagree with STATE? (a single register write per GPIO
 * port, safe from any interrupt). Before SM_Init() all four are off.
 *
 * Activity LEDs: a pulse (LedIndicators_PulseTx/Rx(), called by the serial
 * driver) lights the LED and restarts a one-shot timer; when that timer
 * expires a DMA channel writes the LED's reset bit into the GPIO, so the LED
 * goes dark with no interrupt, no polling and no CPU time -- even while the
 * main loop sits inside a blocking transmit. Activity closer together than
 * LED_ACTIVITY_MS keeps the LED lit.
 *
 * Switch: LED_INDICATORS_ENABLED (ctrlr_config.h). 0 turns every call into
 * nothing and frees the pins, the timers and the DMA channels.
 */
#ifndef LED_INDICATORS_H
#define LED_INDICATORS_H

#include <stdint.h>
#include "ctrlr_config.h"

typedef enum
{
    LED_STATE_NONE = 0,    /* all four state LEDs off */
    LED_STATE_IDLE,
    LED_STATE_ARMED,
    LED_STATE_FIRING,
    LED_STATE_FAULT
} led_state_t;

/* Bits of LedIndicators_GetMask(): what the firmware is driving. */
#define LED_MASK_TX         (0x01U)
#define LED_MASK_RX         (0x02U)
#define LED_MASK_IDLE       (0x04U)
#define LED_MASK_ARMED      (0x08U)
#define LED_MASK_FIRING     (0x10U)
#define LED_MASK_FAULT      (0x20U)

#if (LED_INDICATORS_ENABLED != 0)

/* Pins as outputs, all LEDs off, activity timers and DMA armed. Call once
   at start-up, before the serial link and the state machine. */
void LedIndicators_Init(void);

/* Lights the LED for `state` and no other state LED. */
void LedIndicators_ShowState(led_state_t state);

/* Light the activity LED now and keep it lit for LED_ACTIVITY_MS after the
   LAST call. A few register writes; safe from any context. */
void LedIndicators_PulseTx(void);
void LedIndicators_PulseRx(void);

/* The LED_MASK_* bits of the LEDs the firmware currently drives lit
   (DIAGnostic:LEDs?). */
uint8_t LedIndicators_GetMask(void);

#else

static inline void    LedIndicators_Init(void)                  {}
static inline void    LedIndicators_ShowState(led_state_t s)    { (void)s; }
static inline void    LedIndicators_PulseTx(void)               {}
static inline void    LedIndicators_PulseRx(void)               {}
static inline uint8_t LedIndicators_GetMask(void)               { return 0U; }

#endif /* LED_INDICATORS_ENABLED */

#endif /* LED_INDICATORS_H */
