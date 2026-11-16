/*
 * test_spark_gap.c -- the spark-gap fault input (src/app/protection/spark_gap.c) against a fake pin, a fake output stop
 * and a fake bypass flag: a LOW pin stops the output and latches, HIGH does nothing, the bypass ignores it, and a clear
 * re-checks the pin.
 */
#include "spark_gap.h"
#include "test_util.h"
#include <stdint.h>

static uint8_t f_pin;           /* 1 = HIGH (healthy) */
static uint8_t f_bypass;
static int     f_stops;

uint8_t BoardIo_ReadSparkGap(void) { return f_pin; }
void PFM_ForceStop(void) { f_stops++; }
uint8_t SM_GetFaultBypassEnabled(void) { return f_bypass; }

static void Reset(void)
{
    f_pin = 1U; f_bypass = 0U; f_stops = 0;
    SparkGap_FaultClear();
    f_stops = 0;
}

int main(void)
{
    Reset();
    CHECK(SparkGap_Read() == 1U && SparkGap_FaultIsLatched() == 0U);

    /* HIGH is healthy: a check does nothing */
    SparkGap_CheckFault();
    CHECK(SparkGap_FaultIsLatched() == 0U && f_stops == 0);

    /* LOW is a fault: the output is stopped and the fault latches, and stays latched when the pin recovers */
    f_pin = 0U;
    SparkGap_CheckFault();
    CHECK(SparkGap_FaultIsLatched() == 1U && f_stops == 1 && SparkGap_Read() == 0U);
    f_pin = 1U;
    SparkGap_CheckFault();
    CHECK(SparkGap_FaultIsLatched() == 1U);                  /* latched until FAULT:CLEar */

    /* a clear with the pin healthy again releases it; with the pin still LOW it re-latches before returning */
    SparkGap_FaultClear();
    CHECK(SparkGap_FaultIsLatched() == 0U);
    f_pin = 0U;
    f_stops = 0;
    SparkGap_CheckFault();
    SparkGap_FaultClear();
    CHECK(SparkGap_FaultIsLatched() == 1U && f_stops >= 2);

    /* DEBUG:FAULT:BYPASS: a LOW pin neither stops the output nor latches, and a clear is not undone by the re-check */
    Reset();
    f_bypass = 1U; f_pin = 0U;
    SparkGap_CheckFault();
    CHECK(SparkGap_FaultIsLatched() == 0U && f_stops == 0);
    SparkGap_FaultClear();
    CHECK(SparkGap_FaultIsLatched() == 0U);
    f_bypass = 0U;                                            /* the bypass off: the next check sees the pin */
    SparkGap_CheckFault();
    CHECK(SparkGap_FaultIsLatched() == 1U);

    return test_report("test_spark_gap");
}
