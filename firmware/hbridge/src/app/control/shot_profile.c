/*
 * shot_profile.c -- see shot_profile.h.
 */
#include "shot_profile.h"
#include "supply_mode.h"
#include "hrtim.h"
#include <stddef.h>

/* This firmware is built at -O0 (CubeIDE's Debug configuration). The generator
   runs in the refill interrupt, which only gets the CPU the per-period interrupt
   leaves; at -O0 it cost 1,600-2,100 cycles per entry and the refill fell behind
   the playback above ~75 kHz (measured on the simulator, docs/changelog.txt
   2026-10-01). This file is pure integer logic, host-tested identically, so it is
   compiled at -O2 whatever the project's level. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif

#define TICKS_PER_US   ((uint64_t)(HRTIM_TIMER_CLK_HZ / 1000000UL))

static shot_setpoint_t g_sp[SHOT_SETPOINT_MAX];
static uint16_t        g_count = 0U;

void ShotProfile_Clear(void)
{
    g_count = 0U;
}

shot_append_result_t ShotProfile_Append(uint32_t tUs, uint32_t freqMilliHz, uint16_t dutyHundredths)
{
    supply_limits_t lim;

    if (SupplyMode_GetLimits(SupplyMode_Get(), &lim) == 0U)
    {
        return SHOT_APPEND_NO_MODE;
    }
    if (g_count >= SHOT_SETPOINT_MAX)
    {
        return SHOT_APPEND_FULL;
    }
    if ((g_count == 0U) && (tUs != 0U))
    {
        return SHOT_APPEND_FIRST_NOT_ZERO;
    }
    if ((g_count > 0U) && (tUs <= g_sp[g_count - 1U].tUs))
    {
        return SHOT_APPEND_NOT_INCREASING;
    }
    if (tUs > SHOT_MAX_DURATION_US)
    {
        return SHOT_APPEND_TOO_LONG;
    }
    if (freqMilliHz < (uint32_t)lim.freqMinHz * 1000UL)
    {
        return SHOT_APPEND_FREQ_LOW;
    }
    if ((uint64_t)freqMilliHz > (uint64_t)lim.freqMaxHz * 1000U)
    {
        return SHOT_APPEND_FREQ_HIGH;
    }
    if (dutyHundredths < (uint16_t)(lim.dutyMinPct * 100U))
    {
        return SHOT_APPEND_DUTY_LOW;
    }
    if (dutyHundredths > (uint16_t)(lim.dutyMaxPct * 100U))
    {
        return SHOT_APPEND_DUTY_HIGH;
    }

    g_sp[g_count].tUs            = tUs;
    g_sp[g_count].freqMilliHz    = freqMilliHz;
    g_sp[g_count].dutyHundredths = dutyHundredths;
    g_count++;
    return SHOT_APPEND_OK;
}

uint16_t ShotProfile_Count(void)
{
    return g_count;
}

uint32_t ShotProfile_DurationUs(void)
{
    return (g_count == 0U) ? 0U : g_sp[g_count - 1U].tUs;
}

uint8_t ShotProfile_Get(uint16_t index, shot_setpoint_t *out)
{
    if (index >= g_count)
    {
        return 0U;
    }
    *out = g_sp[index];
    return 1U;
}

uint8_t ShotProfile_IsComplete(void)
{
    return (g_count >= 2U) ? 1U : 0U;
}

/* ---- generator ---- */

/* The period in HRTIM clocks for a frequency in millihertz: round(CLK * 1000 / f),
   exactly -- the same value as (CLK * 1000 + f / 2) / f in 64-bit arithmetic, which
   the host port uses. Done with the core's 32-bit hardware divide only: the refill
   (a low-priority interrupt that gets only what the per-period interrupt leaves)
   runs this once per entry and fell behind the playback above ~75 kHz when it was
   a 64-bit division. Needs f < 2^27 (134 kHz; checked below against the highest
   limit). */
#if (LIMITER_FREQ_MAX_HZ * 1000UL) >= (1UL << 27) || (HVPS_FREQ_MAX_HZ * 1000UL) >= (1UL << 27)
#error "PeriodClocks() needs every mode's maximum frequency below 134 kHz (see shot_profile.c)"
#endif
static uint32_t PeriodClocks(uint32_t fMilliHz)
{
    /* N = CLK * 1000 = top * 2^10 + low, with `top` held in 32 bits (N < 2^42): one
       divide for the top, then the remaining ten bits two at a time in 5-bit chunks
       (the remainder is below f < 2^27, so remainder * 32 + chunk fits 32 bits).
       Three hardware divides in all, the same exact quotient as a long division. */
    const uint64_t n = (uint64_t)HRTIM_TIMER_CLK_HZ * 1000U;
    const uint32_t top = (uint32_t)(n >> 10);
    const uint32_t c1 = (uint32_t)((n >> 5) & 31U);
    const uint32_t c0 = (uint32_t)(n & 31U);
    uint32_t q = top / fMilliHz;
    uint32_t r = top % fMilliHz;

    r = (r << 5) | c1;
    q = (q << 5) + r / fMilliHz;
    r = r % fMilliHz;
    r = (r << 5) | c0;
    q = (q << 5) + r / fMilliHz;
    r = r % fMilliHz;
    if ((2U * (uint64_t)r) >= fMilliHz)
    {
        q++;
    }
    return q;
}

typedef struct
{
    uint8_t  active;
    uint8_t  periodsPerEntry;
    uint16_t seg;            /* current segment = setpoints seg .. seg+1 */
    uint64_t tick;           /* start of the next entry, in HRTIM clocks since t = 0 */
    uint64_t segStart;       /* the segment's start and end, in clocks */
    uint64_t segEnd;
    uint32_t f0;             /* setpoint seg: millihertz and hundredths of a percent */
    uint32_t d0;
    int64_t  slopeF;         /* per clock, Q32 */
    int64_t  slopeD;
    /* limits of the selected mode, as period / compare bounds */
    uint32_t per1Min;        /* shortest period, in clocks (highest allowed frequency) */
    uint32_t per1Max;
    uint8_t  dutyMinPct;
    uint8_t  dutyMaxPct;
} gen_t;

static gen_t g;

static void LoadSegment(uint16_t seg)
{
    const shot_setpoint_t *a = &g_sp[seg];
    const shot_setpoint_t *b = &g_sp[seg + 1U];
    int64_t dt;

    g.seg      = seg;
    g.segStart = (uint64_t)a->tUs * TICKS_PER_US;
    g.segEnd   = (uint64_t)b->tUs * TICKS_PER_US;
    g.f0       = a->freqMilliHz;
    g.d0       = a->dutyHundredths;
    dt         = (int64_t)(g.segEnd - g.segStart);
    /* slope = delta * 2^32 / dt, truncated toward zero (the host port does the same) */
    g.slopeF   = ((int64_t)b->freqMilliHz - (int64_t)a->freqMilliHz) * 4294967296LL / dt;
    g.slopeD   = ((int64_t)b->dutyHundredths - (int64_t)a->dutyHundredths) * 4294967296LL / dt;
}

/* {per, cmp} for a period in clocks and a duty in hundredths of a percent, clamped onto the
   mode's limits: the period into [per1Min, per1Max], then the compare onto the mode's exact
   integer duty limits (the relation SupplyMode_CheckStep() uses: cmp*100 against pct*per). */
static void StepFromPeriod(uint32_t per1, uint32_t dutyHund, uint32_t per1Min, uint32_t per1Max,
                           uint8_t dutyMinPct, uint8_t dutyMaxPct, uint32_t *perOut, uint32_t *cmpOut)
{
    uint32_t perV;
    uint32_t cmpV;
    uint32_t cmpLo;
    uint32_t cmpHi;

    if (per1 < per1Min)
    {
        per1 = per1Min;
    }
    if (per1 > per1Max)
    {
        per1 = per1Max;
    }
    perV  = per1 - 1U;
    cmpV  = (perV * dutyHund + 5000U) / 10000U;
    cmpLo = ((uint32_t)dutyMinPct * perV + 99U) / 100U;
    cmpHi = ((uint32_t)dutyMaxPct * perV) / 100U;
    if (cmpV < cmpLo)
    {
        cmpV = cmpLo;
    }
    if (cmpV > cmpHi)
    {
        cmpV = cmpHi;
    }
    *perOut = perV;
    *cmpOut = cmpV;
}

uint8_t ShotGen_Start(uint8_t periodsPerEntry)
{
    supply_limits_t lim;
    uint64_t clk = (uint64_t)HRTIM_TIMER_CLK_HZ;

    g.active = 0U;
    if ((ShotProfile_IsComplete() == 0U) || (periodsPerEntry == 0U) ||
        (SupplyMode_GetLimits(SupplyMode_Get(), &lim) == 0U))
    {
        return 0U;
    }
    /* f <= fmax  <=>  period >= CLK / fmax (round up);
       f >= fmin  <=>  period <= CLK / fmin (round down). */
    g.per1Min    = (uint32_t)((clk + lim.freqMaxHz - 1U) / lim.freqMaxHz);
    g.per1Max    = (uint32_t)(clk / lim.freqMinHz);
    g.dutyMinPct = lim.dutyMinPct;
    g.dutyMaxPct = lim.dutyMaxPct;
    g.periodsPerEntry = periodsPerEntry;
    g.tick       = 0U;
    LoadSegment(0U);
    g.active     = 1U;
    return 1U;
}

uint8_t ShotGen_Next(uint16_t *per, uint16_t *cmp, uint8_t *last)
{
    uint64_t elapsed;
    int64_t  f;
    int64_t  d;
    uint32_t perV;
    uint32_t cmpV;
    uint64_t end = (uint64_t)g_sp[g_count - 1U].tUs * TICKS_PER_US;

    if (g.active == 0U)
    {
        return 0U;
    }
    if (g.tick >= end)
    {
        g.active = 0U;
        return 0U;
    }
    while (g.tick >= g.segEnd)
    {
        LoadSegment((uint16_t)(g.seg + 1U));   /* g.tick < end, so a next segment exists */
    }

    elapsed = g.tick - g.segStart;
    f = (int64_t)g.f0 + ((g.slopeF * (int64_t)elapsed) >> 32);   /* arithmetic shift: floor */
    d = (int64_t)g.d0 + ((g.slopeD * (int64_t)elapsed) >> 32);

    /* period in clocks = round(CLK / f), f in millihertz */
    StepFromPeriod(PeriodClocks((uint32_t)f), (uint32_t)d, g.per1Min, g.per1Max, g.dutyMinPct, g.dutyMaxPct, &perV, &cmpV);

    *per = (uint16_t)perV;
    *cmp = (uint16_t)cmpV;
    g.tick += (uint64_t)(perV + 1U) * g.periodsPerEntry;   /* the CLAMPED period is what elapses */
    *last = (g.tick >= end) ? 1U : 0U;
    if (*last != 0U)
    {
        g.active = 0U;
    }
    return 1U;
}

uint8_t ShotProfile_StepFor(uint32_t freqMilliHz, uint32_t dutyHundredths, uint16_t *per, uint16_t *cmp)
{
    supply_limits_t lim;
    uint64_t clk = (uint64_t)HRTIM_TIMER_CLK_HZ;
    uint32_t perV;
    uint32_t cmpV;

    if ((freqMilliHz == 0U) || (SupplyMode_GetLimits(SupplyMode_Get(), &lim) == 0U))
    {
        return 0U;
    }
    if (freqMilliHz >= (1UL << 27))
    {
        freqMilliHz = (1UL << 27) - 1U;   /* PeriodClocks()'s input range; the mode's period clamp governs anyway */
    }
    StepFromPeriod(PeriodClocks(freqMilliHz), dutyHundredths,
                   (uint32_t)((clk + lim.freqMaxHz - 1U) / lim.freqMaxHz), (uint32_t)(clk / lim.freqMinHz),
                   lim.dutyMinPct, lim.dutyMaxPct, &perV, &cmpV);
    *per = (uint16_t)perV;
    *cmp = (uint16_t)cmpV;
    return 1U;
}
