/*
 * test_charge.c -- the CHARGE output's policy (src/app/control/charge.c) on the host, with the timer faked.
 *
 * Checked: the timer settings for every frequency from 1 Hz to the 100 kHz ceiling at the board's 170 MHz clock (the
 * produced frequency within one counter step, a 16-bit period, an exact-enough 50 % high count), the refused values,
 * that 0 stops the output and a new frequency re-times it, that off is the state at start-up, and that the setpoint
 * and the status follow what was programmed.
 */
#include "charge.h"
#include "charge_out.h"
#include "ctrlr_config.h"
#include "test_util.h"
#include <string.h>

/* ---- the fake timer ---------------------------------------------------- */
static uint32_t f_clock = 170000000UL;
static int f_inits, f_starts, f_stops;
static uint32_t f_psc, f_arr, f_ccr;

void ChargeOut_Init(void)           { f_inits++; }
uint32_t ChargeOut_ClockHz(void)    { return f_clock; }
void ChargeOut_Start(uint32_t psc, uint32_t arr, uint32_t ccr) { f_starts++; f_psc = psc; f_arr = arr; f_ccr = ccr; }
void ChargeOut_Stop(void)           { f_stops++; }

static double produced_hz(uint32_t clock, uint32_t psc, uint32_t arr)
{
    return (double)clock / (((double)psc + 1.0) * ((double)arr + 1.0));
}

static void test_plan_everywhere(void)
{
    uint32_t hz, psc, arr, ccr;
    double worst = 0.0;
    uint32_t sweep[] = { 1, 2, 5, 10, 50, 100, 400, 1000, 2594, 2595, 2600, 5000, 10000, 33333, 50000, 75000, 99999, 100000 };
    size_t i;

    for (i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++)
    {
        double got, err;
        hz = sweep[i];
        CHECK(Charge_Plan(f_clock, hz, &psc, &arr, &ccr) == 1U);
        CHECK(psc <= 65535UL);
        CHECK(arr <= 65535UL);
        CHECK(arr >= 1UL);
        got = produced_hz(f_clock, psc, arr);
        err = (got - (double)hz) / (double)hz;
        if (err < 0.0) { err = -err; }
        if (err > worst) { worst = err; }
        CHECK(err < 0.0005);                                  /* within 0.05 % */
        CHECK(ccr == (arr + 1UL) / 2UL);                       /* 50 %, to a half count */
        CHECK(ccr >= 1UL);
    }
    CHECK(worst < 0.0005);
    /* 100 kHz is exact at 170 MHz: 1700 counts, 850 high */
    CHECK(Charge_Plan(f_clock, 100000UL, &psc, &arr, &ccr) == 1U);
    CHECK(psc == 0UL && arr == 1699UL && ccr == 850UL);
}

static void test_plan_refuses(void)
{
    uint32_t psc = 77, arr = 77, ccr = 77;

    CHECK(Charge_Plan(f_clock, 0UL, &psc, &arr, &ccr) == 0U);
    CHECK(Charge_Plan(f_clock, 100001UL, &psc, &arr, &ccr) == 0U);
    CHECK(Charge_Plan(f_clock, 4000000000UL, &psc, &arr, &ccr) == 0U);
    CHECK(Charge_Plan(150UL, 100UL, &psc, &arr, &ccr) == 0U);       /* a clock too slow to make two counts per period */
    CHECK(psc == 77UL && arr == 77UL && ccr == 77UL);               /* nothing written on a refusal */
}

static void test_set_and_off(void)
{
    charge_status_t st;

    memset(&st, 0xFF, sizeof(st));
    f_inits = f_starts = f_stops = 0;
    Charge_Init();
    CHECK(f_inits == 1);
    CHECK(Charge_GetFrequency() == 0UL);
    Charge_GetStatus(&st);
    CHECK(st.running == 0U && st.set_hz == 0UL && st.actual_mhz == 0UL && st.psc == 0UL && st.arr == 0UL && st.ccr == 0UL);
    CHECK(f_starts == 0);

    CHECK(Charge_SetFrequency(100000UL) == 1U);
    CHECK(f_starts == 1 && f_psc == 0UL && f_arr == 1699UL && f_ccr == 850UL);
    CHECK(Charge_GetFrequency() == 100000UL);
    Charge_GetStatus(&st);
    CHECK(st.running == 1U && st.set_hz == 100000UL && st.actual_mhz == 100000000UL);
    CHECK(st.psc == 0UL && st.arr == 1699UL && st.ccr == 850UL);

    /* a new frequency re-times, it does not stop */
    CHECK(Charge_SetFrequency(1000UL) == 1U);
    CHECK(f_starts == 2 && f_stops == 0);
    CHECK(f_psc == 2UL && f_arr == 56666UL && f_ccr == 28333UL);   /* 170000 counts do not fit 16 bits: divide by 3 */
    CHECK(Charge_GetFrequency() == 1000UL);

    /* a slow one needs the prescaler */
    CHECK(Charge_SetFrequency(1UL) == 1U);
    CHECK(f_psc > 0UL);
    Charge_GetStatus(&st);
    CHECK(st.actual_mhz == 1000UL);                                 /* 1.000 Hz (999.998 mHz, rounded) */

    /* out of range: refused, the running setting is untouched */
    CHECK(Charge_SetFrequency(100001UL) == 0U);
    CHECK(Charge_GetFrequency() == 1UL && f_stops == 0);

    CHECK(Charge_SetFrequency(0UL) == 1U);
    CHECK(f_stops == 1);
    CHECK(Charge_GetFrequency() == 0UL);
    Charge_GetStatus(&st);
    CHECK(st.running == 0U && st.actual_mhz == 0UL);

    /* off again is harmless and still stops the pin */
    CHECK(Charge_SetFrequency(0UL) == 1U);
    CHECK(f_stops == 2);
}

static void test_other_clock(void)
{
    uint32_t psc, arr, ccr;
    /* the clock is the driver's to report: with 85 MHz (an undivided APB2 at half speed) 100 kHz is 850 counts */
    CHECK(Charge_Plan(85000000UL, 100000UL, &psc, &arr, &ccr) == 1U);
    CHECK(psc == 0UL && arr == 849UL && ccr == 425UL);
}

int main(void)
{
    test_plan_everywhere();
    test_plan_refuses();
    test_set_and_off();
    test_other_clock();
    return test_report("test_charge");
}
