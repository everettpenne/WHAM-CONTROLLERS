/*
 * shot_trigger.c -- see shot_trigger.h.
 */
#include "shot_trigger.h"
#include "state_machine.h"
#include "board_io.h"
#include "ctrlr_config.h"
#include "mcu.h"

static volatile uint8_t g_enabled = (SHOT_TRIGGER_DEFAULT_ENABLED != 0U) ? 1U : 0U;
static volatile uint8_t g_pending = 0U;
static shot_trigger_status_t g_st;      /* written from the two interrupts only (never preempting each other mid-update
                                           in a way that matters: counters, and `last` which only the software
                                           interrupt writes) */

void ShotTrigger_Init(void)
{
    Mcu_StartIrqInit();
}

void ShotTrigger_SetEnabled(uint8_t on)
{
    g_enabled = (on != 0U) ? 1U : 0U;
}

uint8_t ShotTrigger_GetEnabled(void)
{
    return g_enabled;
}

void ShotTrigger_OnEdge(void)
{
    g_st.edges++;
    if (g_enabled == 0U)
    {
        g_st.ignored_disabled++;
    }
    else if (SM_GetState() != SM_STATE_ARMED)
    {
        g_st.ignored_state++;
    }
    else if (BoardIo_ReadExtTrigger() == 0U)
    {
        g_st.ignored_glitch++;          /* LOW again already: a glitch, not a trigger */
    }
    else
    {
        g_pending = 1U;
        Mcu_StartIrqPend();
    }
}

void ShotTrigger_RunPending(void)
{
    shot_start_result_t r;

    if (g_pending == 0U)
    {
        return;
    }
    g_pending = 0U;

    r = ShotStart_Run();
    g_st.last = r;
    g_st.attempts++;
    if (r.code == SHOT_START_OK)
    {
        g_st.started++;
    }
    else
    {
        g_st.refused++;
        if (r.code == SHOT_START_BUSY)
        {
            g_st.busy++;
        }
    }
}

void ShotTrigger_GetStatus(shot_trigger_status_t *out)
{
    Mcu_IrqDisable();
    *out = g_st;
    Mcu_IrqEnable();
}
