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
    return ShotProfile_Append(t, fHz * 1000U, dutyHund);
}

static void TestParse(void)
{
    uint32_t v = 0U;
    CHECK(ShotProfile_ParseScaled("20000", 3U, &v) == 1U && v == 20000000U);
    CHECK(ShotProfile_ParseScaled("20000.5", 3U, &v) == 1U && v == 20000500U);
    CHECK(ShotProfile_ParseScaled("0.125", 3U, &v) == 1U && v == 125U);
    CHECK(ShotProfile_ParseScaled("47.5", 2U, &v) == 1U && v == 4750U);
    CHECK(ShotProfile_ParseScaled("50", 2U, &v) == 1U && v == 5000U);
    CHECK(ShotProfile_ParseScaled("100.", 2U, &v) == 1U && v == 10000U);
    CHECK(ShotProfile_ParseScaled("1500", 0U, &v) == 1U && v == 1500U);
    CHECK(ShotProfile_ParseScaled(".5", 2U, &v) == 1U && v == 50U);
    CHECK(ShotProfile_ParseScaled("47.555", 2U, &v) == 0U);      /* more decimals than the unit has */
    CHECK(ShotProfile_ParseScaled("1.5", 0U, &v) == 0U);         /* no decimals allowed */
    CHECK(ShotProfile_ParseScaled("", 2U, &v) == 0U);
    CHECK(ShotProfile_ParseScaled(".", 2U, &v) == 0U);
    CHECK(ShotProfile_ParseScaled("-5", 2U, &v) == 0U);
    CHECK(ShotProfile_ParseScaled("1e3", 2U, &v) == 0U);
    CHECK(ShotProfile_ParseScaled("1.2.3", 2U, &v) == 0U);
    CHECK(ShotProfile_ParseScaled("5 ", 2U, &v) == 0U);
    CHECK(ShotProfile_ParseScaled("4294967296", 0U, &v) == 0U);  /* overflow */
    CHECK(ShotProfile_ParseScaled("4294967295", 0U, &v) == 1U && v == 4294967295U);
    CHECK(ShotProfile_ParseScaled("99999999999", 3U, &v) == 0U);
    CHECK(ShotProfile_ParseScaled(NULL, 2U, &v) == 0U);
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
    CHECK(ShotProfile_Append(8000U, 9999999U, 5000U) == SHOT_APPEND_FREQ_LOW);
    CHECK(ShotProfile_Append(8000U, 30000001U, 5000U) == SHOT_APPEND_FREQ_HIGH);
    CHECK(ShotProfile_Append(8000U, 20000000U, 3999U) == SHOT_APPEND_DUTY_LOW);
    CHECK(ShotProfile_Append(8000U, 20000000U, 6001U) == SHOT_APPEND_DUTY_HIGH);
    CHECK(ShotProfile_Count() == 4U);                                /* none of those were stored */

    /* longest shot */
    CHECK(ShotProfile_Append(SHOT_MAX_DURATION_US, 20000000U, 5000U) == SHOT_APPEND_OK);
    CHECK(ShotProfile_DurationUs() == SHOT_MAX_DURATION_US);
    CHECK(ShotProfile_Append(SHOT_MAX_DURATION_US + 1U, 20000000U, 5000U) == SHOT_APPEND_TOO_LONG);

    shot_setpoint_t sp;
    CHECK(ShotProfile_Get(1U, &sp) == 1U && sp.tUs == 5000U && sp.freqMilliHz == 15000000U && sp.dutyHundredths == 5000U);
    CHECK(ShotProfile_Get(ShotProfile_Count(), &sp) == 0U);

    /* LIMITER limits: 75-125 kHz, 5-95 % */
    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    ShotProfile_Clear();
    CHECK(Add(0U, 75000U, 500U) == SHOT_APPEND_OK);
    CHECK(Add(10U, 125000U, 9500U) == SHOT_APPEND_OK);
    CHECK(ShotProfile_Append(20U, 74999999U, 5000U) == SHOT_APPEND_FREQ_LOW);
    CHECK(ShotProfile_Append(20U, 125000001U, 5000U) == SHOT_APPEND_FREQ_HIGH);
    CHECK(ShotProfile_Append(20U, 100000000U, 499U) == SHOT_APPEND_DUTY_LOW);
    CHECK(ShotProfile_Append(20U, 100000000U, 9501U) == SHOT_APPEND_DUTY_HIGH);

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
    uint16_t per, cmp;
    uint8_t last = 0U;
    uint32_t n = 0U;
    uint16_t prevPer = 0U;
    int sawLastEarly = 0;

    *total = 0U; *monotonicPer = 1; *lastFlagOk = 1;
    CHECK(ShotGen_Start(periodsPerEntry) == 1U);
    while (ShotGen_Next(&per, &cmp, &last) != 0U)
    {
        uint16_t c3[3] = { cmp, cmp, cmp };
        CHECK(SupplyMode_CheckStep(per, c3, 3U) == SUPPLY_STEP_OK);
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
    CHECK(ShotGen_Next(&per, &cmp, &last) == 0U);                 /* stays finished */
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
    uint16_t per = 0, cmp = 0; uint8_t last = 0;
    CHECK(ShotGen_Start(1U) == 1U);
    for (int i = 0; i <= 80; i++) { CHECK(ShotGen_Next(&per, &cmp, &last) == 1U); }
    printf("GOLDEN ramp   entry80 per %u cmp %u\n", per, cmp);

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

int main(void)
{
    TestParse();
    TestUploadRules();
    TestGeneratorFlat();
    TestGeneratorRamp();
    TestGeneratorLimiterExtremes();
    TestGeneratorLongShot();
    return test_report("test_shot_profile");
}
