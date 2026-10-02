/*
 * test_pid_core.c -- the PID arithmetic (src/app/control/pid_core.c) on the host,
 * against simulated plants: convergence, no steady-state error, anti-windup, the
 * output clamp and slew limit, derivative on the measurement, reset.
 */
#include "pid_core.h"
#include "test_util.h"
#include <math.h>
#include <string.h>

/* y' = (K*u - y) / tau, stepped with the controller's dt */
typedef struct { float y; float K; float tau; float dt; } plant_t;
static float PlantStep(plant_t *pl, float u)
{
    pl->y += pl->dt * (pl->K * u - pl->y) / pl->tau;
    return pl->y;
}

static float Abs(float x) { return x < 0.0f ? -x : x; }

static void TestConverges(void)
{
    pid_core_t pid;
    plant_t pl = { 0.0f, 2.0f, 0.01f, 0.0001f };   /* gain 2, 10 ms lag, 10 kHz */
    float u = 0.0f;

    Pid_Init(&pid, pl.dt, 0.0f, 100.0f);
    pid.kp = 2.0f; pid.ki = 400.0f;
    for (int i = 0; i < 20000; i++)                /* 2 s */
    {
        u = Pid_Update(&pid, 1.5f, pl.y, 0.0f);
        PlantStep(&pl, u);
    }
    CHECK(Abs(pl.y - 1.5f) < 1e-3f);               /* integral action: no steady-state error */
    CHECK(Abs(u - 0.75f) < 1e-2f);                 /* the output settled at setpoint / K */
}

static void TestFeedforwardCarriesTheLoad(void)
{
    pid_core_t pid;
    plant_t pl = { 0.0f, 2.0f, 0.01f, 0.0001f };
    float u = 0.0f;

    Pid_Init(&pid, pl.dt, 0.0f, 100.0f);
    pid.kp = 2.0f; pid.ki = 400.0f;
    /* a perfect feedforward (setpoint / K): the correction stays ~0 from the start */
    for (int i = 0; i < 5000; i++)
    {
        u = Pid_Update(&pid, 1.5f, pl.y, 0.75f);
        PlantStep(&pl, u);
    }
    CHECK(Abs(pl.y - 1.5f) < 5e-3f);
    CHECK(Abs(pid.integ) < 0.05f);
}

static void TestAntiWindup(void)
{
    pid_core_t pid;
    plant_t pl = { 0.0f, 2.0f, 0.01f, 0.0001f };
    float u = 0.0f;
    float peak = 0.0f;

    Pid_Init(&pid, pl.dt, 0.0f, 1.0f);             /* the actuator can only reach y = 2 */
    pid.kp = 2.0f; pid.ki = 400.0f;
    for (int i = 0; i < 20000; i++)                /* 2 s demanding the impossible: y = 5 */
    {
        u = Pid_Update(&pid, 5.0f, pl.y, 0.0f);
        PlantStep(&pl, u);
    }
    CHECK(u == 1.0f);                              /* pinned at the limit */
    CHECK(Abs(pid.integ) < 10.0f);                 /* and the integrator did NOT wind up: it sits at the output
                                                      minus the P term (~ -5); an unprotected one would be ~ 2400 */
    /* now ask for something reachable: y = 1 must be reached promptly, with little overshoot */
    for (int i = 0; i < 6000; i++)                 /* 0.6 s */
    {
        u = Pid_Update(&pid, 1.0f, pl.y, 0.0f);
        PlantStep(&pl, u);
        if (pl.y > peak) { peak = pl.y; }
    }
    CHECK(Abs(pl.y - 1.0f) < 0.02f);
    CHECK(peak < 2.05f);                           /* it was at 2.0 when the demand dropped; no further overshoot */
}

static void TestClampAndSlew(void)
{
    pid_core_t pid;
    float u;

    Pid_Init(&pid, 0.001f, -1.0f, 1.0f);
    pid.kp = 100.0f;
    CHECK(Pid_Update(&pid, 10.0f, 0.0f, 0.0f) == 1.0f);     /* clamped high */
    CHECK(Pid_Update(&pid, -10.0f, 0.0f, 0.0f) == -1.0f);   /* and low */

    Pid_Init(&pid, 0.001f, 0.0f, 100.0f);
    pid.kp = 1000.0f; pid.outSlew = 2.0f;
    Pid_Reset(&pid, 10.0f);
    u = Pid_Update(&pid, 50.0f, 0.0f, 0.0f);
    CHECK(Abs(u - 12.0f) < 1e-4f);                          /* at most 2 per tick from 10 */
    u = Pid_Update(&pid, 50.0f, 0.0f, 0.0f);
    CHECK(Abs(u - 14.0f) < 1e-4f);
    u = Pid_Update(&pid, -50.0f, 0.0f, 0.0f);
    CHECK(Abs(u - 12.0f) < 1e-4f);                          /* downward too */
    /* the slew limit also stops the integrator running away behind it */
    Pid_Init(&pid, 0.001f, 0.0f, 100.0f);
    pid.ki = 1000.0f; pid.outSlew = 0.1f;
    Pid_Reset(&pid, 0.0f);
    {
        float prev = 0.0f;
        float worst = 0.0f;
        float peakInteg = 0.0f;
        for (int i = 0; i < 1000; i++)
        {
            u = Pid_Update(&pid, 10.0f, 0.0f, 0.0f);
            if (Abs(u - prev) > worst) { worst = Abs(u - prev); }
            if (pid.integ > peakInteg) { peakInteg = pid.integ; }
            prev = u;
        }
        CHECK(worst <= 0.1f + 1e-4f);                           /* never more than the slew per tick */
        CHECK(u > 5.0f);                                        /* and it does ramp */
        CHECK(peakInteg < 150.0f);                              /* it tracks the output (<= 100 here); unprotected it would reach 10 * 1000 = 10000 */
    }
}

static void TestDerivativeOnMeasurement(void)
{
    pid_core_t pid;
    float u;

    Pid_Init(&pid, 0.001f, -1000.0f, 1000.0f);
    pid.kd = 0.01f; pid.dFilter = 1.0f;
    /* a setpoint step with a still measurement: no derivative kick */
    Pid_Update(&pid, 0.0f, 1.0f, 0.0f);
    u = Pid_Update(&pid, 100.0f, 1.0f, 0.0f);
    CHECK(Abs(u) < 1e-6f);
    /* a measurement rising by 0.5 in one tick pushes the output DOWN by kd*0.5/dt = 5 */
    u = Pid_Update(&pid, 100.0f, 1.5f, 0.0f);
    CHECK(Abs(u + 5.0f) < 1e-3f);
}

static void TestReset(void)
{
    pid_core_t pid;

    Pid_Init(&pid, 0.001f, 0.0f, 10.0f);
    pid.ki = 1000.0f;
    for (int i = 0; i < 50; i++) { Pid_Update(&pid, 1.0f, 0.0f, 0.0f); }
    CHECK(pid.integ > 0.0f);
    Pid_Reset(&pid, 3.0f);
    CHECK(pid.integ == 0.0f && pid.lastOut == 3.0f && pid.havePrev == 0U);
    CHECK(pid.ki == 1000.0f);                               /* gains are kept */
}

static void TestNegativeGainPlant(void)
{
    /* a plant whose output FALLS as the actuator rises (HVPS above resonance). closed_loop.c
       handles it by polarity: it hands the controller the negated setpoint and measurement. */
    pid_core_t pid;
    plant_t pl = { 0.0f, -2.0f, 0.01f, 0.0001f };   /* y = -2 u */
    float u = 0.0f;

    Pid_Init(&pid, pl.dt, 0.0f, 100.0f);
    pid.kp = 2.0f; pid.ki = 400.0f;
    for (int i = 0; i < 20000; i++)
    {
        u = Pid_Update(&pid, -(-1.5f), -pl.y, 0.0f);   /* setpoint -1.5 V, polarity flipped */
        PlantStep(&pl, u);
    }
    CHECK(Abs(pl.y + 1.5f) < 1e-3f);
    CHECK(Abs(u - 0.75f) < 1e-2f);
}

int main(void)
{
    TestConverges();
    TestFeedforwardCarriesTheLoad();
    TestAntiWindup();
    TestClampAndSlew();
    TestDerivativeOnMeasurement();
    TestReset();
    TestNegativeGainPlant();
    return test_report("test_pid_core");
}
