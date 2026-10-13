/*
 * test_closed_loop.c -- the closed-loop controller (src/app/control/closed_loop.c) on the host,
 * running against simulated plants for both supplies: tracking a time-series target, the two
 * HVPS branches, disturbance rejection, and the protection faults.
 */
#include "closed_loop.h"
#include "target_profile.h"
#include "supply_mode.h"
#include "hrtim.h"
#include "test_util.h"
#include <math.h>
#include <string.h>

#define VREF      (2.9f)
#define RATE      (10000U)
#define OFFSET_V  (1.45f)           /* mid-scale: a bipolar output on the unipolar ADC */

static float Abs(float x) { return x < 0.0f ? -x : x; }

/* ---- simulated plants: the step the controller commands decides the steady-state voltage,
        which the output approaches with a first-order lag ---- */
typedef struct
{
    int   limiter;        /* 1: V = Vscale (2D - 1)    0: HVPS resonant curve */
    float vscale;         /* limiter */
    float vpeak;          /* HVPS: voltage at resonance */
    float resonance;      /* HVPS */
    float width;          /* HVPS: the curve falls to half its peak this far (Hz) from resonance */
    float tau;            /* seconds */
    float gainError;      /* multiplies the steady-state voltage (a disturbance) */
    float v;
} plant_t;

static float SteadyState(const plant_t *p, uint16_t per, uint16_t cmp)
{
    if (p->limiter != 0)
    {
        float duty = (float)cmp / (float)per;
        return p->gainError * p->vscale * (2.0f * duty - 1.0f);
    }
    else
    {
        float f = (float)HRTIM_TIMER_CLK_HZ / ((float)per + 1.0f);
        float x = (f - p->resonance) / p->width;
        return p->gainError * p->vpeak / (1.0f + x * x);
    }
}

static void PlantStep(plant_t *p, uint16_t per, uint16_t cmp)
{
    float target = SteadyState(p, per, cmp);
    p->v += (target - p->v) * (1.0f / (float)RATE) / p->tau;
}

/* what the ADC would read for a process voltage */
static float CodeFor(float volts, float scale, float offsetV)
{
    float adcV = volts / scale + offsetV;
    if (adcV < 0.0f) { adcV = 0.0f; }
    if (adcV > VREF) { adcV = VREF; }
    return adcV / VREF * 4095.0f;
}

static void Profile(const uint32_t *t, const int32_t *mv, int n)
{
    Target_Clear();
    for (int i = 0; i < n; i++) { CHECK(Target_AppendUniform(t[i], mv[i], 4000) == TARGET_APPEND_OK); }
}

static void TestLimiterTracksTriangle(void)
{
    closed_loop_t cl;
    plant_t pl = { 1, 1.0f, 0.0f, 0.0f, 0.0f, 0.002f, 1.0f, 0.0f };
    uint16_t per, cmp;
    float worst = 0.0f, sumSq = 0.0f; int n = 0;
    cl_status_t st;
    /* 50 ms: -0.8 V -> +0.8 V -> -0.8 V -> +0.8 V -> -0.8 V, 12.5 ms per ramp */
    const uint32_t t[5]  = { 0U, 12500U, 25000U, 37500U, 50000U };
    const int32_t  mv[5] = { -800, 800, -800, 800, -800 };

    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    Profile(t, mv, 5);
    Cl_Init(&cl, RATE, VREF);
    Feedback_SetCalibration(&cl.fb, 1.0f, OFFSET_V);
    cl.cfg.kp[CL_PLANT_LIMITER] = 150.0f;     /* % per V: D = 0.5 %/V... a loop gain of ~3 on V = 2 (2D - 1) */
    cl.cfg.ki[CL_PLANT_LIMITER] = 6000.0f;
    cl.cfg.limiterFreqHz = 100000.0f;
    cl.cfg.limiterVscale = 1.0f;
    cl.cfg.clipTicks = 50U;
    CHECK(Cl_Start(&cl, 1U, 0.0f, &per, &cmp) == 1U);
    CHECK(cl.plant == CL_PLANT_LIMITER && cl.polarity == 1.0f);
    pl.v = -0.8f;                              /* the plant starts at the first target */
    for (int i = 0; i < 6000; i++)
    {
        PlantStep(&pl, per, cmp);
        st = Cl_Tick(&cl, CodeFor(pl.v, 1.0f, OFFSET_V), &per, &cmp);
        if (st != CL_RUNNING) { break; }
        {
            uint16_t c3[3] = { cmp, cmp, cmp };
            CHECK(SupplyMode_CheckStep(per, c3, 3U) == SUPPLY_STEP_OK);
        }
        if (i > 200)                           /* after the start-up transient */
        {
            float e = Abs(pl.v - cl.sp);
            if (e > worst) { worst = e; }
            sumSq += e * e; n++;
        }
    }
    CHECK(st == CL_FINISHED);                  /* ended by itself at 50 ms */
    CHECK(cl.tick == 500U);
    printf("  limiter triangle: worst error %.4f V, rms %.4f V\n", worst, sqrtf(sumSq / (float)n));
    CHECK(worst < 0.06f);                      /* within 60 mV of a +-0.8 V triangle */
    CHECK(sqrtf(sumSq / (float)n) < 0.045f);                   /* a 128 V/s ramp against a 2 ms lag */
}

/* the real shape of the Limiter: a +-900 V output, the divider mapping -900 V -> 0 V and +900 V -> full scale,
   calibrated with Feedback_SetBipolarRange (CONFig:FEEDback:RANGE), a +-720 V triangle (the same 89 % of what a 95 % duty
   reaches as the 1 V version's +-0.8 V), gains scaled to volts */
static void TestLimiter900V(void)
{
    closed_loop_t cl;
    plant_t pl = { 1, 900.0f, 0.0f, 0.0f, 0.0f, 0.002f, 1.0f, 0.0f };
    uint16_t per, cmp;
    float worst = 0.0f, sumSq = 0.0f; int n = 0;
    cl_status_t st;
    const uint32_t t[5]  = { 0U, 12500U, 25000U, 37500U, 50000U };
    const int32_t  mv[5] = { -720000, 720000, -720000, 720000, -720000 };

    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    Target_Clear();
    for (int i = 0; i < 5; i++) { CHECK(Target_AppendUniform(t[i], mv[i], 1000000) == TARGET_APPEND_OK); }
    Cl_Init(&cl, RATE, VREF);
    Feedback_SetBipolarRange(&cl.fb, 900.0f);
    cl.cfg.maxVolts = 1000.0f;
    cl.cfg.kp[CL_PLANT_LIMITER] = 150.0f / 900.0f;
    cl.cfg.ki[CL_PLANT_LIMITER] = 6000.0f / 900.0f;
    cl.cfg.limiterFreqHz = 100000.0f;
    cl.cfg.limiterVscale = 900.0f;
    cl.cfg.clipTicks = 50U;
    CHECK(Cl_Start(&cl, 1U, 0.0f, &per, &cmp) == 1U);
    pl.v = -720.0f;
    for (int i = 0; i < 6000; i++)
    {
        PlantStep(&pl, per, cmp);
        st = Cl_Tick(&cl, CodeFor(pl.v, cl.fb.scale, cl.fb.offsetV), &per, &cmp);
        if (st != CL_RUNNING) { break; }
        if (i > 200)
        {
            float e = Abs(pl.v - cl.sp);
            if (e > worst) { worst = e; }
            sumSq += e * e; n++;
        }
    }
    CHECK(st == CL_FINISHED);                  /* no overvoltage and no clipping at +-720 of +-900 */
    printf("  limiter +-900 V range: worst error %.2f V, rms %.2f V\n", worst, sqrtf(sumSq / (float)n));
    CHECK(worst < 54.0f);                      /* the same 6 % as the 1 V version */
    CHECK(sqrtf(sumSq / (float)n) < 40.5f);
}

static void RunHvpsRamp(uint8_t above, float *worst, float *finalErr, int *monotonicOk)
{
    closed_loop_t cl;
    plant_t pl = { 0, 0.0f, 2.0f, 19000.0f, 3000.0f, 0.003f, 1.0f, 0.0f };
    uint16_t per, cmp;
    cl_status_t st = CL_RUNNING;
    float prevF = above ? 1e9f : 0.0f;
    const uint32_t t[3]  = { 0U, 20000U, 100000U };
    const int32_t  mv[3] = { 200, 1600, 1600 };            /* 0.2 V -> 1.6 V in 20 ms, then hold 80 ms */

    SupplyMode_Set(SUPPLY_MODE_HVPS);
    Profile(t, mv, 3);
    Cl_Init(&cl, RATE, VREF);
    Feedback_SetCalibration(&cl.fb, 1.0f, 0.0f);
    cl.cfg.kp[CL_PLANT_HVPS] = 4000.0f;        /* Hz per V */
    cl.cfg.ki[CL_PLANT_HVPS] = 200000.0f;
    cl.cfg.hvpsAbove = above;
    cl.cfg.hvpsResonanceHz = 19000.0f;
    cl.cfg.hvpsVpeak = 2.0f;
    cl.cfg.hvpsDutyPct = 50.0f;
    CHECK(Cl_Start(&cl, 1U, 0.0f, &per, &cmp) == 1U);
    CHECK(cl.polarity == (above ? -1.0f : 1.0f));
    *worst = 0.0f; *monotonicOk = 1;
    pl.v = 0.2f;
    for (int i = 0; i < 1100; i++)
    {
        float f;
        PlantStep(&pl, per, cmp);
        st = Cl_Tick(&cl, CodeFor(pl.v, 1.0f, 0.0f), &per, &cmp);
        if (st != CL_RUNNING) { break; }
        f = (float)HRTIM_TIMER_CLK_HZ / ((float)per + 1.0f);
        if (i < 190)                                        /* during the ramp the frequency moves one way */
        {
            if (above  && f > prevF + 30.0f) { *monotonicOk = 0; }
            if (!above && f < prevF - 30.0f) { *monotonicOk = 0; }
            prevF = f;
        }
        if (i > 300 && Abs(pl.v - cl.sp) > *worst) { *worst = Abs(pl.v - cl.sp); }      /* after the ramp and its settling */
    }
    *finalErr = Abs(pl.v - 1.6f);
    printf("  hvps %s: worst %.4f V, final error %.4f V (ended %d)\n", above ? "above" : "below", *worst, *finalErr, (int)st);
    CHECK(st == CL_FINISHED);
}

static void TestHvpsBothBranches(void)
{
    float worst, fin; int mono;

    RunHvpsRamp(1U, &worst, &fin, &mono);                   /* above resonance */
    CHECK(worst < 0.10f);
    CHECK(fin < 0.02f);
    CHECK(mono);                                            /* frequency FALLS as the voltage rises */

    RunHvpsRamp(0U, &worst, &fin, &mono);                   /* below resonance */
    CHECK(worst < 0.10f);
    CHECK(fin < 0.02f);
    CHECK(mono);                                            /* frequency RISES as the voltage rises */
}

static void TestDisturbanceRejection(void)
{
    closed_loop_t cl;
    plant_t pl = { 1, 1.0f, 0.0f, 0.0f, 0.0f, 0.002f, 1.0f, 0.0f };
    uint16_t per, cmp;
    const uint32_t t[2]  = { 0U, 100000U };
    const int32_t  mv[2] = { 500, 500 };                    /* hold 0.5 V for 100 ms */
    float errAfter = 0.0f;

    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    Profile(t, mv, 2);
    Cl_Init(&cl, RATE, VREF);
    Feedback_SetCalibration(&cl.fb, 1.0f, OFFSET_V);
    cl.cfg.kp[CL_PLANT_LIMITER] = 150.0f;
    cl.cfg.ki[CL_PLANT_LIMITER] = 6000.0f;
    CHECK(Cl_Start(&cl, 1U, 0.0f, &per, &cmp) == 1U);
    pl.v = 0.5f;
    for (int i = 0; i < 900; i++)
    {
        if (i == 300) { pl.gainError = 0.7f; }              /* the plant loses 30 % of its gain mid-shot */
        PlantStep(&pl, per, cmp);
        if (Cl_Tick(&cl, CodeFor(pl.v, 1.0f, OFFSET_V), &per, &cmp) != CL_RUNNING) { break; }
        if (i > 800) { errAfter = Abs(pl.v - 0.5f); }
    }
    printf("  disturbance: error after recovery %.5f V\n", errAfter);
    CHECK(errAfter < 0.01f);                                /* the integrator absorbed it */
}

static void TestHoldMode(void)
{
    closed_loop_t cl;
    plant_t pl = { 1, 1.0f, 0.0f, 0.0f, 0.0f, 0.002f, 1.0f, 0.0f };
    uint16_t per, cmp;

    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    Target_Clear();                                         /* no profile needed to hold */
    Cl_Init(&cl, RATE, VREF);
    Feedback_SetCalibration(&cl.fb, 1.0f, OFFSET_V);
    cl.cfg.kp[CL_PLANT_LIMITER] = 150.0f;
    cl.cfg.ki[CL_PLANT_LIMITER] = 6000.0f;
    CHECK(Cl_Start(&cl, 0U, 0.3f, &per, &cmp) == 1U);
    for (int i = 0; i < 2000; i++)
    {
        PlantStep(&pl, per, cmp);
        CHECK(Cl_Tick(&cl, CodeFor(pl.v, 1.0f, OFFSET_V), &per, &cmp) == CL_RUNNING);   /* a hold never finishes */
    }
    CHECK(Abs(pl.v - 0.3f) < 0.005f);
    Cl_SetHoldVolts(&cl, -0.4f);                            /* the setpoint can move while holding */
    for (int i = 0; i < 2000; i++)
    {
        PlantStep(&pl, per, cmp);
        Cl_Tick(&cl, CodeFor(pl.v, 1.0f, OFFSET_V), &per, &cmp);
    }
    CHECK(Abs(pl.v + 0.4f) < 0.005f);
}

static void TestProtection(void)
{
    closed_loop_t cl;
    uint16_t per, cmp;
    cl_status_t st;

    SupplyMode_Set(SUPPLY_MODE_LIMITER);
    Target_Clear();
    Cl_Init(&cl, RATE, VREF);
    Feedback_SetCalibration(&cl.fb, 2.0f, 0.0f);            /* ADC volts x 2: up to 5.8 V */
    cl.cfg.maxVolts = 4.0f;
    cl.cfg.clipTicks = 5U;
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);

    /* just under the limit: fine; just over: an overvoltage fault at once, and nothing is written */
    CHECK(Cl_Tick(&cl, CodeFor(3.9f, 2.0f, 0.0f), &per, &cmp) == CL_RUNNING);
    {
        uint16_t per0 = 12345U, cmp0 = 6789U;
        st = Cl_Tick(&cl, CodeFor(4.2f, 2.0f, 0.0f), &per0, &cmp0);
        CHECK(st == CL_FAULT_OVERVOLTAGE);
        CHECK(per0 == 12345U && cmp0 == 6789U);
    }
    CHECK(cl.active == 0U);

    /* negative overvoltage counts too (bipolar output) */
    Feedback_SetCalibration(&cl.fb, 4.0f, 1.45f);
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);
    CHECK(Cl_Tick(&cl, 0.0f, &per, &cmp) == CL_FAULT_OVERVOLTAGE);   /* (0 - 1.45) * 4 = -5.8 V */

    /* a slow filter must not hide an overvoltage: the check uses the unfiltered value */
    Feedback_SetCalibration(&cl.fb, 2.0f, 0.0f);
    Feedback_SetFilter(&cl.fb, 1.0f, cl.dt);                /* a 1 Hz low-pass */
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);
    Cl_Tick(&cl, CodeFor(0.5f, 2.0f, 0.0f), &per, &cmp);
    CHECK(Cl_Tick(&cl, CodeFor(5.0f, 2.0f, 0.0f), &per, &cmp) == CL_FAULT_OVERVOLTAGE);
    Feedback_SetFilter(&cl.fb, 0.0f, cl.dt);

    /* clipping: the ADC at full scale for 5 ticks. A limit above the ADC range so only clipping trips. */
    cl.cfg.maxVolts = 100.0f;
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);
    for (int i = 0; i < 4; i++) { CHECK(Cl_Tick(&cl, 4095.0f, &per, &cmp) == CL_RUNNING); }
    CHECK(Cl_Tick(&cl, 4095.0f, &per, &cmp) == CL_FAULT_CLIPPED);
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);
    for (int i = 0; i < 4; i++) { Cl_Tick(&cl, 4095.0f, &per, &cmp); }
    CHECK(Cl_Tick(&cl, 2000.0f, &per, &cmp) == CL_RUNNING);          /* a good reading resets the count */
    for (int i = 0; i < 4; i++) { CHECK(Cl_Tick(&cl, 4095.0f, &per, &cmp) == CL_RUNNING); }
    cl.cfg.clipTicks = 0U;                                           /* switched off */
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);
    for (int i = 0; i < 50; i++) { CHECK(Cl_Tick(&cl, 4095.0f, &per, &cmp) == CL_RUNNING); }

    /* the bottom of the range: a dead feedback line reads 0, which on a mid-scale-zero (bipolar) range would read as
       -Vmax and send the output up. Counted only with a non-zero offset, with the same 5-tick limit. */
    Feedback_SetBipolarRange(&cl.fb, 50.0f);
    cl.cfg.maxVolts = 100.0f;
    cl.cfg.clipTicks = 5U;
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);
    for (int i = 0; i < 4; i++) { CHECK(Cl_Tick(&cl, 0.0f, &per, &cmp) == CL_RUNNING); }
    CHECK(Cl_Tick(&cl, 0.0f, &per, &cmp) == CL_FAULT_CLIPPED_LOW);
    CHECK(cl.active == 0U);
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);
    for (int i = 0; i < 4; i++) { Cl_Tick(&cl, 0.0f, &per, &cmp); }
    CHECK(Cl_Tick(&cl, 2000.0f, &per, &cmp) == CL_RUNNING);          /* a good reading resets the count */
    Feedback_SetCalibration(&cl.fb, 1.0f, 0.0f);                     /* offset 0 (HVPS): 0 V is a normal reading */
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);
    for (int i = 0; i < 50; i++) { CHECK(Cl_Tick(&cl, 0.0f, &per, &cmp) == CL_RUNNING); }
}

static void TestStartRefusals(void)
{
    closed_loop_t cl;
    uint16_t per, cmp;

    Cl_Init(&cl, RATE, VREF);
    SupplyMode_Set(SUPPLY_MODE_NONE);
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 0U);                /* no supply mode */

    SupplyMode_Set(SUPPLY_MODE_HVPS);
    Target_Clear();
    CHECK(Cl_Start(&cl, 1U, 0.0f, &per, &cmp) == 0U);                /* a profile run with no profile */
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);                /* but a hold run needs none */

    cl.cfg.hvpsAbove = 1U;
    cl.cfg.hvpsResonanceHz = 35000.0f;                               /* above the whole HVPS range: no band */
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 0U);
    cl.cfg.hvpsAbove = 0U;
    CHECK(Cl_Start(&cl, 0U, 0.0f, &per, &cmp) == 1U);                /* below it, 10-30 kHz is all usable */

    /* the opening step of a hold run is a legal step for the mode */
    SupplyMode_Set(SUPPLY_MODE_HVPS);
    cl.cfg.hvpsResonanceHz = 19000.0f;
    cl.cfg.hvpsAbove = 1U;
    cl.cfg.hvpsVpeak = 2.0f;
    CHECK(Cl_Start(&cl, 0U, 1.0f, &per, &cmp) == 1U);
    {
        uint16_t c3[3] = { cmp, cmp, cmp };
        CHECK(SupplyMode_CheckStep(per, c3, 3U) == SUPPLY_STEP_OK);
        /* 1.0 V of a 2.0 V peak is halfway: 30 kHz - (30 - 19) / 2 = 24.5 kHz */
        CHECK(Abs((float)HRTIM_TIMER_CLK_HZ / ((float)per + 1.0f) - 24500.0f) < 20.0f);
        CHECK(Abs((float)cmp / (float)per - 0.5f) < 0.001f);         /* the fixed 50 % duty */
    }

    /* HVPS output is power: a negative target (or hold) has no meaning, the Limiter's bipolar one does */
    {
        const uint32_t tn[2] = { 0U, 10000U };
        const int32_t  vn[2] = { 500, -200 };

        Profile(tn, vn, 2);
        CHECK(Cl_Start(&cl, 1U, 0.0f, &per, &cmp) == 0U);
        CHECK(Cl_Start(&cl, 0U, -0.1f, &per, &cmp) == 0U);
        SupplyMode_Set(SUPPLY_MODE_LIMITER);
        CHECK(Cl_Start(&cl, 1U, 0.0f, &per, &cmp) == 1U);
        SupplyMode_Set(SUPPLY_MODE_HVPS);
    }

    /* a lookup table loaded by the operator replaces the linear default, and its peak defines the band */
    {
        hvps_curve_t *t = &cl.cfg.curve;

        HvpsCurve_Clear(t);
        for (int f = 10000; f <= 30000; f += 1000)
        {
            float x = ((float)f - 21000.0f) / 3000.0f;
            (void)HvpsCurve_Append(t, (float)f, 1.5f / (1.0f + x * x));       /* peaks at 21 kHz, not the 19 kHz setting */
        }
        cl.cfg.curveCustom = 1U;
        cl.cfg.hvpsResonanceHz = 19000.0f;                                    /* ignored: the table says 21 kHz */
        cl.cfg.hvpsAbove = 1U;
        CHECK(Cl_Start(&cl, 0U, 0.75f, &per, &cmp) == 1U);                    /* half the peak power */
        CHECK(Abs(cl.uMin - 21000.0f) < 1e-3f);
        CHECK(Abs((float)HRTIM_TIMER_CLK_HZ / ((float)per + 1.0f) - HvpsCurve_FreqFor(t, 0.75f, 1U)) < 20.0f);
        CHECK(Abs(HvpsCurve_Eval(t, (float)HRTIM_TIMER_CLK_HZ / ((float)per + 1.0f)) - 0.75f) < 0.01f);   /* the step gives the power asked for */

        HvpsCurve_Clear(t);                                                   /* an invalid table is refused, not used */
        (void)HvpsCurve_Append(t, 10000.0f, 1.0f);
        CHECK(Cl_Start(&cl, 0U, 0.5f, &per, &cmp) == 0U);
        cl.cfg.curveCustom = 0U;
        CHECK(Cl_Start(&cl, 0U, 0.5f, &per, &cmp) == 1U);                     /* the default is back */
    }
}

int main(void)
{
    TestLimiterTracksTriangle();
    TestLimiter900V();
    TestHvpsBothBranches();
    TestDisturbanceRejection();
    TestHoldMode();
    TestProtection();
    TestStartRefusals();
    return test_report("test_closed_loop");
}
