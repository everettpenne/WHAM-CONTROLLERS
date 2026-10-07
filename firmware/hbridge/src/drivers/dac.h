/*
 * dac.h -- the two analog outputs, AnalogOut_01 (PA4, DAC1_OUT1) and
 * AnalogOut_02 (PA5, DAC1_OUT2) (docs/pin_mapping_v4.csv). Added 2026-10-02.
 * Implemented in src/bsp/stm32g4/dac.c.
 *
 * Channels are numbered 0 and 1 here (1 and 2 on the wire, DAC:OUTPut). Both
 * come up enabled, with the DAC's output buffer on, at 0 V. Full scale is
 * ANALOG_VREF_MV (ctrlr_config.h, produced by the internal reference, vref.h),
 * 12 bits.
 */
#ifndef DAC_H
#define DAC_H

#include <stdint.h>

#define DAC_NUM_CHANNELS  (2U)

/* Configures PA4/PA5 as analog and enables DAC1 channels 1 and 2 at 0 V. Call
   once at start-up, after the clocks are running. */
void Dac_Init(void);

/* Sets a channel's output voltage in millivolts, rounded to the nearest of the
   4096 steps. Returns 0 (and changes nothing) for a bad channel or a value above
   ANALOG_VREF_MV, else 1. */
uint8_t Dac_SetMilliVolts(uint8_t channel, uint16_t milliVolts);

/* The 12-bit code last commanded for a channel (0 for a bad channel). */
uint16_t Dac_GetCode(uint8_t channel);

/* The code the DAC's output register actually holds, read back from the
   hardware: equals Dac_GetCode() once the DAC has taken the value (0 for a bad
   channel). */
uint16_t Dac_GetOutputCode(uint8_t channel);

#endif /* DAC_H */
