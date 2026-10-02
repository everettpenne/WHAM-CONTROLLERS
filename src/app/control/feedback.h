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
 *
 * The BOTTOM of the range is watched too, but only when the offset is non-zero, i.e. when
 * the calibration says 0 process volts is somewhere in the middle of the ADC range (a
 * bipolar output on a divider with its zero at mid-scale, e.g. the Limiter: -Vmax -> 0 V,
 * 0 -> vref/2, +Vmax -> vref). There a reading at code 0 is either the output beyond -Vmax
 * or, much more likely, a broken feedback line (open, shorted to ground, wrong channel),
 * and the loop would otherwise read it as a huge negative voltage and drive the output
 * up with nothing real behind the reading. With offset 0 (the HVPS, whose feedback is 0 V
 * at the start of every shot) a low reading is normal and is not counted.
 */
#ifndef FEEDBACK_H
#define FEEDBACK_H

#include <stdint.h>

#define FEEDBACK_CLIP_CODE   (4090U)   /* mean codes at or above this count as full scale */
#define FEEDBACK_CLIP_LOW_CODE (5U)    /* mean codes at or below this count as the bottom of the range -- see below */

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
    uint32_t clipLowTicks;    /* consecutive ticks at the bottom of the range (counted only when offsetV != 0) */
} feedback_t;

/* Defaults: scale 1, offset 0, no filter. */
void Feedback_Init(feedback_t *fb, float vrefV);

void Feedback_SetCalibration(feedback_t *fb, float scale, float offsetV);

/* The calibration for a bipolar output +-vmax whose feedback divider maps -vmax to 0 V and
   +vmax to the ADC full scale (zero at mid-scale): scale = 2 vmax / vref, offset = vref / 2. */
void Feedback_SetBipolarRange(feedback_t *fb, float vmax);

/* Low-pass cutoff in Hz for a loop ticking every `dt` seconds; 0 turns it off. */
void Feedback_SetFilter(feedback_t *fb, float cutoffHz, float dt);

/* Forgets the filter state and the clip count (call at the start of a run). */
void Feedback_Reset(feedback_t *fb);

/* Converts one tick's mean ADC code (0..4095, fractional because it is an average) to
   process volts, filters, updates the clip count, and returns the filtered value. */
float Feedback_Process(feedback_t *fb, float meanCode);

#endif /* FEEDBACK_H */
