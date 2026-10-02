/*
 * feedback.c -- see feedback.h.
 */
#include "feedback.h"

#define PI_F  (3.14159265f)

void Feedback_Init(feedback_t *fb, float vrefV)
{
    fb->vrefV = vrefV;
    fb->scale = 1.0f;
    fb->offsetV = 0.0f;
    fb->alpha = 1.0f;
    Feedback_Reset(fb);
}

void Feedback_SetCalibration(feedback_t *fb, float scale, float offsetV)
{
    fb->scale = scale;
    fb->offsetV = offsetV;
}

void Feedback_SetBipolarRange(feedback_t *fb, float vmax)
{
    Feedback_SetCalibration(fb, 2.0f * vmax / fb->vrefV, fb->vrefV * 0.5f);
}

void Feedback_SetFilter(feedback_t *fb, float cutoffHz, float dt)
{
    if ((cutoffHz <= 0.0f) || (dt <= 0.0f))
    {
        fb->alpha = 1.0f;
        return;
    }
    /* first-order low-pass, alpha = dt / (dt + RC), RC = 1 / (2 pi fc) */
    {
        float rc = 1.0f / (2.0f * PI_F * cutoffHz);
        fb->alpha = dt / (dt + rc);
    }
}

void Feedback_Reset(feedback_t *fb)
{
    fb->rawVolts = 0.0f;
    fb->volts = 0.0f;
    fb->primed = 0U;
    fb->clipHighTicks = 0U;
    fb->clipLowTicks = 0U;
}

float Feedback_Process(feedback_t *fb, float meanCode)
{
    float adcVolts = meanCode * fb->vrefV / 4095.0f;
    float v = (adcVolts - fb->offsetV) * fb->scale;

    fb->rawVolts = v;
    if (fb->primed == 0U)
    {
        fb->volts = v;          /* start the filter at the first sample: no ramp-up from zero */
        fb->primed = 1U;
    }
    else
    {
        fb->volts += fb->alpha * (v - fb->volts);
    }
    if (meanCode >= (float)FEEDBACK_CLIP_CODE)
    {
        fb->clipHighTicks++;
    }
    else
    {
        fb->clipHighTicks = 0U;
    }
    if ((fb->offsetV > 0.0f) && (meanCode <= (float)FEEDBACK_CLIP_LOW_CODE))
    {
        fb->clipLowTicks++;
    }
    else
    {
        fb->clipLowTicks = 0U;
    }
    return fb->volts;
}
