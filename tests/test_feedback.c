/*
 * test_feedback.c -- the feedback conditioning (src/app/control/feedback.c) on the host:
 * code to process volts, calibration, the low-pass, and the clipping count.
 */
#include "feedback.h"
#include "test_util.h"

static float Abs(float x) { return x < 0.0f ? -x : x; }

static void TestScaling(void)
{
    feedback_t fb;

    Feedback_Init(&fb, 2.9f);
    CHECK(Abs(Feedback_Process(&fb, 0.0f) - 0.0f) < 1e-6f);
    Feedback_Reset(&fb);
    CHECK(Abs(Feedback_Process(&fb, 4095.0f) - 2.9f) < 1e-5f);          /* full scale = the reference */
    Feedback_Reset(&fb);
    CHECK(Abs(Feedback_Process(&fb, 2047.5f) - 1.45f) < 1e-5f);

    /* a divider: process volts = adc volts * 10 */
    Feedback_SetCalibration(&fb, 10.0f, 0.0f);
    Feedback_Reset(&fb);
    CHECK(Abs(Feedback_Process(&fb, 2047.5f) - 14.5f) < 1e-4f);

    /* a bipolar output riding on mid-scale: 1.45 V reads as 0, scale 4 */
    Feedback_SetCalibration(&fb, 4.0f, 1.45f);
    Feedback_Reset(&fb);
    CHECK(Abs(Feedback_Process(&fb, 2047.5f) - 0.0f) < 1e-4f);
    Feedback_Reset(&fb);
    CHECK(Abs(Feedback_Process(&fb, 4095.0f) - 5.8f) < 1e-4f);          /* (2.9 - 1.45) * 4 */
    Feedback_Reset(&fb);
    CHECK(Abs(Feedback_Process(&fb, 0.0f) + 5.8f) < 1e-4f);             /* negative below the offset */
}

static void TestFilter(void)
{
    feedback_t fb;
    float v = 0.0f;

    Feedback_Init(&fb, 2.9f);
    Feedback_SetFilter(&fb, 0.0f, 0.0001f);
    Feedback_Process(&fb, 0.0f);
    v = Feedback_Process(&fb, 4095.0f);
    CHECK(Abs(v - 2.9f) < 1e-5f);                                       /* off: follows at once */

    /* 100 Hz cutoff at 10 kHz: a step settles to ~63 % in 1/(2 pi fc) = 1.59 ms = ~16 ticks */
    Feedback_SetFilter(&fb, 100.0f, 0.0001f);
    Feedback_Reset(&fb);
    Feedback_Process(&fb, 0.0f);                                        /* primed at 0 */
    for (int i = 0; i < 16; i++) { v = Feedback_Process(&fb, 4095.0f); }
    CHECK(v > 0.55f * 2.9f && v < 0.70f * 2.9f);
    for (int i = 0; i < 200; i++) { v = Feedback_Process(&fb, 4095.0f); }
    CHECK(Abs(v - 2.9f) < 1e-3f);                                       /* and gets there */
    CHECK(Abs(fb.rawVolts - 2.9f) < 1e-5f);                             /* the unfiltered value is kept */

    /* primed at the first sample: no ramp up from zero */
    Feedback_Reset(&fb);
    CHECK(Abs(Feedback_Process(&fb, 2047.5f) - 1.45f) < 1e-5f);
}

static void TestClipping(void)
{
    feedback_t fb;

    Feedback_Init(&fb, 2.9f);
    for (int i = 0; i < 4; i++) { Feedback_Process(&fb, 4095.0f); }
    CHECK(fb.clipHighTicks == 4U);
    Feedback_Process(&fb, 4090.0f);                                     /* at the threshold still counts */
    CHECK(fb.clipHighTicks == 5U);
    Feedback_Process(&fb, 4089.0f);                                     /* below it resets */
    CHECK(fb.clipHighTicks == 0U);
    Feedback_Process(&fb, 4095.0f);
    Feedback_Reset(&fb);
    CHECK(fb.clipHighTicks == 0U && fb.primed == 0U);
}

int main(void)
{
    TestScaling();
    TestFilter();
    TestClipping();
    return test_report("test_feedback");
}
