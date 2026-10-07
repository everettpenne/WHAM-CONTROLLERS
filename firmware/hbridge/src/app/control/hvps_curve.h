/*
 * hvps_curve.h / hvps_curve.c -- the HVPS's frequency -> output relationship as a lookup table.
 * Added 2026-10-05. Pure logic: host-tested (tests/test_hvps_curve.c).
 *
 * The HVPS's feedback voltage stands in for POWER, which is never negative: power peaks at the
 * resonant frequency (about 19 kHz) and falls away on both sides. To raise the output the drive
 * frequency moves toward resonance (typically from about 20 kHz). The true curve is nonlinear and
 * is not measurable yet, so the controller works from a table of (frequency Hz, process volts)
 * points with linear interpolation between them. Until a measured table is loaded the default is
 * linear: zero at the edge of the mode's frequency band, the peak voltage at resonance, a straight
 * line between (HvpsCurve_SetLinear).
 *
 * A table is UNIMODAL: frequencies strictly increase, voltages are >= 0, and the voltage rises
 * (not necessarily strictly) to a single peak and then falls. Each side of the peak is one branch
 * the loop can run on, and HvpsCurve_FreqFor() inverts that branch: the frequency that gives a
 * wanted voltage.
 */
#ifndef HVPS_CURVE_H
#define HVPS_CURVE_H

#include <stdint.h>

#define HVPS_CURVE_MAX_POINTS  (32U)

typedef struct
{
    uint16_t n;
    float    hz[HVPS_CURVE_MAX_POINTS];
    float    volts[HVPS_CURVE_MAX_POINTS];
} hvps_curve_t;

typedef enum
{
    HVPS_CURVE_OK = 0,
    HVPS_CURVE_FULL,            /* HVPS_CURVE_MAX_POINTS points already */
    HVPS_CURVE_NOT_INCREASING,  /* frequency not above the previous point's */
    HVPS_CURVE_NEGATIVE         /* voltage below 0 (power is never negative) */
} hvps_curve_result_t;

void HvpsCurve_Clear(hvps_curve_t *c);

/* Appends one point. Nothing is stored unless HVPS_CURVE_OK. */
hvps_curve_result_t HvpsCurve_Append(hvps_curve_t *c, float hz, float volts);

/* 1 if the table has at least two points and is unimodal (see above). */
uint8_t HvpsCurve_IsValid(const hvps_curve_t *c);

/* The default: (fMin, 0), (resHz, vpeak), (fMax, 0); a side with no room is left out. */
void HvpsCurve_SetLinear(hvps_curve_t *c, float fMinHz, float resHz, float fMaxHz, float vpeak);

/* Volts at a frequency, linear between points, the end values beyond them. 0 for an empty table. */
float HvpsCurve_Eval(const hvps_curve_t *c, float hz);

float HvpsCurve_PeakHz(const hvps_curve_t *c);
float HvpsCurve_PeakVolts(const hvps_curve_t *c);

/* The frequency on the branch ABOVE (1) or BELOW (0) the peak at which the curve gives `volts`:
   the peak frequency for a voltage at or above the peak, the branch's end frequency for a voltage
   at or below the end value. */
float HvpsCurve_FreqFor(const hvps_curve_t *c, float volts, uint8_t above);

#endif /* HVPS_CURVE_H */
