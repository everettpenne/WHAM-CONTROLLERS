/*
 * shot_start.c -- see shot_start.h.
 */
#include "shot_start.h"
#include "state_machine.h"
#include "pfm.h"
#include "hrtim.h"
#include "gate_driver.h"
#include "mcu.h"

/* Taken with interrupts masked, so two callers (the main loop and the trigger's software interrupt) can never both
   be inside the start sequence. */
static volatile uint8_t g_busy = 0U;

uint8_t ShotStart_FaultLatched(void)
{
    return (HRTIM1_FaultIsTripped() != 0U) || (GateDriver_FaultIsLatched() != 0U) || (PFM_UnderrunLatched() != 0U) ||
           ((SM_GetFaultSources() & (SM_FAULT_SRC_OVERVOLT | SM_FAULT_SRC_FEEDBACK)) != 0U);
}

shot_start_result_t ShotStart_Run(void)
{
    shot_start_result_t r = { SHOT_START_OK, CTL_OK };

    Mcu_IrqDisable();
    if (g_busy != 0U)
    {
        Mcu_IrqEnable();
        r.code = SHOT_START_BUSY;
        return r;
    }
    g_busy = 1U;
    Mcu_IrqEnable();

    if (ShotStart_FaultLatched() != 0U)
    {
        r.code = SHOT_START_FAULT;
    }
    else if (SM_GetState() != SM_STATE_ARMED)
    {
        r.code = SHOT_START_NOT_ARMED;
    }
    else if (Control_ClosedRunWanted() != 0U)
    {
        /* closed loop on at least one phase: the TARGet:* profile for the closed ones (and the SHOT:* profile for
           any open ones), played by the controller through the live source */
        r.ctl = Control_Prepare(1U);
        if (r.ctl != CTL_OK)
        {
            r.code = SHOT_START_CONTROL;
        }
        else if (SM_Fire() == 0U)
        {
            Control_Stop();
            r.code = SHOT_START_CHANGED;
        }
        else
        {
            Control_Go();
        }
    }
    else
    {
        PFM_RestoreOpenSource();    /* an open-loop shot after a closed-loop run plays the uploaded shot again */
        if (PFM_ShotReady() == 0U)
        {
            r.code = SHOT_START_NO_SHOT;     /* PFM_CycleBoundaryHandler() would otherwise stop the output again at
                                                the first interrupt, silently, after a near-instant blip */
        }
        else if (SM_Fire() == 0U)
        {
            r.code = SHOT_START_CHANGED;     /* narrow race only */
        }
    }

    g_busy = 0U;
    return r;
}
