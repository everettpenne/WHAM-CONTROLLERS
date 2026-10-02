/*
 * closed_loop.c -- see closed_loop.h.
 */
#include "closed_loop.h"
#include "supply_mode.h"
#include "shot_profile.h"
#include "target_profile.h"

static float Clamp(float x, float lo, float hi)
{
    if (x < lo) { return lo; }
    if (x > hi) { return hi; }
    return x;
}

static float Abs(float x)
{
    return (x < 0.0f) ? -x : x;
}

void Cl_Init(closed_loop_t *cl, uint32_t rateHz, float vrefV)
{
    cl_config_t *c = &cl->cfg;

    for (int i = 0; i < CL_PLANT_COUNT; i++)
    {
        c->kp[i] = 0.0f;
        c->ki[i] = 0.0f;
        c->kd[i] = 0.0f;
    }
    c->dFilter = 0.2f;
    c->slewPerTick = 0.0f;
    c->maxVolts = 4.0f;
    c->clipTicks = 0U;
    c->hvpsDutyPct = 50.0f;
    c->hvpsResonanceHz = 19000.0f;
    c->hvpsAbove = 1U;
    c->hvpsVpeak = 1.0f;
    c->limiterFreqHz = 100000.0f;
    c->limiterVscale = 1.0f;

    Feedback_Init(&cl->fb, vrefV);
    cl->active = 0U;
    cl->plant = CL_PLANT_HVPS;
    cl->useProfile = 0U;
    cl->holdVolts = 0.0f;
    cl->tick = 0U;
    cl->sp = cl->pv = cl->out = 0.0f;
    cl->polarity = 1.0f;
    cl->uMin = cl->uMax = 0.0f;
    cl->rateHz = 0U;
    cl->dt = 0.0f;
    Cl_SetRate(cl, rateHz);
}

void Cl_SetRate(closed_loop_t *cl, uint32_t rateHz)
{
    if (rateHz == 0U)
    {
        rateHz = 1U;
    }
    cl->rateHz = rateHz;
    cl->dt = 1.0f / (float)rateHz;
}

/* The actuator value for a target voltage, from the plant model, within [uMin, uMax]. */
static float Feedforward(const closed_loop_t *cl, float volts)
{
    const cl_config_t *c = &cl->cfg;
    float u;

    if (cl->plant == CL_PLANT_LIMITER)
    {
        u = 50.0f * (1.0f + volts / c->limiterVscale);              /* V = Vscale * (2D - 1) */
    }
    else
    {
        float ratio = Clamp(volts / c->hvpsVpeak, 0.0f, 1.0f);      /* 0 = far from resonance, 1 = at it */
        u = (c->hvpsAbove != 0U) ? cl->uMax - (cl->uMax - cl->uMin) * ratio
                                 : cl->uMin + (cl->uMax - cl->uMin) * ratio;
    }
    return Clamp(u, cl->uMin, cl->uMax);
}

/* {per, cmp} for actuator value u. */
static void MakeStep(const closed_loop_t *cl, float u, uint16_t *per, uint16_t *cmp)
{
    const cl_config_t *c = &cl->cfg;
    uint32_t fMilli;
    uint32_t dHund;

    if (cl->plant == CL_PLANT_LIMITER)
    {
        fMilli = (uint32_t)(c->limiterFreqHz * 1000.0f + 0.5f);
        dHund = (uint32_t)(u * 100.0f + 0.5f);
    }
    else
    {
        fMilli = (uint32_t)(u * 1000.0f + 0.5f);
        dHund = (uint32_t)(c->hvpsDutyPct * 100.0f + 0.5f);
    }
    (void)ShotProfile_StepFor(fMilli, dHund, per, cmp);
}

static float TargetNow(const closed_loop_t *cl, uint32_t tick)
{
    if (cl->useProfile == 0U)
    {
        return cl->holdVolts;
    }
    return Target_ValueAt((uint32_t)(((uint64_t)tick * 1000000ULL) / cl->rateHz));
}

uint8_t Cl_Start(closed_loop_t *cl, uint8_t useProfile, float holdVolts, uint16_t *per, uint16_t *cmp)
{
    supply_limits_t lim;
    supply_mode_t mode = SupplyMode_Get();
    const cl_config_t *c = &cl->cfg;
    float start;

    if (SupplyMode_GetLimits(mode, &lim) == 0U)
    {
        return 0U;
    }
    if ((useProfile != 0U) && (Target_IsComplete() == 0U))
    {
        return 0U;
    }

    cl->plant = (mode == SUPPLY_MODE_LIMITER) ? CL_PLANT_LIMITER : CL_PLANT_HVPS;
    if (cl->plant == CL_PLANT_LIMITER)
    {
        cl->uMin = (float)lim.dutyMinPct;
        cl->uMax = (float)lim.dutyMaxPct;
        cl->polarity = 1.0f;
    }
    else
    {
        float res = c->hvpsResonanceHz;
        float fmin = (float)lim.freqMinHz;
        float fmax = (float)lim.freqMaxHz;

        if (c->hvpsAbove != 0U)
        {
            cl->uMin = (res > fmin) ? res : fmin;
            cl->uMax = fmax;
            cl->polarity = -1.0f;
        }
        else
        {
            cl->uMin = fmin;
            cl->uMax = (res < fmax) ? res : fmax;
            cl->polarity = 1.0f;
        }
        if (cl->uMin >= cl->uMax)
        {
            return 0U;                  /* the resonance leaves no usable band inside the mode's range */
        }
    }

    cl->useProfile = useProfile;
    cl->holdVolts = holdVolts;
    cl->tick = 0U;
    Target_Rewind();
    Feedback_Reset(&cl->fb);

    Pid_Init(&cl->pid, cl->dt, cl->uMin, cl->uMax);
    cl->pid.kp = c->kp[cl->plant];
    cl->pid.ki = c->ki[cl->plant];
    cl->pid.kd = c->kd[cl->plant];
    cl->pid.dFilter = c->dFilter;
    cl->pid.outSlew = c->slewPerTick;

    cl->sp = TargetNow(cl, 0U);
    start = Feedforward(cl, cl->sp);
    Pid_Reset(&cl->pid, start);
    cl->out = start;
    cl->pv = 0.0f;
    MakeStep(cl, start, per, cmp);
    cl->active = 1U;
    return 1U;
}

cl_status_t Cl_Tick(closed_loop_t *cl, float meanCode, uint16_t *per, uint16_t *cmp)
{
    float pv;
    float sp;
    float base;
    float u;

    if (cl->active == 0U)
    {
        return CL_FINISHED;
    }
    cl->tick++;
    pv = Feedback_Process(&cl->fb, meanCode);
    cl->pv = pv;

    /* protection first, on the UNFILTERED value: a slow filter must not hide an overvoltage */
    if (Abs(cl->fb.rawVolts) > cl->cfg.maxVolts)
    {
        cl->active = 0U;
        return CL_FAULT_OVERVOLTAGE;
    }
    if ((cl->cfg.clipTicks != 0U) && (cl->fb.clipHighTicks >= cl->cfg.clipTicks))
    {
        cl->active = 0U;
        return CL_FAULT_CLIPPED;
    }
    if ((cl->useProfile != 0U) &&
        ((((uint64_t)cl->tick * 1000000ULL) / cl->rateHz) >= Target_DurationUs()))
    {
        cl->active = 0U;
        return CL_FINISHED;
    }

    sp = TargetNow(cl, cl->tick);
    cl->sp = sp;
    base = Feedforward(cl, sp);
    /* the polarity flips the error's sign for a plant whose output falls as the actuator rises */
    u = Pid_Update(&cl->pid, sp * cl->polarity, pv * cl->polarity, base);
    cl->out = u;
    MakeStep(cl, u, per, cmp);
    return CL_RUNNING;
}

void Cl_SetHoldVolts(closed_loop_t *cl, float volts)
{
    cl->holdVolts = volts;
}

void Cl_Stop(closed_loop_t *cl)
{
    cl->active = 0U;
}
