/*
 * adc.h -- the four analog inputs, AnalogIn_01..04 (docs/pin_mapping_v4.csv):
 *   1  PA0  ADC1_IN1       2  PA1  ADC1_IN2
 *   3  PC0  ADC1_IN6       4  PB0  ADC1_IN15
 * plus the internal reference channel (VREFINT), which needs no pin and so
 * proves the converter works and measures the real analog supply.
 * Implemented in src/bsp/stm32g4/adc.c. Added 2026-10-02.
 *
 * ADC1, 12 bits, single-ended, software-triggered one conversion at a time, each
 * result the average of 16 (the converter's hardware oversampler), with a long
 * sampling time so a high-impedance source is read correctly. A read blocks for
 * ~100 us. Channels are numbered 0..3 here (1..4 on the wire).
 */
#ifndef ADC_H
#define ADC_H

#include <stdint.h>

#define ADC_NUM_CHANNELS  (4U)

/* Enables the pins and ADC1 (clock, regulator, calibration, enable). Call once at
   start-up, after the clocks are running. Check Adc_IsReady() afterwards. */
void Adc_Init(void);

/* 1 if the converter calibrated and enabled; 0 if start-up failed or timed out. */
uint8_t Adc_IsReady(void);

/* One averaged conversion of an input (0..ADC_NUM_CHANNELS-1): *code is 0..4095,
   0..VREF+ . Returns 0 (and leaves *code alone) for a bad channel, an ADC that is
   not ready, or a conversion that timed out. */
uint8_t Adc_Read(uint8_t channel, uint16_t *code);

/* The same for the internal reference VREFINT (about 1.21 V). With the factory
   calibration value it gives the real voltage on VREF+ (the ADC's reference, vref.h):
   *calCode is the code the chip's own calibration recorded with VREF+ = 3.0 V (0 if
   unavailable), so VREF+ = 3.0 V * calCode / code. */
uint8_t Adc_ReadVrefint(uint16_t *code, uint16_t *calCode);

#endif /* ADC_H */
