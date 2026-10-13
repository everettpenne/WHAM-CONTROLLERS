/*
 * test_target_profile.c -- the closed-loop target profile (src/app/control/target_profile.c) on the
 * host: upload rules, evaluation between setpoints, and the signed millivolt parser.
 */
#include "target_profile.h"
#include "test_util.h"
#include <string.h>

static float Abs(float x) { return x < 0.0f ? -x : x; }

static void TestParse(void)
{
    int32_t v = 0;
    CHECK(Target_ParseMilliVolts("1.25", &v) == 1U && v == 1250);
    CHECK(Target_ParseMilliVolts("-0.5", &v) == 1U && v == -500);
    CHECK(Target_ParseMilliVolts("12", &v) == 1U && v == 12000);
    CHECK(Target_ParseMilliVolts("0", &v) == 1U && v == 0);
    CHECK(Target_ParseMilliVolts("-0.001", &v) == 1U && v == -1);
    CHECK(Target_ParseMilliVolts("1.2345", &v) == 0U);          /* more than 3 decimals */
    CHECK(Target_ParseMilliVolts("--1", &v) == 0U);
    CHECK(Target_ParseMilliVolts("+1", &v) == 0U);
    CHECK(Target_ParseMilliVolts("abc", &v) == 0U);
    CHECK(Target_ParseMilliVolts("", &v) == 0U);
    CHECK(Target_ParseMilliVolts("-", &v) == 0U);
    CHECK(Target_ParseMilliVolts(NULL, &v) == 0U);
}

static void TestUpload(void)
{
    target_setpoint_t sp;

    Target_Clear();
    CHECK(Target_Count() == 0U && Target_DurationUs() == 0U && !Target_IsComplete());
    CHECK(Target_AppendUniform(5U, 1000, 4000) == TARGET_APPEND_FIRST_NOT_ZERO);
    CHECK(Target_AppendUniform(0U, 1000, 4000) == TARGET_APPEND_OK);
    CHECK(!Target_IsComplete());
    CHECK(Target_AppendUniform(0U, 2000, 4000) == TARGET_APPEND_NOT_INCREASING);
    CHECK(Target_AppendUniform(1000U, 5000, 4000) == TARGET_APPEND_TOO_HIGH);
    CHECK(Target_AppendUniform(1000U, -5000, 4000) == TARGET_APPEND_TOO_HIGH);       /* by magnitude */
    CHECK(Target_AppendUniform(1000U, 4000, 4000) == TARGET_APPEND_OK);              /* exactly at the limit */
    CHECK(Target_AppendUniform(2000U, -4000, 4000) == TARGET_APPEND_OK);
    CHECK(Target_IsComplete() && Target_Count() == 3U && Target_DurationUs() == 2000U);
    CHECK(Target_AppendUniform(SHOT_MAX_DURATION_US + 1U, 0, 4000) == TARGET_APPEND_TOO_LONG);
    CHECK(Target_Get(1U, &sp) == 1U && sp.tUs == 1000U && sp.milliVolts[0] == 4000 && sp.milliVolts[1] == 4000 && sp.milliVolts[2] == 4000);
    CHECK(Target_Get(3U, &sp) == 0U);

    Target_Clear();
    for (uint32_t i = 0U; i < TARGET_SETPOINT_MAX; i++) { CHECK(Target_AppendUniform(i, 0, 4000) == TARGET_APPEND_OK); }
    CHECK(Target_AppendUniform(TARGET_SETPOINT_MAX, 0, 4000) == TARGET_APPEND_FULL);
    Target_Clear();
    CHECK(Target_Count() == 0U);
}

static void TestEvaluate(void)
{
    Target_Clear();
    Target_AppendUniform(0U, 0, 4000);
    Target_AppendUniform(10000U, 2000, 4000);     /* 0 -> 2 V over 10 ms */
    Target_AppendUniform(20000U, 2000, 4000);     /* hold */
    Target_AppendUniform(30000U, -1000, 4000);    /* down through zero to -1 V */
    Target_Rewind();

    CHECK(Abs(Target_ValueAt(0U, 0U) - 0.0f) < 1e-6f);
    CHECK(Abs(Target_ValueAt(0U, 5000U) - 1.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(0U, 10000U) - 2.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(0U, 15000U) - 2.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(0U, 20000U) - 2.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(0U, 25000U) - 0.5f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(0U, 30000U) + 1.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(0U, 99999U) + 1.0f) < 1e-5f);        /* holds the last value after the end */

    Target_Rewind();                                           /* evaluation can start over */
    CHECK(Abs(Target_ValueAt(0U, 2500U) - 0.5f) < 1e-5f);

    /* a tick-by-tick walk at 10 kHz is monotonic through the first ramp */
    Target_Rewind();
    float prev = -1.0f;
    for (uint32_t t = 0U; t <= 10000U; t += 100U)
    {
        float v = Target_ValueAt(0U, t);
        CHECK(v >= prev);
        prev = v;
    }
    Target_Clear();
    CHECK(Target_ValueAt(0U, 0U) == 0.0f);
}


/* ---- one target per channel (2026-10-13) ---- */

static target_append_result_t Add3(uint32_t t, int32_t a, int32_t b, int32_t c)
{
    const int32_t v[HRTIM_NUM_CHANNELS] = { a, b, c };

    return Target_Append(t, v, 4000);
}

static void TestPerChannel(void)
{
    target_setpoint_t sp;

    CHECK(HRTIM_NUM_CHANNELS == 3U);
    Target_Clear();
    CHECK(Target_IsUniform());
    CHECK(Add3(0U, 0, 1000, -2000) == TARGET_APPEND_OK);
    CHECK(Add3(10000U, 2000, 1000, 2000) == TARGET_APPEND_OK);
    CHECK(Add3(20000U, 2000, -1000, 4000) == TARGET_APPEND_OK);
    CHECK(!Target_IsUniform());
    CHECK(Target_Get(1U, &sp) == 1U && sp.milliVolts[0] == 2000 && sp.milliVolts[1] == 1000 && sp.milliVolts[2] == 2000);

    /* each channel's limit, and which channel failed */
    CHECK(Add3(30000U, 4001, 0, 0) == TARGET_APPEND_TOO_HIGH && Target_BadChannel() == 0U);
    CHECK(Add3(30000U, 0, -4001, 0) == TARGET_APPEND_TOO_HIGH && Target_BadChannel() == 1U);
    CHECK(Add3(30000U, 0, 0, 4001) == TARGET_APPEND_TOO_HIGH && Target_BadChannel() == 2U);
    CHECK(Target_Count() == 3U);

    /* minimum per channel */
    CHECK(Target_MinMilliVolts(0U) == 0 && Target_MinMilliVolts(1U) == -1000 && Target_MinMilliVolts(2U) == -2000);
    CHECK(Target_MinMilliVolts(3U) == 0);                                      /* no such channel */

    /* each channel evaluates its own series on the shared time grid */
    Target_Rewind();
    CHECK(Abs(Target_ValueAt(0U, 5000U) - 1.0f) < 1e-5f);                      /* 0 -> 2 V */
    CHECK(Abs(Target_ValueAt(1U, 5000U) - 1.0f) < 1e-5f);                      /* flat 1 V */
    CHECK(Abs(Target_ValueAt(2U, 5000U) - 0.0f) < 1e-5f);                      /* -2 -> 2 V: zero at the middle */
    CHECK(Abs(Target_ValueAt(0U, 15000U) - 2.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(1U, 15000U) - 0.0f) < 1e-5f);                     /* 1 -> -1 V */
    CHECK(Abs(Target_ValueAt(2U, 15000U) - 3.0f) < 1e-5f);                     /* 2 -> 4 V */
    CHECK(Abs(Target_ValueAt(2U, 99999U) - 4.0f) < 1e-5f);                     /* each holds its own last value */
    CHECK(Abs(Target_ValueAt(1U, 99999U) + 1.0f) < 1e-5f);
    CHECK(Target_ValueAt(3U, 5000U) == 0.0f);                                  /* no such channel */

    /* the channels may be asked in any order within a tick, and an earlier time searches again */
    Target_Rewind();
    CHECK(Abs(Target_ValueAt(2U, 15000U) - 3.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(0U, 15000U) - 2.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(1U, 2500U) - 1.0f) < 1e-5f);                      /* earlier than the cursor */
    CHECK(Abs(Target_ValueAt(0U, 2500U) - 0.5f) < 1e-5f);

    /* the one-voltage form is three equal voltages */
    Target_Clear();
    CHECK(Target_AppendUniform(0U, 500, 4000) == TARGET_APPEND_OK);
    CHECK(Target_AppendUniform(1000U, 1500, 4000) == TARGET_APPEND_OK);
    CHECK(Target_IsUniform());
    CHECK(Target_AppendUniform(2000U, 5000, 4000) == TARGET_APPEND_TOO_HIGH);
}

int main(void)
{
    TestParse();
    TestUpload();
    TestEvaluate();
    TestPerChannel();
    return test_report("test_target_profile");
}
