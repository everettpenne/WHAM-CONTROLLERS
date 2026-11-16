/*
 * spark_gap.c -- see spark_gap.h.
 */
#include "spark_gap.h"
#include "board_io.h"
#include "pfm.h"
#include "state_machine.h"
#include "ctrlr_config.h"

/* Set from SparkGap_CheckFault() (EXTI interrupt context), cleared from the main loop by SparkGap_FaultClear(). */
static volatile uint8_t g_latched = 0U;
static volatile uint8_t g_enabled = (SPARK_GAP_DEFAULT_ENABLED != 0U) ? 1U : 0U;

void SparkGap_Reset(void)
{
    g_enabled = (SPARK_GAP_DEFAULT_ENABLED != 0U) ? 1U : 0U;
}

uint8_t SparkGap_IsEnabled(void)
{
    return g_enabled;
}

void SparkGap_SetEnabled(uint8_t enabled)
{
    g_enabled = (enabled != 0U) ? 1U : 0U;
}

uint8_t SparkGap_Read(void)
{
    return BoardIo_ReadSparkGap();
}

void SparkGap_CheckFault(void)
{
    /* DEBUG:FAULT:BYPASS: ignored like every other external fault input; otherwise this interrupt would stop a shot the
       state machine has been told not to watch this input for, and SparkGap_FaultClear() would re-latch it at once. */
    if ((g_enabled != 0U) && (SparkGap_Read() == 0U) && (SM_GetFaultBypassEnabled() == 0U))
    {
        PFM_ForceStop();
        g_latched = 1U;
    }
}

uint8_t SparkGap_FaultIsLatched(void)
{
    return g_latched;
}

void SparkGap_FaultClear(void)
{
    PFM_ForceStop();
    g_latched = 0U;
    SparkGap_CheckFault();        /* an edge interrupt would not fire again for a pin that is still LOW */
}
