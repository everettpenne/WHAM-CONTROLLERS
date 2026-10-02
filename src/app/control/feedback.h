/*
 * feedback.h / feedback.c -- turns the raw ADC reading of the feedback input into the
 * process voltage the controller works in. Added 2026-10-02. Pure logic: host-tested
 * (tests/test_feedback.c).
 *
 * The seam for the FEEDBACK SOURCE of the closed loop: today one ADC input through a
 * calibration (a divider/level-shifter on the board) and an optional low-pass; a
 * different measurement (a current, a sum of channels, a model) would be another module
 * with the same shape -- closed_loop.c only asks for process volts per tick.
 *
 *   adc volts    = code * vref / 4095
 *   process volts = (adc volts - offset) * scale        (calibration of the board's divider;
 *                                                          offset lets a bipolar output ride on
 *                                                          the ADC's unipolar range)
 *   filtered     = a first-order low-pass of that, cutoff in Hz (off by default)
 *
 * Also watches the ADC for CLIPPING: a reading at full scale means the signal is at or
 * beyond what the ADC can see (the loop is blind above it), so the number of consecutive
 * ticks at the top is counted for the controller's overrange fault.
 */
#ifndef FEEDBACK_H
#define FEEDBACK_H

#include <stdint.h>

#define FEEDBACK_CLIP_CODE   (4090U)   /* mean codes at or above this count as full scale */

typedef struct
{
    float vrefV;        /* the ADC's full scale in volts (ANALOG_VREF_MV / 1000) */
    float scale;        /* process volts per ADC volt */
    float offsetV;      /* ADC volts that read as 0 process volts */
    float alpha;        /* low-pass coefficient 0..1 (1 = off) */

    float rawVolts;     /* the last UNFILTERED process voltage (the overvoltage check uses this) */
    float volts;        /* the last filtered process voltage */
    uint8_t primed;     /* the filter has seen a sample */
    uint32_t clipHighTicks;   /* consecutive ticks with the reading at full scale */
} feedback_t;

/* Defaults: scale 1, offset 0, no filter. */
void Feedback_Init(feedback_t *fb, float vrefV);

void Feedback_SetCalibration(feedback_t *fb, float scale, float offsetV);

/* Low-pass cutoff in Hz for a loop ticking every `dt` seconds; 0 turns it off. */
void Feedback_SetFilter(feedback_t *fb, float cutoffHz, float dt);

/* Forgets the filter state and the clip count (call at the start of a run). */
void Feedback_Reset(feedback_t *fb);

/* Converts one tick's mean ADC code (0..4095, fractional because it is an average) to
   process volts, filters, updates the clip count, and returns the filtered value. */
float Feedback_Process(feedback_t *fb, float meanCode);

#endif /* FEEDBACK_H */
