/*
 * sim_plant.c -- see sim_plant.h.
 */
#include "sim_plant.h"
#include "hrtim.h"

void SimPlant_Init(sim_plant_t *p)
{
    p->tau = 0.003f;
    p->gain = 1.0f;
    p->resonance = 19000.0f;
    p->width = 3000.0f;
    p->vpeak = 2.0f;
    p->vscale = 1.0f;
    p->v = 0.0f;
}

void SimPlant_Reset(sim_plant_t *p, float volts)
{
    p->v = volts;
}

float SimPlant_Step(sim_plant_t *p, uint8_t limiter, uint16_t per, uint16_t cmp, float dt)
{
    float target;

    if (limiter != 0U)
    {
        float duty = (per != 0U) ? ((float)cmp / (float)per) : 0.5f;
        target = SimPlant_LimiterSteady(p, duty);
    }
    else
    {
        float f = (float)HRTIM_TIMER_CLK_HZ / ((float)per + 1.0f);
        float x = (f - p->resonance) / p->width;
        target = p->gain * p->vpeak / (1.0f + x * x);
    }
    return SimPlant_StepSteady(p, target, dt);
}

float SimPlant_StepSteady(sim_plant_t *p, float steadyVolts, float dt)
{
    p->v += (steadyVolts - p->v) * (dt / p->tau);
    return p->v;
}

float SimPlant_LimiterSteady(const sim_plant_t *p, float duty)
{
    return p->gain * p->vscale * (2.0f * duty - 1.0f);
}
