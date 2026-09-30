/*
 * supply_mode.c -- see supply_mode.h.
 */
#include "supply_mode.h"
#include "hrtim.h"

static supply_mode_t g_mode = SUPPLY_MODE_NONE;

supply_mode_t SupplyMode_Get(void)
{
    return g_mode;
}

uint8_t SupplyMode_Set(supply_mode_t mode)
{
    if (mode == g_mode)
    {
        return 0U;
    }
    g_mode = mode;
    return 1U;
}

const char *SupplyMode_Name(supply_mode_t mode)
{
    switch (mode)
    {
        case SUPPLY_MODE_LIMITER: return "LIMITER";
        case SUPPLY_MODE_HVPS:    return "HVPS";
        default:                  return "NONE";
    }
}

uint8_t SupplyMode_GetLimits(supply_mode_t mode, supply_limits_t *out)
{
    switch (mode)
    {
        case SUPPLY_MODE_LIMITER:
            out->freqMinHz = LIMITER_FREQ_MIN_HZ;
            out->freqMaxHz = LIMITER_FREQ_MAX_HZ;
            out->dutyMinPct = LIMITER_DUTY_MIN_PCT;
            out->dutyMaxPct = LIMITER_DUTY_MAX_PCT;
            return 1U;
        case SUPPLY_MODE_HVPS:
            out->freqMinHz = HVPS_FREQ_MIN_HZ;
            out->freqMaxHz = HVPS_FREQ_MAX_HZ;
            out->dutyMinPct = HVPS_DUTY_MIN_PCT;
            out->dutyMaxPct = HVPS_DUTY_MAX_PCT;
            return 1U;
        default:
            return 0U;
    }
}

supply_step_result_t SupplyMode_CheckStep(uint16_t per, const uint16_t *cmp, uint8_t channels)
{
    supply_limits_t lim;
    uint64_t counts;   /* period in HRTIM clocks: per + 1 */

    if (SupplyMode_GetLimits(g_mode, &lim) == 0U)
    {
        return SUPPLY_STEP_NO_MODE;
    }

    /* Carrier f = CLK / (per + 1). f > fmax  <=>  CLK > fmax * (per + 1);
       f < fmin  <=>  CLK < fmin * (per + 1). Exact integer math, so a step
       exactly on a limit is accepted and one count either side is not. */
    counts = (uint64_t)per + 1U;
    if ((uint64_t)HRTIM_TIMER_CLK_HZ > (uint64_t)lim.freqMaxHz * counts)
    {
        return SUPPLY_STEP_FREQ_HIGH;
    }
    if ((uint64_t)HRTIM_TIMER_CLK_HZ < (uint64_t)lim.freqMinHz * counts)
    {
        return SUPPLY_STEP_FREQ_LOW;
    }

    /* Duty = cmp / per, per channel. */
    for (uint8_t ch = 0U; ch < channels; ch++)
    {
        uint32_t cmp100 = (uint32_t)cmp[ch] * 100U;

        if (cmp100 < (uint32_t)lim.dutyMinPct * per)
        {
            return SUPPLY_STEP_DUTY_LOW;
        }
        if (cmp100 > (uint32_t)lim.dutyMaxPct * per)
        {
            return SUPPLY_STEP_DUTY_HIGH;
        }
    }
    return SUPPLY_STEP_OK;
}
