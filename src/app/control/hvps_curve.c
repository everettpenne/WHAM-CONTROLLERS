/*
 * hvps_curve.c -- see hvps_curve.h.
 */
#include "hvps_curve.h"

void HvpsCurve_Clear(hvps_curve_t *c)
{
    c->n = 0U;
}

hvps_curve_result_t HvpsCurve_Append(hvps_curve_t *c, float hz, float volts)
{
    if (c->n >= HVPS_CURVE_MAX_POINTS)
    {
        return HVPS_CURVE_FULL;
    }
    if (volts < 0.0f)
    {
        return HVPS_CURVE_NEGATIVE;
    }
    if ((c->n != 0U) && (hz <= c->hz[c->n - 1U]))
    {
        return HVPS_CURVE_NOT_INCREASING;
    }
    c->hz[c->n] = hz;
    c->volts[c->n] = volts;
    c->n++;
    return HVPS_CURVE_OK;
}

static uint16_t PeakIndex(const hvps_curve_t *c)
{
    uint16_t best = 0U;

    for (uint16_t i = 1U; i < c->n; i++)
    {
        if (c->volts[i] > c->volts[best])
        {
            best = i;                       /* the first maximum on a flat top */
        }
    }
    return best;
}

uint8_t HvpsCurve_IsValid(const hvps_curve_t *c)
{
    uint8_t falling = 0U;

    if (c->n < 2U)
    {
        return 0U;
    }
    for (uint16_t i = 1U; i < c->n; i++)
    {
        if (c->volts[i] < c->volts[i - 1U])
        {
            falling = 1U;
        }
        else if ((c->volts[i] > c->volts[i - 1U]) && (falling != 0U))
        {
            return 0U;                      /* rose again after falling: a second peak */
        }
    }
    return 1U;
}

void HvpsCurve_SetLinear(hvps_curve_t *c, float fMinHz, float resHz, float fMaxHz, float vpeak)
{
    HvpsCurve_Clear(c);
    if (resHz > fMinHz)
    {
        (void)HvpsCurve_Append(c, fMinHz, 0.0f);
    }
    (void)HvpsCurve_Append(c, resHz, vpeak);
    if (fMaxHz > resHz)
    {
        (void)HvpsCurve_Append(c, fMaxHz, 0.0f);
    }
}

float HvpsCurve_Eval(const hvps_curve_t *c, float hz)
{
    if (c->n == 0U)
    {
        return 0.0f;
    }
    if (hz <= c->hz[0])
    {
        return c->volts[0];
    }
    for (uint16_t i = 1U; i < c->n; i++)
    {
        if (hz <= c->hz[i])
        {
            float x = (hz - c->hz[i - 1U]) / (c->hz[i] - c->hz[i - 1U]);

            return c->volts[i - 1U] + (c->volts[i] - c->volts[i - 1U]) * x;
        }
    }
    return c->volts[c->n - 1U];
}

float HvpsCurve_PeakHz(const hvps_curve_t *c)
{
    return (c->n != 0U) ? c->hz[PeakIndex(c)] : 0.0f;
}

float HvpsCurve_PeakVolts(const hvps_curve_t *c)
{
    return (c->n != 0U) ? c->volts[PeakIndex(c)] : 0.0f;
}

float HvpsCurve_FreqFor(const hvps_curve_t *c, float volts, uint8_t above)
{
    uint16_t p;

    if (c->n == 0U)
    {
        return 0.0f;
    }
    p = PeakIndex(c);
    if (volts >= c->volts[p])
    {
        return c->hz[p];
    }
    if (above != 0U)
    {
        for (uint16_t i = p; (i + 1U) < c->n; i++)         /* falling: v[i] >= volts > v[i + 1] */
        {
            if (volts >= c->volts[i + 1U])
            {
                float span = c->volts[i] - c->volts[i + 1U];
                float x = (span > 0.0f) ? (c->volts[i] - volts) / span : 1.0f;

                return c->hz[i] + (c->hz[i + 1U] - c->hz[i]) * x;
            }
        }
        return c->hz[c->n - 1U];
    }
    for (uint16_t i = p; i >= 1U; i--)                     /* rising toward the peak: v[i - 1] <= volts < v[i] */
    {
        if (volts >= c->volts[i - 1U])
        {
            float span = c->volts[i] - c->volts[i - 1U];
            float x = (span > 0.0f) ? (c->volts[i] - volts) / span : 1.0f;

            return c->hz[i] - (c->hz[i] - c->hz[i - 1U]) * x;
        }
    }
    return c->hz[0];
}
