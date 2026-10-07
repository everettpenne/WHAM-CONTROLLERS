/*
 * adc.h -- the analog inputs, AnalogIn_01..06 (docs/pin_mapping_v4.csv), as a continuously
 * running DMA sampler. Implemented in src/bsp/stm32g4/adc.c. Rewritten 2026-10-02.
 *
 *   wire  pin   board name    ADC1 channel
 *    1    PA0   AnalogIn_01   IN1
 *    2    PA1   AnalogIn_02   IN2
 *    3    PC0   AnalogIn_03   IN6
 *    4    PB0   AnalogIn_04   IN15
 *    5    PC1   AnalogIn_05   IN7
 *    6    PB1   AnalogIn_08   IN12
 *   and the internal reference VREFINT (IN18), scanned with them, which measures the real
 *   voltage on VREF+ (vref.h).
 *
 * ONE scan of all seven channels is triggered by a timer at a fixed rate and written by DMA
 * into a circular buffer; every `blockScans` scans the DMA's half/full-transfer interrupt hands
 * the application the AVERAGE of the block, per channel. That block interrupt is the closed-loop
 * controller's fixed-rate tick (app/control/control_task.c), so the sample timing is exact and
 * jitter-free and the CPU only wakes once per block. 12 bits, single-ended, the full scale is
 * ANALOG_VREF_MV.
 *
 * The sampling time (how long the converter looks at the input before converting) is a setting:
 * a high-impedance source needs a long one, a long one limits the scan rate.
 */
#ifndef ADC_H
#define ADC_H

#include <stdint.h>

#define ADC_NUM_CHANNELS    (6U)                 /* the input pins */
#define ADC_SCAN_LEN        (7U)                 /* ... plus VREFINT, scanned last */
#define ADC_VREFINT_INDEX   (6U)
#define ADC_MAX_BLOCK_SCANS (64U)

/* Called from the DMA interrupt (priority 2) with the per-channel mean ADC code of the block
   just completed: means[0..5] the inputs, means[6] VREFINT. */
typedef void (*adc_block_handler_t)(const float *means);

/* Pins, clock, regulator, calibration, enable, and the (stopped) sampler. Call once at
   start-up after Vref_Init(). */
void Adc_Init(void);

/* 1 if the converter calibrated and enabled. */
uint8_t Adc_IsReady(void);

/* Starts the sampler: one scan every 1/scanRateHz, blocks of `blockScans` scans, `handler`
   called per block. Restarts it if it is running. Returns 0 (nothing changed) if the converter
   is not ready, an argument is out of range, or the scan does not fit in the period at the
   present sampling time (Adc_ScanTimeUs()). */
uint8_t Adc_StartSampler(uint32_t scanRateHz, uint16_t blockScans, adc_block_handler_t handler);
void    Adc_StopSampler(void);
uint8_t Adc_SamplerRunning(void);

/* The sampling time in ADC clocks x10 (25 = 2.5, 65, 125, 245, 475, 925, 2475, 6405: the eight
   choices of the converter). Takes effect at the next Adc_StartSampler(). Returns 0 for a value
   that is not one of them. */
uint8_t  Adc_SetSampleTimeX10(uint16_t clocksX10);
uint16_t Adc_GetSampleTimeX10(void);

/* How long one full scan takes at the present sampling time, in microseconds (x1: the seven
   conversions, VREFINT with its fixed long sampling time). */
float Adc_ScanTimeUs(void);

/* The same for a candidate sampling time (x10 clocks), without changing the setting; 0 for a value
   that is not one of the eight. */
float Adc_ScanTimeUsFor(uint16_t clocksX10);

/* The latest block means. Valid once the sampler has delivered a block; Adc_Read returns the
   mean of an input rounded to a code, 0 (code untouched) if there is none yet or `channel` is
   out of range. */
uint8_t Adc_Read(uint8_t channel, uint16_t *code);

/* VREFINT's latest mean and the chip's factory calibration (the code recorded with VREF+ =
   3.0 V, 0 if unavailable): VREF+ = 3.0 V * calCode / code. */
uint8_t Adc_ReadVrefint(uint16_t *code, uint16_t *calCode);

/* Blocks delivered since the sampler started (for diagnostics and rate checks). */
uint32_t Adc_GetBlockCount(void);

#endif /* ADC_H */
