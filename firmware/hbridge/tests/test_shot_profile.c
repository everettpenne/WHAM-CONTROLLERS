/*
 * test_shot_profile.c -- the time-series shot (src/app/control/shot_profile.c)
 * on the host: upload rules, the limits of each supply mode, the decimal
 * parser, and the generator that turns setpoints into per-period entries.
 *
 * "GOLDEN" lines print generated entries for fixed inputs; python/pfm_shot_test.py
 * carries the same numbers in its selftest, so the host tool's Python port of
 * the generator is held to this C code tick for tick.
 */
#include "shot_profile.h"
#include "supply_mode.h"
#include "hrtim.h"
#include "test_util.h"
#include <string.h>

static shot_append_result_t Add(uint32_t t, uint32_t fHz, uint16_t dutyHund)
{
    return ShotProfile_AppendUniform(t, fHz * 1000U, dutyHund);
}

static void TestUploadRules(void)
{
    ShotProfile_Clear();
    SupplyMode_Set(SUPPLY_MODE_NONE);
    CHECK(Add(0U, 20000U, 5000U) == SHOT_APPEND_NO_MODE);

    SupplyMode_Set(SUPPLY_MODE_HVPS);
    ShotProfile_Clear();
    CHECK(ShotProfile_Count() == 0U && ShotProfile_DurationUs() == 0U && !ShotProfile_IsComplete());
    CHECK(Add(1U, 20000U, 5000U) == SHOT_APPEND_FIRST_NOT_ZERO);     /* t = 0 is required */
    CHECK(ShotProfile_Count() == 0U);
    CHECK(Add(0U, 20000U, 5000U) == SHOT_APPEND_OK);
    CHECK(ShotProfile_Count() == 1U && !ShotProfile_IsComplete());   /* one point is not a shot */
    CHECK(Add(0U, 20000U, 5000U) == SHOT_APPEND_NOT_INCREASING);
    CHECK(Add(5000U, 15000U, 5000U) == SHOT_APPEND_OK);
    CHECK(Add(5000U, 15000U, 5000U) == SHOT_APPEND_NOT_INCREASING);
    CHECK(Add(4000U, 15000U, 5000U) == SHOT_APPEND_NOT_INCREASING);
    CHECK(ShotProfile_IsComplete() && ShotProfile_DurationUs() == 5000U);

    /* HVPS limits: 10-30 kHz, 40-60 %, exactly on and one step outside */
    CHECK(Add(6000U, 10000U, 4000U) == SHOT_APPEND_OK);
    CHECK(Add(7000U, 30000U, 6000U) == SHOT_APPEND_OK);
    CHECK(ShotProfile_AppendUniform(8000U, 9999999U, 5000U) == SHOT_APPEND_FREQ_LOW);
    CHECK(ShotProfile_AppendUniform(8000U, 30000001U, 5000U) == SHOT_APPEND_FREQ_HIGH);
    CHECK(ShotProfile_AppendUniform(8000U, 20000000U, 3999U) == SHOT_APPEND_DUTY_LOW);
    CHECK(ShotProfile_AppendUniform(8000U, 20000000U, 6001U) == SHOT_APPEND_DUTY_HIGH);
    CHECK(ShotProfile_Count() == 4U);                                /* none of those were stored */

    /* longest shot */
    CHECK(ShotProfile_AppendUniform(SHOT_MAX_DURATION_US, 20000000U, 5000U) == SHOT_APPEND_OK);
    CHECK(ShotProfile_DurationUs() == SHOT_MAX_DURATION_US);
    CHECK(ShotProfile_AppendUniform(SHOT_MAX_DURATION_US + 1U, 20000000U, 5000U) == SHOT_APPEND_TOO_LONG);

    shot_setpoint_t sp;
    CHECK(ShotProfile_Get(1U, &sp) == 1U && sp.tUs == 5000U && sp.freqMilliHz == 15000000U &&
          sp.dutyHundredths[0] == 5000U && sp.dutyHundredths[1] == 5000U && sp.dutyHundredths[2] == 5000U);
    CHECK(ShotProfile_Get(ShotProfile_Count(), &sp) == 0U);

    /* LIMITER limits: 75-125 kHz, 5-95 % */
    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    ShotProfile_Clear();
    CHECK(Add(0U, 75000U, 500U) == SHOT_APPEND_OK);
    CHECK(Add(10U, 125000U, 9500U) == SHOT_APPEND_OK);
    CHECK(ShotProfile_AppendUniform(20U, 74999999U, 5000U) == SHOT_APPEND_FREQ_LOW);
    CHECK(ShotProfile_AppendUniform(20U, 125000001U, 5000U) == SHOT_APPEND_FREQ_HIGH);
    CHECK(ShotProfile_AppendUniform(20U, 100000000U, 499U) == SHOT_APPEND_DUTY_LOW);
    CHECK(ShotProfile_AppendUniform(20U, 100000000U, 9501U) == SHOT_APPEND_DUTY_HIGH);

    /* capacity */
    ShotProfile_Clear();
    for (uint32_t i = 0U; i < SHOT_SETPOINT_MAX; i++)
    {
        CHECK(Add(i, 100000U, 5000U) == SHOT_APPEND_OK);
    }
    CHECK(Add(SHOT_SETPOINT_MAX, 100000U, 5000U) == SHOT_APPEND_FULL);
    CHECK(ShotProfile_Count() == SHOT_SETPOINT_MAX);
    ShotProfile_Clear();
    CHECK(ShotProfile_Count() == 0U);
}

/* Runs the generator to its end; returns the number of entries. Every entry is
   checked against the supply mode; `total` gets the sum of the periods (clocks). */
static uint32_t RunAll(uint16_t *firstPer, uint16_t *firstCmp, uint16_t *lastPer, uint64_t *total,
                       int *monotonicPer, int *lastFlagOk, uint8_t periodsPerEntry)
{
    uint16_t per, cmpv[HRTIM_NUM_CHANNELS];
    uint16_t cmp;
    uint8_t last = 0U;
    uint32_t n = 0U;
    uint16_t prevPer = 0U;
    int sawLastEarly = 0;

    *total = 0U; *monotonicPer = 1; *lastFlagOk = 1;
    CHECK(ShotGen_Start(periodsPerEntry) == 1U);
    while (ShotGen_Next(&per, cmpv, &last) != 0U)
    {
        cmp = cmpv[0];
        CHECK(SupplyMode_CheckStep(per, cmpv, HRTIM_NUM_CHANNELS) == SUPPLY_STEP_OK);
        CHECK(cmpv[1] == cmpv[0] && cmpv[2] == cmpv[0]);               /* a one-duty shot gives every channel the same compare */
        if (n == 0U) { *firstPer = per; *firstCmp = cmp; }
        if ((n > 0U) && (per < prevPer)) { *monotonicPer = 0; }   /* meaningful for a falling-frequency ramp */
        if (sawLastEarly) { *lastFlagOk = 0; }
        if (last) { sawLastEarly = 1; }
        prevPer = per;
        *lastPer = per;
        *total += ((uint64_t)per + 1U) * periodsPerEntry;
        n++;
    }
    if (n > 0U && !sawLastEarly) { *lastFlagOk = 0; }             /* the final entry must carry last = 1 */
    CHECK(ShotGen_Next(&per, cmpv, &last) == 0U);                 /* stays finished */
    return n;
}

static void TestGeneratorFlat(void)
{
    uint16_t fp = 0, fc = 0, lp = 0; uint64_t total = 0; int mono = 0, lastOk = 0;

    /* 20 kHz, 50 % for 1 ms: 20 periods of 8500 clocks */
    SupplyMode_Set(SUPPLY_MODE_HVPS);
    ShotProfile_Clear();
    CHECK(Add(0U, 20000U, 5000U) == SHOT_APPEND_OK);
    CHECK(Add(1000U, 20000U, 5000U) == SHOT_APPEND_OK);
    uint32_t n = RunAll(&fp, &fc, &lp, &total, &mono, &lastOk, 1U);
    CHECK(n == 20U);
    CHECK(fp == 8499U && fc == 4250U && lp == 8499U);             /* starts at the first slot's values */
    CHECK(lastOk);
    CHECK(total == 20ULL * 8500ULL);
    printf("GOLDEN flat   first per %u cmp %u n %lu\n", fp, fc, (unsigned long)n);
}

static void TestGeneratorRamp(void)
{
    uint16_t fp = 0, fc = 0, lp = 0; uint64_t total = 0; int mono = 0, lastOk = 0;

    /* 20 -> 15 kHz and 40 -> 60 % over 10 ms */
    SupplyMode_Set(SUPPLY_MODE_HVPS);
    ShotProfile_Clear();
    CHECK(Add(0U, 20000U, 4000U) == SHOT_APPEND_OK);
    CHECK(Add(10000U, 15000U, 6000U) == SHOT_APPEND_OK);
    uint32_t n = RunAll(&fp, &fc, &lp, &total, &mono, &lastOk, 1U);
    CHECK(fp == 8499U && fc == 3400U);                            /* 40 % of 8499 */
    CHECK(mono);                                                  /* frequency falls, so period only grows */
    CHECK(lastOk);
    CHECK(lp > 11000U && lp <= 11332U);                           /* approaches 15 kHz (per 11332) without passing it */
    /* the shot is over at the first period that starts at or after 10 ms:
       it ends less than one period past it */
    CHECK(total >= 10000ULL * 170ULL && total < 10000ULL * 170ULL + 11334ULL);
    CHECK(n > 150U && n < 200U);
    printf("GOLDEN ramp   n %lu last per %u total %llu\n", (unsigned long)n, lp, (unsigned long long)total);

    /* spot-check a middle entry: re-run and read entry 80 */
    uint16_t per = 0, cmp[HRTIM_NUM_CHANNELS] = {0}; uint8_t last = 0;
    CHECK(ShotGen_Start(1U) == 1U);
    for (int i = 0; i <= 80; i++) { CHECK(ShotGen_Next(&per, cmp, &last) == 1U); }
    printf("GOLDEN ramp   entry80 per %u cmp %u\n", per, cmp[0]);

    /* one entry held 4 periods: same shot, a quarter of the entries, same length */
    uint32_t n4 = RunAll(&fp, &fc, &lp, &total, &mono, &lastOk, 4U);
    CHECK(n4 >= n / 4U - 2U && n4 <= n / 4U + 2U);
    CHECK(total >= 10000ULL * 170ULL && total < 10000ULL * 170ULL + 4ULL * 11334ULL);
}

static void TestGeneratorLimiterExtremes(void)
{
    uint16_t fp = 0, fc = 0, lp = 0; uint64_t total = 0; int mono = 0, lastOk = 0;

    /* LIMITER corners: 125 kHz at 5 % rising to 75 kHz at 95 %; every entry must pass
       the mode's exact integer check (RunAll checks it), including the clamps */
    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    ShotProfile_Clear();
    CHECK(Add(0U, 125000U, 500U) == SHOT_APPEND_OK);
    CHECK(Add(10000U, 75000U, 9500U) == SHOT_APPEND_OK);
    uint32_t n = RunAll(&fp, &fc, &lp, &total, &mono, &lastOk, 1U);
    CHECK(fp == 1359U && fc == 68U);                              /* 125 kHz: per 1359; 5 % of it, rounded */
    CHECK(n > 900U && n < 1300U);
    CHECK(mono && lastOk);

    /* 75 kHz is not exactly representable: the period is clamped to the longest
       one that is still >= 75 kHz */
    ShotProfile_Clear();
    CHECK(Add(0U, 75000U, 500U) == SHOT_APPEND_OK);
    CHECK(Add(1000U, 75000U, 500U) == SHOT_APPEND_OK);
    RunAll(&fp, &fc, &lp, &total, &mono, &lastOk, 1U);
    CHECK(fp == 2265U);                                           /* 170e6/75e3 = 2266.67 -> floor 2266 clocks -> per 2265 */
    CHECK(fc == 114U);                                            /* 113.25 would be 4.99 %: clamped up to 5 % */

    /* a 1-microsecond segment shorter than a period is passed over cleanly */
    ShotProfile_Clear();
    CHECK(Add(0U, 100000U, 5000U) == SHOT_APPEND_OK);
    CHECK(Add(1U, 110000U, 5000U) == SHOT_APPEND_OK);
    CHECK(Add(100U, 90000U, 5000U) == SHOT_APPEND_OK);
    CHECK(RunAll(&fp, &fc, &lp, &total, &mono, &lastOk, 1U) >= 9U);
    CHECK(lastOk);
}

static void TestGeneratorLongShot(void)
{
    uint16_t fp = 0, fc = 0, lp = 0; uint64_t total = 0; int mono = 0, lastOk = 0;

    /* the longest shot: 1 s at 20 kHz is 20,000 periods */
    SupplyMode_Set(SUPPLY_MODE_HVPS);
    ShotProfile_Clear();
    CHECK(Add(0U, 20000U, 5000U) == SHOT_APPEND_OK);
    CHECK(Add(SHOT_MAX_DURATION_US, 20000U, 5000U) == SHOT_APPEND_OK);
    CHECK(RunAll(&fp, &fc, &lp, &total, &mono, &lastOk, 1U) == 20000U);

    /* no complete shot / no mode: the generator refuses */
    ShotProfile_Clear();
    CHECK(ShotGen_Start(1U) == 0U);
    CHECK(Add(0U, 20000U, 5000U) == SHOT_APPEND_OK);
    CHECK(ShotGen_Start(1U) == 0U);                               /* one setpoint */
    CHECK(Add(100U, 20000U, 5000U) == SHOT_APPEND_OK);
    CHECK(ShotGen_Start(0U) == 0U);                               /* zero periods per entry */
    SupplyMode_Set(SUPPLY_MODE_NONE);
    CHECK(ShotGen_Start(1U) == 0U);
}

/* The period of the first entry of a flat shot must equal the plain 64-bit formula
   round(CLK * 1000 / f), clamped to the mode's range -- at every frequency in both
   modes (the firmware uses a 32-bit-only long division for speed). */
static void TestPeriodDivision(void)
{
    const supply_mode_t modes[2] = { SUPPLY_MODE_HVPS, SUPPLY_MODE_LIMITER };
    uint32_t bad = 0U, n = 0U;

    for (int m = 0; m < 2; m++)
    {
        supply_limits_t lim;
        SupplyMode_Set(modes[m]);
        SupplyMode_GetLimits(modes[m], &lim);
        uint64_t clk = HRTIM_TIMER_CLK_HZ;
        uint64_t per1Min = (clk + lim.freqMaxHz - 1U) / lim.freqMaxHz;
        uint64_t per1Max = clk / lim.freqMinHz;

        for (uint32_t f = lim.freqMinHz * 1000U; f <= lim.freqMaxHz * 1000U; f += (f < lim.freqMinHz * 1000U + 5000U) ? 1U : 1013U)
        {
            uint16_t per, cmp[HRTIM_NUM_CHANNELS]; uint8_t last;
            uint64_t ref = (clk * 1000U + f / 2U) / f;
            if (ref < per1Min) { ref = per1Min; }
            if (ref > per1Max) { ref = per1Max; }
            ShotProfile_Clear();
            if (ShotProfile_AppendUniform(0U, f, (uint16_t)(lim.dutyMinPct * 100U + 100U)) != SHOT_APPEND_OK ||
                ShotProfile_AppendUniform(1U, f, (uint16_t)(lim.dutyMinPct * 100U + 100U)) != SHOT_APPEND_OK ||
                ShotGen_Start(1U) == 0U || ShotGen_Next(&per, cmp, &last) == 0U)
            {
                bad++;
                continue;
            }
            n++;
            if ((uint64_t)per + 1U != ref) { bad++; }
        }
        /* the very top of the range, where the long division's remainder is largest */
        for (uint32_t f = lim.freqMaxHz * 1000U - 40U; f <= lim.freqMaxHz * 1000U; f++)
        {
            uint16_t per, cmp[HRTIM_NUM_CHANNELS]; uint8_t last;
            uint64_t ref = (clk * 1000U + f / 2U) / f;
            if (ref < per1Min) { ref = per1Min; }
            ShotProfile_Clear();
            ShotProfile_AppendUniform(0U, f, (uint16_t)(lim.dutyMinPct * 100U + 100U));
            ShotProfile_AppendUniform(1U, f, (uint16_t)(lim.dutyMinPct * 100U + 100U));
            ShotGen_Start(1U); ShotGen_Next(&per, cmp, &last);
            n++;
            if ((uint64_t)per + 1U != ref) { bad++; }
        }
    }
    CHECK(bad == 0U);
    CHECK(n > 60000U);
    printf("  period division checked at %lu frequencies, %lu mismatches\n", (unsigned long)n, (unsigned long)bad);
}


/* ---- one duty per channel (2026-10-13) ---- */

static shot_append_result_t Add3(uint32_t t, uint32_t fHz, uint16_t d0, uint16_t d1, uint16_t d2)
{
    const uint16_t d[HRTIM_NUM_CHANNELS] = { d0, d1, d2 };

    return ShotProfile_Append(t, fHz * 1000U, d);
}

static void TestPerChannelUpload(void)
{
    shot_setpoint_t sp;

    CHECK(HRTIM_NUM_CHANNELS == 3U);                               /* the tests below are written for U, V, W */
    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    ShotProfile_Clear();
    CHECK(Add3(0U, 100000U, 1000U, 5000U, 9000U) == SHOT_APPEND_OK);
    CHECK(!ShotProfile_IsUniform());
    CHECK(ShotProfile_Get(0U, &sp) == 1U && sp.dutyHundredths[0] == 1000U && sp.dutyHundredths[1] == 5000U &&
          sp.dutyHundredths[2] == 9000U);

    /* each channel is checked against the mode's limits (5-95 %), and the channel that failed is reported */
    CHECK(Add3(10U, 100000U, 499U, 5000U, 5000U) == SHOT_APPEND_DUTY_LOW && ShotProfile_BadChannel() == 0U);
    CHECK(Add3(10U, 100000U, 5000U, 499U, 5000U) == SHOT_APPEND_DUTY_LOW && ShotProfile_BadChannel() == 1U);
    CHECK(Add3(10U, 100000U, 5000U, 5000U, 9501U) == SHOT_APPEND_DUTY_HIGH && ShotProfile_BadChannel() == 2U);
    CHECK(Add3(10U, 100000U, 500U, 5000U, 9500U) == SHOT_APPEND_OK);       /* exactly on both limits */
    CHECK(ShotProfile_Count() == 2U);                                      /* the refused ones were not stored */

    /* the one-duty form is the same as three equal duties, and a shot of those is "uniform" */
    ShotProfile_Clear();
    CHECK(ShotProfile_IsUniform());                                        /* nothing: trivially */
    CHECK(ShotProfile_AppendUniform(0U, 100000000U, 5000U) == SHOT_APPEND_OK);
    CHECK(Add3(10U, 100000U, 5000U, 5000U, 5000U) == SHOT_APPEND_OK);
    CHECK(ShotProfile_IsUniform());
    CHECK(Add3(20U, 100000U, 5000U, 5001U, 5000U) == SHOT_APPEND_OK);
    CHECK(!ShotProfile_IsUniform());
}

#define MAXN 4096U
static uint16_t g_per[MAXN];
static uint16_t g_cmp[HRTIM_NUM_CHANNELS][MAXN];

static uint32_t Collect(uint16_t per[], uint16_t cmp[HRTIM_NUM_CHANNELS][MAXN], uint8_t periodsPerEntry)
{
    uint16_t p, c[HRTIM_NUM_CHANNELS];
    uint8_t last;
    uint32_t n = 0U;

    CHECK(ShotGen_Start(periodsPerEntry) == 1U);
    while (ShotGen_Next(&p, c, &last) != 0U)
    {
        CHECK(SupplyMode_CheckStep(p, c, HRTIM_NUM_CHANNELS) == SUPPLY_STEP_OK);
        if (n < MAXN)
        {
            per[n] = p;
            for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++) { cmp[ch][n] = c[ch]; }
        }
        n++;
    }
    return n;
}

/* Every channel of a per-channel shot is exactly what a one-duty shot with that channel's duties produces. */
static void TestPerChannelGenerator(void)
{
    static const uint16_t kD[3][3] = { { 1000U, 9000U, 2500U },        /* channel U at the three setpoints (hundredths of %) */
                                       { 5000U, 5000U, 7500U },        /* V */
                                       { 9000U, 1000U, 9500U } };      /* W */
    static const uint32_t kF[3] = { 125000U, 100000U, 80000U };         /* Hz */
    static const uint32_t kT[3] = { 0U, 6000U, 10000U };               /* us */
    static uint16_t refPer[MAXN];
    static uint16_t refCmp[HRTIM_NUM_CHANNELS][MAXN];
    uint32_t n;
    uint32_t differing = 0U;

    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    ShotProfile_Clear();
    for (int i = 0; i < 3; i++)
    {
        CHECK(Add3(kT[i], kF[i], kD[0][i], kD[1][i], kD[2][i]) == SHOT_APPEND_OK);
    }
    n = Collect(g_per, g_cmp, 1U);
    CHECK(n > 900U && n < MAXN);

    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        uint32_t m;

        ShotProfile_Clear();
        for (int i = 0; i < 3; i++)
        {
            CHECK(ShotProfile_AppendUniform(kT[i], kF[i] * 1000U, kD[ch][i]) == SHOT_APPEND_OK);
        }
        m = Collect(refPer, refCmp, 1U);
        CHECK(m == n);
        for (uint32_t k = 0U; (k < n) && (k < m); k++)
        {
            if ((refPer[k] != g_per[k]) || (refCmp[0][k] != g_cmp[ch][k]))
            {
                CHECK(0 && "per-channel entry differs from the one-duty reference");
                break;
            }
        }
    }
    for (uint32_t k = 0U; k < n; k++)
    {
        if ((g_cmp[0][k] != g_cmp[1][k]) || (g_cmp[1][k] != g_cmp[2][k])) { differing++; }
    }
    CHECK(differing > n / 2U);                                       /* the channels really do play different duties */
    /* the first entry is each channel's first setpoint, the 125 kHz period */
    CHECK(g_per[0] == 1359U);
    CHECK(g_cmp[0][0] == 136U && g_cmp[1][0] == 680U && g_cmp[2][0] == 1223U);
    printf("GOLDEN perch  n %lu entry0 per %u cmp %u %u %u\n", (unsigned long)n, g_per[0], g_cmp[0][0], g_cmp[1][0], g_cmp[2][0]);
    printf("GOLDEN perch  entry300 per %u cmp %u %u %u\n", g_per[300], g_cmp[0][300], g_cmp[1][300], g_cmp[2][300]);

    /* held several periods per entry: the same curve, the channels still independent */
    ShotProfile_Clear();
    for (int i = 0; i < 3; i++)
    {
        CHECK(Add3(kT[i], kF[i], kD[0][i], kD[1][i], kD[2][i]) == SHOT_APPEND_OK);
    }
    CHECK(Collect(g_per, g_cmp, 4U) >= n / 4U - 2U);
}

/* within one unit: the generator's integer interpolation truncates the slope and floors the product (documented, and
   what python/pfm_shot_test.py ports), so a value can land one unit under the ideal straight line */
static int Near(int64_t got, int64_t want)
{
    return (got >= want - 1) && (got <= want);
}

/* The evaluator that mixed open/closed-loop runs use: linear in time, the last values held. */
static void TestShotEval(void)
{
    uint32_t f = 0U;
    uint16_t d[HRTIM_NUM_CHANNELS];

    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    ShotProfile_Clear();
    CHECK(ShotEval_At(0U, &f, d) == 0U);                              /* no shot */
    CHECK(Add3(0U, 80000U, 1000U, 5000U, 9000U) == SHOT_APPEND_OK);
    CHECK(Add3(10000U, 120000U, 9000U, 5000U, 1000U) == SHOT_APPEND_OK);
    CHECK(Add3(20000U, 100000U, 5000U, 6000U, 7000U) == SHOT_APPEND_OK);

    ShotEval_Rewind();
    CHECK(ShotEval_At(0U, &f, d) == 1U);
    CHECK(f == 80000000U && d[0] == 1000U && d[1] == 5000U && d[2] == 9000U);
    CHECK(ShotEval_At(2500U, &f, d) == 1U);                           /* a quarter of the way through the first segment */
    CHECK(Near(f, 90000000) && Near(d[0], 3000) && d[1] == 5000U && Near(d[2], 7000));
    CHECK(ShotEval_At(5000U, &f, d) == 1U);
    CHECK(Near(f, 100000000) && Near(d[0], 5000) && d[1] == 5000U && Near(d[2], 5000));
    CHECK(ShotEval_At(10000U, &f, d) == 1U);                          /* the middle setpoint, from the next segment */
    CHECK(f == 120000000U && d[0] == 9000U && d[1] == 5000U && d[2] == 1000U);
    CHECK(ShotEval_At(15000U, &f, d) == 1U);
    CHECK(Near(f, 110000000) && Near(d[0], 7000) && Near(d[1], 5500) && Near(d[2], 4000));
    CHECK(ShotEval_At(20000U, &f, d) == 1U);                          /* the end, and beyond it, hold the last setpoint */
    CHECK(f == 100000000U && d[0] == 5000U && d[1] == 6000U && d[2] == 7000U);
    CHECK(ShotEval_At(900000U, &f, d) == 1U);
    CHECK(f == 100000000U && d[0] == 5000U && d[1] == 6000U && d[2] == 7000U);

    ShotEval_Rewind();                                                /* a second run starts again from t = 0 */
    CHECK(ShotEval_At(0U, &f, d) == 1U && f == 80000000U && d[0] == 1000U);

    ShotProfile_Clear();
    CHECK(Add3(0U, 80000U, 1000U, 5000U, 9000U) == SHOT_APPEND_OK);   /* one setpoint is not a shot */
    CHECK(ShotEval_At(0U, &f, d) == 0U);
}

/* ShotProfile_StepFor() is the composition of its two halves, which the closed loop uses per channel. */
static void TestStepForHalves(void)
{
    const uint32_t fs[] = { 75000000U, 99999999U, 100000000U, 125000000U };
    const uint32_t ds[] = { 400U, 500U, 5000U, 9500U, 9600U };

    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    for (unsigned i = 0U; i < sizeof(fs) / sizeof(fs[0]); i++)
    {
        for (unsigned j = 0U; j < sizeof(ds) / sizeof(ds[0]); j++)
        {
            uint16_t per, cmp, per2, cmp2;

            CHECK(ShotProfile_StepFor(fs[i], ds[j], &per, &cmp) == 1U);
            CHECK(ShotProfile_PeriodFor(fs[i], &per2) == 1U && per2 == per);
            cmp2 = ShotProfile_CompareFor(per2, ds[j]);
            CHECK(cmp2 == cmp);
        }
    }
    CHECK(ShotProfile_PeriodFor(0U, &(uint16_t){0}) == 0U);
}

int main(void)
{
    TestUploadRules();
    TestGeneratorFlat();
    TestGeneratorRamp();
    TestGeneratorLimiterExtremes();
    TestGeneratorLongShot();
    TestPeriodDivision();
    TestPerChannelUpload();
    TestPerChannelGenerator();
    TestShotEval();
    TestStepForHalves();
    return test_report("test_shot_profile");
}
