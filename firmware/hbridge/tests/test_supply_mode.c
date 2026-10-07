/*
 * test_supply_mode.c -- the supply-mode limits (src/app/control/supply_mode.c)
 * on the host: frequency and duty boundaries of both modes, exactly on the
 * limit and one count either side, and the no-mode state.
 */
#include "supply_mode.h"
#include "hrtim.h"
#include "test_util.h"
#include <string.h>

/* period register value for a frequency, same formula as the host builder
   (python/pfm_table_upload.py per_from_freq()): round(clock / f) - 1 */
static uint16_t PerFromFreq(uint32_t hz)
{
    return (uint16_t)(((uint64_t)HRTIM_TIMER_CLK_HZ + hz / 2U) / hz - 1U);
}

static supply_step_result_t Check(uint16_t per, uint16_t c0, uint16_t c1, uint16_t c2)
{
    uint16_t cmp[3];
    cmp[0] = c0; cmp[1] = c1; cmp[2] = c2;
    return SupplyMode_CheckStep(per, cmp, 3U);
}

static uint16_t Half(uint16_t per) { return (uint16_t)(per / 2U); }

static void TestNoModeAndNames(void)
{
    CHECK(SupplyMode_Get() == SUPPLY_MODE_NONE);               /* boot state */
    CHECK(Check(1699U, 850U, 850U, 850U) == SUPPLY_STEP_NO_MODE);
    CHECK_STR(SupplyMode_Name(SUPPLY_MODE_NONE), "NONE");
    CHECK_STR(SupplyMode_Name(SUPPLY_MODE_LIMITER), "LIMITER");
    CHECK_STR(SupplyMode_Name(SUPPLY_MODE_HVPS), "HVPS");
    supply_limits_t l;
    CHECK(SupplyMode_GetLimits(SUPPLY_MODE_NONE, &l) == 0U);
    CHECK(SupplyMode_GetLimits(SUPPLY_MODE_LIMITER, &l) == 1U);
    CHECK(l.freqMinHz == 75000U && l.freqMaxHz == 125000U && l.dutyMinPct == 5U && l.dutyMaxPct == 95U);
    CHECK(SupplyMode_GetLimits(SUPPLY_MODE_HVPS, &l) == 1U);
    CHECK(l.freqMinHz == 10000U && l.freqMaxHz == 30000U && l.dutyMinPct == 40U && l.dutyMaxPct == 60U);
    CHECK(SupplyMode_Set(SUPPLY_MODE_LIMITER) == 1U);          /* changed */
    CHECK(SupplyMode_Set(SUPPLY_MODE_LIMITER) == 0U);          /* already selected */
    CHECK(SupplyMode_Set(SUPPLY_MODE_NONE) == 1U);
}

static void TestLimiterFrequency(void)
{
    (void)SupplyMode_Set(SUPPLY_MODE_LIMITER);
    /* per = CLK/f - 1: 125 kHz -> 1359, 100 kHz -> 1699, 75 kHz -> 2265.67 */
    CHECK(PerFromFreq(125000U) == 1359U);
    CHECK(Check(1359U, Half(1359U), Half(1359U), Half(1359U)) == SUPPLY_STEP_OK);         /* exactly 125 kHz */
    CHECK(Check(1358U, Half(1358U), Half(1358U), Half(1358U)) == SUPPLY_STEP_FREQ_HIGH);  /* one count above */
    CHECK(Check(1699U, 850U, 850U, 850U) == SUPPLY_STEP_OK);                              /* 100 kHz */
    CHECK(Check(2265U, Half(2265U), Half(2265U), Half(2265U)) == SUPPLY_STEP_OK);         /* 75.02 kHz */
    CHECK(Check(2266U, Half(2266U), Half(2266U), Half(2266U)) == SUPPLY_STEP_FREQ_LOW);   /* 74.99 kHz */
    CHECK(Check(5666U, 2833U, 2833U, 2833U) == SUPPLY_STEP_FREQ_LOW);                     /* an HVPS-range step */
    CHECK(Check(0U, 0U, 0U, 0U) == SUPPLY_STEP_FREQ_HIGH);                                /* per 0 = 170 MHz */
}

static void TestHvpsFrequency(void)
{
    (void)SupplyMode_Set(SUPPLY_MODE_HVPS);
    /* 30 kHz -> per 5666.67, so per 5666 (29.998 kHz) is the first valid one */
    CHECK(Check(5666U, Half(5666U), Half(5666U), Half(5666U)) == SUPPLY_STEP_OK);
    CHECK(Check(5665U, Half(5665U), Half(5665U), Half(5665U)) == SUPPLY_STEP_FREQ_HIGH);  /* 30.003 kHz */
    CHECK(Check(16999U, Half(16999U), Half(16999U), Half(16999U)) == SUPPLY_STEP_OK);     /* exactly 10 kHz */
    CHECK(Check(17000U, Half(17000U), Half(17000U), Half(17000U)) == SUPPLY_STEP_FREQ_LOW);
    CHECK(Check(1699U, 850U, 850U, 850U) == SUPPLY_STEP_FREQ_HIGH);                       /* a Limiter-range step */
    CHECK(Check(65535U, 32767U, 32767U, 32767U) == SUPPLY_STEP_FREQ_LOW);                 /* 16-bit ceiling: 2.6 kHz */
    CHECK(PerFromFreq(20000U) == 8499U);
    CHECK(Check(8499U, 4250U, 4250U, 4250U) == SUPPLY_STEP_OK);
}

static void TestLimiterDuty(void)
{
    uint16_t per = 1699U;   /* 5% = 84.95 counts, 95% = 1614.05 */

    (void)SupplyMode_Set(SUPPLY_MODE_LIMITER);
    CHECK(Check(per, 85U, 85U, 85U) == SUPPLY_STEP_OK);
    CHECK(Check(per, 84U, 850U, 850U) == SUPPLY_STEP_DUTY_LOW);      /* U just below 5% */
    CHECK(Check(per, 850U, 84U, 850U) == SUPPLY_STEP_DUTY_LOW);      /* per-phase: V */
    CHECK(Check(per, 850U, 850U, 84U) == SUPPLY_STEP_DUTY_LOW);      /* W */
    CHECK(Check(per, 1614U, 1614U, 1614U) == SUPPLY_STEP_OK);
    CHECK(Check(per, 1615U, 850U, 850U) == SUPPLY_STEP_DUTY_HIGH);
    CHECK(Check(per, 850U, 850U, 1615U) == SUPPLY_STEP_DUTY_HIGH);
    CHECK(Check(per, 2U, 2U, 2U) == SUPPLY_STEP_DUTY_LOW);           /* the hardware compare clamp is not enough */
    CHECK(Check(per, 1697U, 1697U, 1697U) == SUPPLY_STEP_DUTY_HIGH);
    CHECK(Check(per, 0U, 850U, 850U) == SUPPLY_STEP_DUTY_LOW);
    /* 5% / 95% duty at the mode's frequency ends (per-phase independent) */
    CHECK(Check(1359U, 68U, 1291U, 700U) == SUPPLY_STEP_OK);
    CHECK(Check(2265U, 114U, 2151U, 1000U) == SUPPLY_STEP_OK);
}

static void TestHvpsDuty(void)
{
    uint16_t per = 8499U;   /* 20 kHz: 40% = 3399.6, 60% = 5099.4 */

    (void)SupplyMode_Set(SUPPLY_MODE_HVPS);
    CHECK(Check(per, 3400U, 3400U, 3400U) == SUPPLY_STEP_OK);
    CHECK(Check(per, 3399U, 3400U, 3400U) == SUPPLY_STEP_DUTY_LOW);
    CHECK(Check(per, 5099U, 5099U, 5099U) == SUPPLY_STEP_OK);
    CHECK(Check(per, 5100U, 5099U, 5099U) == SUPPLY_STEP_DUTY_HIGH);
    CHECK(Check(per, 4250U, 4250U, 5100U) == SUPPLY_STEP_DUTY_HIGH);
    CHECK(Check(per, 4250U, 850U, 4250U) == SUPPLY_STEP_DUTY_LOW);   /* a Limiter-range 10% */
}

static void TestFrequencyCheckedBeforeDuty(void)
{
    (void)SupplyMode_Set(SUPPLY_MODE_LIMITER);
    /* both wrong: the frequency error is reported */
    CHECK(Check(8499U, 4250U, 4250U, 4250U) == SUPPLY_STEP_FREQ_LOW);
    (void)SupplyMode_Set(SUPPLY_MODE_HVPS);
    CHECK(Check(1699U, 10U, 10U, 10U) == SUPPLY_STEP_FREQ_HIGH);
}

static void TestChannelCount(void)
{
    uint16_t cmp[5] = {4250U, 4250U, 4250U, 1U, 1U};   /* channels 4-5 out of range */

    (void)SupplyMode_Set(SUPPLY_MODE_HVPS);
    CHECK(SupplyMode_CheckStep(8499U, cmp, 3U) == SUPPLY_STEP_OK);          /* only the first 3 are checked */
    CHECK(SupplyMode_CheckStep(8499U, cmp, 5U) == SUPPLY_STEP_DUTY_LOW);
}

int main(void)
{
    TestNoModeAndNames();
    TestLimiterFrequency();
    TestHvpsFrequency();
    TestLimiterDuty();
    TestHvpsDuty();
    TestFrequencyCheckedBeforeDuty();
    TestChannelCount();
    return test_report("test_supply_mode");
}
