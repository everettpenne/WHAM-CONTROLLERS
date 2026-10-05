/*
 * test_hvps_curve.c -- the HVPS frequency -> volts lookup table: validation, interpolation, the default
 * linear shape, and inversion on either branch.
 */
#include "hvps_curve.h"
#include "test_util.h"
#include <math.h>

static float Abs(float x) { return x < 0.0f ? -x : x; }
static int Near(float a, float b, float tol) { return Abs(a - b) <= tol; }

static void TestAppendRules(void)
{
    hvps_curve_t c;

    HvpsCurve_Clear(&c);
    CHECK(HvpsCurve_IsValid(&c) == 0U);                               /* empty */
    CHECK(HvpsCurve_Append(&c, 10000.0f, 0.0f) == HVPS_CURVE_OK);
    CHECK(HvpsCurve_IsValid(&c) == 0U);                               /* one point */
    CHECK(HvpsCurve_Append(&c, 10000.0f, 0.5f) == HVPS_CURVE_NOT_INCREASING);
    CHECK(HvpsCurve_Append(&c, 9000.0f, 0.5f) == HVPS_CURVE_NOT_INCREASING);
    CHECK(HvpsCurve_Append(&c, 19000.0f, -0.1f) == HVPS_CURVE_NEGATIVE);   /* power is never negative */
    CHECK(c.n == 1U);                                                 /* rejected points are not stored */
    CHECK(HvpsCurve_Append(&c, 19000.0f, 1.5f) == HVPS_CURVE_OK);
    CHECK(HvpsCurve_Append(&c, 30000.0f, 0.0f) == HVPS_CURVE_OK);
    CHECK(HvpsCurve_IsValid(&c) == 1U);
    CHECK(Near(HvpsCurve_PeakHz(&c), 19000.0f, 1e-3f) && Near(HvpsCurve_PeakVolts(&c), 1.5f, 1e-6f));

    for (uint16_t i = c.n; i < HVPS_CURVE_MAX_POINTS; i++)
    {
        CHECK(HvpsCurve_Append(&c, 30000.0f + (float)i, 0.0f) == HVPS_CURVE_OK);
    }
    CHECK(HvpsCurve_Append(&c, 99999.0f, 0.0f) == HVPS_CURVE_FULL);
}

static void TestUnimodal(void)
{
    hvps_curve_t c;

    HvpsCurve_Clear(&c);
    (void)HvpsCurve_Append(&c, 10000.0f, 0.2f);
    (void)HvpsCurve_Append(&c, 15000.0f, 1.0f);
    (void)HvpsCurve_Append(&c, 19000.0f, 1.0f);                        /* a flat top is fine */
    (void)HvpsCurve_Append(&c, 25000.0f, 0.4f);
    CHECK(HvpsCurve_IsValid(&c) == 1U);
    (void)HvpsCurve_Append(&c, 28000.0f, 0.4f);                        /* flat on the way down is fine */
    CHECK(HvpsCurve_IsValid(&c) == 1U);
    (void)HvpsCurve_Append(&c, 30000.0f, 0.9f);                        /* rises again: a second peak */
    CHECK(HvpsCurve_IsValid(&c) == 0U);
}

static void TestLinearAndEval(void)
{
    hvps_curve_t c;

    HvpsCurve_SetLinear(&c, 10000.0f, 19000.0f, 30000.0f, 1.5f);
    CHECK(c.n == 3U && HvpsCurve_IsValid(&c) == 1U);
    CHECK(Near(HvpsCurve_Eval(&c, 19000.0f), 1.5f, 1e-6f));
    CHECK(Near(HvpsCurve_Eval(&c, 30000.0f), 0.0f, 1e-6f) && Near(HvpsCurve_Eval(&c, 10000.0f), 0.0f, 1e-6f));
    CHECK(Near(HvpsCurve_Eval(&c, 24500.0f), 0.75f, 1e-5f));            /* halfway down the upper side */
    CHECK(Near(HvpsCurve_Eval(&c, 14500.0f), 0.75f, 1e-5f));            /* halfway up the lower side */
    CHECK(Near(HvpsCurve_Eval(&c, 5000.0f), 0.0f, 1e-6f) && Near(HvpsCurve_Eval(&c, 50000.0f), 0.0f, 1e-6f));   /* end values beyond */

    /* a resonance at the edge of the band leaves one side out */
    HvpsCurve_SetLinear(&c, 19000.0f, 19000.0f, 30000.0f, 1.0f);
    CHECK(c.n == 2U && Near(c.hz[0], 19000.0f, 1e-3f));
    HvpsCurve_SetLinear(&c, 10000.0f, 30000.0f, 30000.0f, 1.0f);
    CHECK(c.n == 2U && Near(c.hz[1], 30000.0f, 1e-3f));

    HvpsCurve_Clear(&c);
    CHECK(Near(HvpsCurve_Eval(&c, 20000.0f), 0.0f, 1e-9f) && Near(HvpsCurve_FreqFor(&c, 1.0f, 1U), 0.0f, 1e-9f));
}

static void TestFreqFor(void)
{
    hvps_curve_t lin, tab;

    HvpsCurve_SetLinear(&lin, 10000.0f, 19000.0f, 30000.0f, 1.5f);
    CHECK(Near(HvpsCurve_FreqFor(&lin, 1.5f, 1U), 19000.0f, 1e-2f));    /* the peak */
    CHECK(Near(HvpsCurve_FreqFor(&lin, 9.0f, 1U), 19000.0f, 1e-2f));    /* beyond the peak: the peak */
    CHECK(Near(HvpsCurve_FreqFor(&lin, 0.0f, 1U), 30000.0f, 1e-2f));    /* zero power: the far end */
    CHECK(Near(HvpsCurve_FreqFor(&lin, 0.75f, 1U), 24500.0f, 1e-1f));
    CHECK(Near(HvpsCurve_FreqFor(&lin, 0.75f, 0U), 14500.0f, 1e-1f));
    CHECK(Near(HvpsCurve_FreqFor(&lin, 0.0f, 0U), 10000.0f, 1e-2f));
    /* the "typical start": 20 kHz is a little under the peak on the upper side */
    CHECK(Near(HvpsCurve_Eval(&lin, 20000.0f), 1.5f * 10.0f / 11.0f, 1e-4f));
    CHECK(Near(HvpsCurve_FreqFor(&lin, 1.5f * 10.0f / 11.0f, 1U), 20000.0f, 1e-1f));

    /* a nonlinear table (a Lorentzian sampled every kHz): inverting gives back the frequency on either branch */
    HvpsCurve_Clear(&tab);
    for (int f = 10000; f <= 30000; f += 1000)
    {
        float x = ((float)f - 19000.0f) / 3000.0f;
        CHECK(HvpsCurve_Append(&tab, (float)f, 1.5f / (1.0f + x * x)) == HVPS_CURVE_OK);
    }
    CHECK(HvpsCurve_IsValid(&tab) == 1U);
    for (int f = 19000; f <= 30000; f += 500)
    {
        float v = HvpsCurve_Eval(&tab, (float)f);
        CHECK(Near(HvpsCurve_FreqFor(&tab, v, 1U), (float)f, 2.0f));
    }
    for (int f = 10000; f <= 19000; f += 500)
    {
        float v = HvpsCurve_Eval(&tab, (float)f);
        CHECK(Near(HvpsCurve_FreqFor(&tab, v, 0U), (float)f, 2.0f));
    }
}

int main(void)
{
    TestAppendRules();
    TestUnimodal();
    TestLinearAndEval();
    TestFreqFor();
    return test_report("test_hvps_curve");
}
