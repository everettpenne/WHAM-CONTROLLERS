/*
 * duty_meter.h -- measures the duty and frequency of the signal on PFM_Input_01 (PA15), continuously,
 * for the closed-loop loopback test (Phase U wired to PFM_Input_01). Added 2026-10-02.
 * Implemented in src/bsp/stm32g4/duty_meter.c.
 *
 * TIM2 in PWM-input mode: PA15 (TI1) resets the counter on each rising edge, CCR1 captures the period
 * and CCR2 (the same pin, falling edge) the high time, all in hardware -- no interrupt, no DMA, no CPU
 * work per edge; the application just reads the latest pair when it wants it. The meter borrows TIM2
 * from the PFM_Input capture (pfm_input.h): DutyMeter_Start() saves TIM2's registers and
 * DutyMeter_Stop() puts them back, so a PFMIN capture works as before afterwards. They cannot run
 * at the same time (Start refuses while a capture has TIM2's interrupts/DMA on).
 */
#ifndef DUTY_METER_H
#define DUTY_METER_H

#include <stdint.h>

/* Configures TIM2 as above and starts it. Returns 1 when running (also if it already was), 0 when TIM2
   is busy with a PFM_Input capture. */
uint8_t DutyMeter_Start(void);

/* Stops the meter and restores TIM2 to the state DutyMeter_Start() found. Safe to call when not running. */
void DutyMeter_Stop(void);

/* The latest completed period: duty 0..1 (high time / period) and frequency in Hz. Returns 1 if at least
   one new period was captured since the previous call, 0 if not (then the outputs are the last good values,
   or 0 if there never was one). */
uint8_t DutyMeter_Read(float *duty, float *freqHz);

#endif /* DUTY_METER_H */
