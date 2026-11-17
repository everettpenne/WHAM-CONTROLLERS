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

static uint32_t g_revision = 0U;

uint32_t ShotProfile_Revision(void)
{
    return g_revision;
}

void ShotProfile_Clear(void)
{
    g_count = 0U;
    g_revision++;
}

static uint8_t g_badChannel = 0U;

shot_append_result_t ShotProfile_Append(uint32_t tUs, uint32_t freqMilliHz,
                                        const uint16_t dutyHundredths[HRTIM_NUM_CHANNELS])
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
    if (freqMilliHz == 0U)
    {
        /* an off setpoint (a gap): no frequency or duty to check -- but never the first one */
        if (g_count == 0U)
        {
            return SHOT_APPEND_FIRST_OFF;
        }
    }
    else
    {
        if (freqMilliHz < (uint32_t)lim.freqMinHz * 1000UL)
        {
            return SHOT_APPEND_FREQ_LOW;
        }
        if ((uint64_t)freqMilliHz > (uint64_t)lim.freqMaxHz * 1000U)
        {
            return SHOT_APPEND_FREQ_HIGH;
        }
        for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
        {
            g_badChannel = ch;
            if (dutyHundredths[ch] < (uint16_t)(lim.dutyMinPct * 100U))
            {
                return SHOT_APPEND_DUTY_LOW;
            }
            if (dutyHundredths[ch] > (uint16_t)(lim.dutyMaxPct * 100U))
            {
                return SHOT_APPEND_DUTY_HIGH;
            }
        }
    }

    g_sp[g_count].tUs         = tUs;
    g_sp[g_count].freqMilliHz = freqMilliHz;
    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        g_sp[g_count].dutyHundredths[ch] = (freqMilliHz == 0U) ? 0U : dutyHundredths[ch];   /* an off setpoint keeps no duty */
    }
    g_count++;
    g_revision++;
    return SHOT_APPEND_OK;
}

shot_append_result_t ShotProfile_AppendUniform(uint32_t tUs, uint32_t freqMilliHz, uint16_t dutyHundredths)
{
    uint16_t d[HRTIM_NUM_CHANNELS];

    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        d[ch] = dutyHundredths;
    }
    return ShotProfile_Append(tUs, freqMilliHz, d);
}

uint8_t ShotProfile_BadChannel(void)
{
    return g_badChannel;
}

uint8_t ShotProfile_IsUniform(void)
{
    for (uint16_t i = 0U; i < g_count; i++)
    {
        for (uint8_t ch = 1U; ch < HRTIM_NUM_CHANNELS; ch++)
        {
            if (g_sp[i].dutyHundredths[ch] != g_sp[i].dutyHundredths[0])
            {
                return 0U;
            }
        }
    }
    return 1U;
}

uint8_t ShotProfile_HasGaps(void)
{
    for (uint16_t i = 0U; i < g_count; i++)
    {
        if (g_sp[i].freqMilliHz == 0U)
        {
            return 1U;
        }
    }
    return 0U;
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
    /* the first setpoint is on by Append(); the last two must be on: the shot cannot end in a gap */
    return ((g_count >= 2U) && (g_sp[g_count - 1U].freqMilliHz != 0U) && (g_sp[g_count - 2U].freqMilliHz != 0U)) ? 1U : 0U;
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
    uint8_t  off;            /* this segment is a gap: its start setpoint is off */
    uint8_t  uniform;        /* every channel has the same duty and slope in this segment: one compare serves all */
    uint16_t seg;            /* current segment = setpoints seg .. seg+1 */
    uint64_t tick;           /* start of the next entry, in HRTIM clocks since t = 0 */
    uint64_t segStart;       /* the segment's start and end, in clocks */
    uint64_t segEnd;
    uint32_t f0;             /* setpoint seg: millihertz and hundredths of a percent */
    uint32_t d0[HRTIM_NUM_CHANNELS];
    int64_t  slopeF;         /* per clock, Q32 */
    int64_t  slopeD[HRTIM_NUM_CHANNELS];
    /* limits of the selected mode, as period / compare bounds */
    uint32_t per1Min;        /* shortest period, in clocks (highest allowed frequency) */
    uint32_t per1Max;
    uint8_t  dutyMinPct;
    uint8_t  dutyMaxPct;
} gen_t;

/* An off period is at most this long, in clocks: a gap is played as a run of off entries, and the longest the
   16-bit period register takes is 65535 (some margin kept). */
#define OFF_PERIOD_MAX_CLOCKS  (60000U)

static gen_t g;      /* the generator (the refill interrupt) */
static gen_t e;      /* the evaluator (ShotEval_At(), the control interrupt): its own state, the same math */

static void LoadSegment(gen_t *s, uint16_t seg)
{
    const shot_setpoint_t *a = &g_sp[seg];
    const shot_setpoint_t *b = &g_sp[seg + 1U];
    int64_t dt;

    s->seg      = seg;
    s->segStart = (uint64_t)a->tUs * TICKS_PER_US;
    s->segEnd   = (uint64_t)b->tUs * TICKS_PER_US;
    s->f0       = a->freqMilliHz;
    dt          = (int64_t)(s->segEnd - s->segStart);
    /* slope = delta * 2^32 / dt, truncated toward zero (the host port does the same) */
    s->slopeF   = ((int64_t)b->freqMilliHz - (int64_t)a->freqMilliHz) * 4294967296LL / dt;
    s->uniform  = 1U;
    s->off      = (a->freqMilliHz == 0U) ? 1U : 0U;
    if (b->freqMilliHz == 0U)
    {
        s->slopeF = 0;                    /* on -> off: hold the start setpoint's values until the gap begins */
    }
    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        s->d0[ch]     = a->dutyHundredths[ch];
        s->slopeD[ch] = (b->freqMilliHz == 0U)
                          ? 0
                          : ((int64_t)b->dutyHundredths[ch] - (int64_t)a->dutyHundredths[ch]) * 4294967296LL / dt;
        if ((s->d0[ch] != s->d0[0]) || (s->slopeD[ch] != s->slopeD[0]))
        {
            s->uniform = 0U;
        }
    }
}

/* The compare for a period (in clocks, minus one) and a duty in hundredths of a percent, clamped onto the
   mode's exact integer duty limits (the relation SupplyMode_CheckStep() uses: cmp*100 against pct*per). */
static uint32_t CompareFromPeriod(uint32_t perV, uint32_t dutyHund, uint8_t dutyMinPct, uint8_t dutyMaxPct)
{
    uint32_t cmpV  = (perV * dutyHund + 5000U) / 10000U;
    uint32_t cmpLo = ((uint32_t)dutyMinPct * perV + 99U) / 100U;
    uint32_t cmpHi = ((uint32_t)dutyMaxPct * perV) / 100U;

    if (cmpV < cmpLo)
    {
        cmpV = cmpLo;
    }
    if (cmpV > cmpHi)
    {
        cmpV = cmpHi;
    }
    return cmpV;
}

/* The period (clocks minus one) for a period in clocks, clamped into [per1Min, per1Max]. */
static uint32_t PeriodFromClocks(uint32_t per1, uint32_t per1Min, uint32_t per1Max)
{
    if (per1 < per1Min)
    {
        per1 = per1Min;
    }
    if (per1 > per1Max)
    {
        per1 = per1Max;
    }
    return per1 - 1U;
}

static void LoadLimits(gen_t *s, const supply_limits_t *lim)
{
    uint64_t clk = (uint64_t)HRTIM_TIMER_CLK_HZ;

    /* f <= fmax  <=>  period >= CLK / fmax (round up);
       f >= fmin  <=>  period <= CLK / fmin (round down). */
    s->per1Min    = (uint32_t)((clk + lim->freqMaxHz - 1U) / lim->freqMaxHz);
    s->per1Max    = (uint32_t)(clk / lim->freqMinHz);
    s->dutyMinPct = lim->dutyMinPct;
    s->dutyMaxPct = lim->dutyMaxPct;
}

uint8_t ShotGen_Start(uint8_t periodsPerEntry)
{
    supply_limits_t lim;

    g.active = 0U;
    if ((ShotProfile_IsComplete() == 0U) || (periodsPerEntry == 0U) ||
        (SupplyMode_GetLimits(SupplyMode_Get(), &lim) == 0U))
    {
        return 0U;
    }
    LoadLimits(&g, &lim);
    g.periodsPerEntry = periodsPerEntry;
    g.tick       = 0U;
    LoadSegment(&g, 0U);
    g.active     = 1U;
    return 1U;
}

uint8_t ShotGen_Next(uint16_t *per, uint16_t cmp[HRTIM_NUM_CHANNELS], uint8_t *last)
{
    uint64_t elapsed;
    int64_t  f;
    uint32_t perV;
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
        LoadSegment(&g, (uint16_t)(g.seg + 1U));   /* g.tick < end, so a next segment exists */
    }

    if (g.off != 0U)
    {
        /* A gap: a run of off entries (every compare 0) that ends with one short off period as long as the
           first period after the gap, so that the output-off period the playback sits in when the next on
           entry arrives is no longer than one on period (the output comes back at the next period boundary,
           pfm.c). `len` is the entry's length in clocks. */
        const shot_setpoint_t *nb = &g_sp[g.seg + 1U];
        uint64_t remaining = g.segEnd - g.tick;
        uint64_t r;
        uint32_t len;
        uint32_t finalLen = 0U;

        if (nb->freqMilliHz != 0U)
        {
            finalLen = PeriodFromClocks(PeriodClocks(nb->freqMilliHz), g.per1Min, g.per1Max) + 1U;
        }
        if ((finalLen != 0U) && (remaining <= (uint64_t)finalLen + g.per1Min))
        {
            len = finalLen;
        }
        else
        {
            uint64_t chunks;

            r = remaining - finalLen;
            chunks = (r + OFF_PERIOD_MAX_CLOCKS - 1U) / OFF_PERIOD_MAX_CLOCKS;   /* r > 0 here, so at least 1 */
            len = (uint32_t)(r / chunks);
            if (len < g.per1Min)
            {
                len = g.per1Min;
            }
        }
        *per = (uint16_t)(len - 1U);
        for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
        {
            cmp[ch] = 0U;
        }
        g.tick += (uint64_t)len * g.periodsPerEntry;
        *last = (g.tick >= end) ? 1U : 0U;
        if (*last != 0U)
        {
            g.active = 0U;
        }
        return 1U;
    }

    elapsed = g.tick - g.segStart;
    f = (int64_t)g.f0 + ((g.slopeF * (int64_t)elapsed) >> 32);   /* arithmetic shift: floor */

    /* period in clocks = round(CLK / f), f in millihertz */
    perV = PeriodFromClocks(PeriodClocks((uint32_t)f), g.per1Min, g.per1Max);
    *per = (uint16_t)perV;
    if (g.uniform != 0U)
    {
        /* the usual case, and what every shot cost before channels had their own duty: one compare for all */
        int64_t  d = (int64_t)g.d0[0] + ((g.slopeD[0] * (int64_t)elapsed) >> 32);
        uint16_t c = (uint16_t)CompareFromPeriod(perV, (uint32_t)d, g.dutyMinPct, g.dutyMaxPct);

        for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
        {
            cmp[ch] = c;
        }
    }
    else
    {
        for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
        {
            int64_t d = (int64_t)g.d0[ch] + ((g.slopeD[ch] * (int64_t)elapsed) >> 32);   /* arithmetic shift: floor */

            cmp[ch] = (uint16_t)CompareFromPeriod(perV, (uint32_t)d, g.dutyMinPct, g.dutyMaxPct);
        }
    }
    g.tick += (uint64_t)(perV + 1U) * g.periodsPerEntry;   /* the CLAMPED period is what elapses */
    *last = (g.tick >= end) ? 1U : 0U;
    if (*last != 0U)
    {
        g.active = 0U;
    }
    return 1U;
}

uint8_t ShotProfile_PeriodFor(uint32_t freqMilliHz, uint16_t *per)
{
    supply_limits_t lim;
    uint64_t clk = (uint64_t)HRTIM_TIMER_CLK_HZ;

    if ((freqMilliHz == 0U) || (SupplyMode_GetLimits(SupplyMode_Get(), &lim) == 0U))
    {
        return 0U;
    }
    if (freqMilliHz >= (1UL << 27))
    {
        freqMilliHz = (1UL << 27) - 1U;   /* PeriodClocks()'s input range; the mode's period clamp governs anyway */
    }
    *per = (uint16_t)PeriodFromClocks(PeriodClocks(freqMilliHz),
                                      (uint32_t)((clk + lim.freqMaxHz - 1U) / lim.freqMaxHz),
                                      (uint32_t)(clk / lim.freqMinHz));
    return 1U;
}

uint16_t ShotProfile_CompareFor(uint16_t per, uint32_t dutyHundredths)
{
    supply_limits_t lim;

    if (SupplyMode_GetLimits(SupplyMode_Get(), &lim) == 0U)
    {
        return 0U;
    }
    return (uint16_t)CompareFromPeriod(per, dutyHundredths, lim.dutyMinPct, lim.dutyMaxPct);
}

uint8_t ShotProfile_StepFor(uint32_t freqMilliHz, uint32_t dutyHundredths, uint16_t *per, uint16_t *cmp)
{
    if (ShotProfile_PeriodFor(freqMilliHz, per) == 0U)
    {
        return 0U;
    }
    *cmp = ShotProfile_CompareFor(*per, dutyHundredths);
    return 1U;
}

/* ---- the shot at a given time ---- */

void ShotEval_Rewind(void)
{
    e.active = 0U;     /* LoadSegment() runs on the first call */
}

uint8_t ShotEval_At(uint32_t tUs, uint32_t *freqMilliHz, uint16_t duty[HRTIM_NUM_CHANNELS])
{
    uint64_t tick;
    uint64_t elapsed;
    supply_limits_t lim;

    if ((ShotProfile_IsComplete() == 0U) || (SupplyMode_GetLimits(SupplyMode_Get(), &lim) == 0U))
    {
        return 0U;
    }
    tick = (uint64_t)tUs * TICKS_PER_US;
    if (tick >= (uint64_t)g_sp[g_count - 1U].tUs * TICKS_PER_US)
    {
        *freqMilliHz = g_sp[g_count - 1U].freqMilliHz;
        for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
        {
            duty[ch] = g_sp[g_count - 1U].dutyHundredths[ch];
        }
        return 1U;
    }
    if (e.active == 0U)
    {
        LoadSegment(&e, 0U);
        e.active = 1U;
    }
    while (tick >= e.segEnd)
    {
        LoadSegment(&e, (uint16_t)(e.seg + 1U));
    }
    elapsed = tick - e.segStart;
    if (e.off != 0U)
    {
        *freqMilliHz = 0U;
        for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
        {
            duty[ch] = 0U;
        }
        return 1U;
    }
    *freqMilliHz = (uint32_t)((int64_t)e.f0 + ((e.slopeF * (int64_t)elapsed) >> 32));
    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        duty[ch] = (uint16_t)((int64_t)e.d0[ch] + ((e.slopeD[ch] * (int64_t)elapsed) >> 32));
    }
    return 1U;
}
