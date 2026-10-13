/*
 * pid_core.c -- see pid_core.h.
 */
#include "pid_core.h"

/* Float logic run by the control interrupt once per tick per loop: compiled at -O2 whatever the project's level
   (see closed_loop.c). Host-tested identically. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif

void Pid_Init(pid_core_t *p, float dt, float outMin, float outMax)
{
    p->kp = 0.0f;
    p->ki = 0.0f;
    p->kd = 0.0f;
    p->dt = dt;
    p->outMin = outMin;
    p->outMax = outMax;
    p->outSlew = 0.0f;
    p->dFilter = 0.2f;
    Pid_Reset(p, outMin);
}

void Pid_Reset(pid_core_t *p, float startOut)
{
    p->integ = 0.0f;
    p->prevPv = 0.0f;
    p->dState = 0.0f;
    p->lastOut = startOut;
    p->havePrev = 0U;
}

float Pid_Update(pid_core_t *p, float setpoint, float measurement, float base)
{
    float e = setpoint - measurement;
    float pTerm = p->kp * e;
    float dTerm = 0.0f;
    float out;
    float applied;

    /* derivative on the measurement, low-passed; none on the first tick (no history) */
    if (p->havePrev != 0U)
    {
        float raw = -p->kd * (measurement - p->prevPv) / p->dt;
        p->dState += p->dFilter * (raw - p->dState);
        dTerm = p->dState;
    }
    p->prevPv = measurement;
    p->havePrev = 1U;

    {
        float step = p->ki * e * p->dt;
        float clamped;

        p->integ += step;
        out = base + pTerm + p->integ + dTerm;

        clamped = out;
        if (clamped > p->outMax)
        {
            clamped = p->outMax;
        }
        if (clamped < p->outMin)
        {
            clamped = p->outMin;
        }
        if (clamped != out)
        {
            /* hard limit: back-calculation. The integrator takes the value that yields
               the clamped output, so it can never run away behind the limit. */
            p->integ = clamped - base - pTerm - dTerm;
        }

        applied = clamped;
        if (p->outSlew > 0.0f)
        {
            if (applied > p->lastOut + p->outSlew)
            {
                applied = p->lastOut + p->outSlew;
            }
            if (applied < p->lastOut - p->outSlew)
            {
                applied = p->lastOut - p->outSlew;
            }
            if ((applied != clamped) && (((clamped - applied) * step) > 0.0f))
            {
                /* rate limit: undo this tick's accumulation if it was pushing the
                   same way (conditional integration). Back-calculating here instead
                   would make the integrator forget the demand and stall the ramp. */
                p->integ -= step;
            }
        }
    }

    p->lastOut = applied;
    return applied;
}
