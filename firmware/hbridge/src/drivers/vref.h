/*
 * vref.h -- the analog reference, VREF+ (pin 43). Added 2026-10-02.
 * Implemented in src/bsp/stm32g4/vref.c.
 *
 * This board has NO external reference: pin 43 is "VREF+ Capacitor"
 * (docs/pin_mapping_v4.csv), so the chip's internal reference buffer (VREFBUF)
 * must drive it. The buffer is off at reset, which leaves VREF+ near 0 V --
 * every ADC conversion then saturates at 4095 and the DAC output is scaled to
 * nothing. Vref_Init() turns it on; the voltage it produces is
 * ANALOG_VREF_MV (ctrlr_config.h) and is the full scale of BOTH the ADC inputs
 * (adc.h) and the DAC outputs (dac.h). Call it before Dac_Init()/Adc_Init().
 */
#ifndef VREF_H
#define VREF_H

#include <stdint.h>

/* Enables VREFBUF at ANALOG_VREF_MV and waits for it to settle. Returns 1 if the
   buffer reports ready, 0 if it did not within the time allowed. */
uint8_t Vref_Init(void);

/* 1 if Vref_Init() found the buffer ready. */
uint8_t Vref_IsReady(void);

#endif /* VREF_H */
