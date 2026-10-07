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
    CHECK(Target_Append(5U, 1000, 4000) == TARGET_APPEND_FIRST_NOT_ZERO);
    CHECK(Target_Append(0U, 1000, 4000) == TARGET_APPEND_OK);
    CHECK(!Target_IsComplete());
    CHECK(Target_Append(0U, 2000, 4000) == TARGET_APPEND_NOT_INCREASING);
    CHECK(Target_Append(1000U, 5000, 4000) == TARGET_APPEND_TOO_HIGH);
    CHECK(Target_Append(1000U, -5000, 4000) == TARGET_APPEND_TOO_HIGH);       /* by magnitude */
    CHECK(Target_Append(1000U, 4000, 4000) == TARGET_APPEND_OK);              /* exactly at the limit */
    CHECK(Target_Append(2000U, -4000, 4000) == TARGET_APPEND_OK);
    CHECK(Target_IsComplete() && Target_Count() == 3U && Target_DurationUs() == 2000U);
    CHECK(Target_Append(SHOT_MAX_DURATION_US + 1U, 0, 4000) == TARGET_APPEND_TOO_LONG);
    CHECK(Target_Get(1U, &sp) == 1U && sp.tUs == 1000U && sp.milliVolts == 4000);
    CHECK(Target_Get(3U, &sp) == 0U);

    Target_Clear();
    for (uint32_t i = 0U; i < TARGET_SETPOINT_MAX; i++) { CHECK(Target_Append(i, 0, 4000) == TARGET_APPEND_OK); }
    CHECK(Target_Append(TARGET_SETPOINT_MAX, 0, 4000) == TARGET_APPEND_FULL);
    Target_Clear();
    CHECK(Target_Count() == 0U);
}

static void TestEvaluate(void)
{
    Target_Clear();
    Target_Append(0U, 0, 4000);
    Target_Append(10000U, 2000, 4000);     /* 0 -> 2 V over 10 ms */
    Target_Append(20000U, 2000, 4000);     /* hold */
    Target_Append(30000U, -1000, 4000);    /* down through zero to -1 V */
    Target_Rewind();

    CHECK(Abs(Target_ValueAt(0U) - 0.0f) < 1e-6f);
    CHECK(Abs(Target_ValueAt(5000U) - 1.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(10000U) - 2.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(15000U) - 2.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(20000U) - 2.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(25000U) - 0.5f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(30000U) + 1.0f) < 1e-5f);
    CHECK(Abs(Target_ValueAt(99999U) + 1.0f) < 1e-5f);        /* holds the last value after the end */

    Target_Rewind();                                           /* evaluation can start over */
    CHECK(Abs(Target_ValueAt(2500U) - 0.5f) < 1e-5f);

    /* a tick-by-tick walk at 10 kHz is monotonic through the first ramp */
    Target_Rewind();
    float prev = -1.0f;
    for (uint32_t t = 0U; t <= 10000U; t += 100U)
    {
        float v = Target_ValueAt(t);
        CHECK(v >= prev);
        prev = v;
    }
    Target_Clear();
    CHECK(Target_ValueAt(0U) == 0.0f);
}

int main(void)
{
    TestParse();
    TestUpload();
    TestEvaluate();
    return test_report("test_target_profile");
}
