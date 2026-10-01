/*
 * state_machine.c -- see state_machine.h for the states, transitions and
 * fault policy.
 */
#include "state_machine.h"
#include "pfm.h"
#include "hrtim.h"
#include "gate_driver.h"
#include "mcu.h"

static SM_State_t     g_state     = SM_STATE_IDLE;
static SM_FaultType_t g_faultType = SM_FAULT_NONE;

/* Set by SM_ReportGeneralFault() (a software-reported fault has no
   hardware latch of its own); cleared by SM_ClearFault(). */
static uint8_t g_softFaultLatched = 0U;

static uint8_t AnySourceLatched(void)
{
    return ((HRTIM1_FaultIsTripped() != 0U) || (GateDriver_FaultIsLatched() != 0U) ||
            (PFM_UnderrunLatched() != 0U) || (g_softFaultLatched != 0U)) ? 1U : 0U;
}

/* The single place a fault is latched into the state machine. Callers hold
   interrupts masked. The output is stopped immediately and unconditionally
   (idempotent if it was already stopped by the hardware/EXTI path). */
static void EnterFault(SM_FaultType_t type)
{
    g_state     = SM_STATE_FAULT;
    g_faultType = type;
    PFM_ForceStop();
}

void SM_Init(void)
{
    g_state            = SM_STATE_IDLE;
    g_faultType        = SM_FAULT_NONE;
    g_softFaultLatched = 0U;
}

SM_State_t SM_GetState(void)
{
    return g_state;
}

SM_FaultType_t SM_GetFaultType(void)
{
    return (g_state == SM_STATE_FAULT) ? g_faultType : SM_FAULT_NONE;
}

uint8_t SM_Arm(void)
{
    uint8_t ok = 0U;

    Mcu_IrqDisable();
    if ((g_state == SM_STATE_IDLE) && (AnySourceLatched() == 0U))
    {
        g_state = SM_STATE_ARMED;
        ok = 1U;
    }
    Mcu_IrqEnable();
    return ok;
}

void SM_Disarm(void)
{
    Mcu_IrqDisable();
    if (g_state == SM_STATE_ARMED)
    {
        g_state = SM_STATE_IDLE;
    }
    Mcu_IrqEnable();
}

uint8_t SM_Fire(void)
{
    if ((g_state != SM_STATE_ARMED) || (AnySourceLatched() != 0U) || (PFM_ShotReady() == 0U))
    {
        return 0U;
    }
    /* SM_PollFaults() runs in the same (main-loop) context, so it can only
       look after this returns; by then a very short table may already have
       finished, which it then reports as a completed shot. */
    g_state = SM_STATE_FIRING;
    PFM_Restart();
    return 1U;
}

void SM_NotifyShotComplete(void)
{
    if (g_state == SM_STATE_FIRING)
    {
        g_state = SM_STATE_IDLE;
    }
}

void SM_PollFaults(void)
{
    Mcu_IrqDisable();
    if (g_state == SM_STATE_FAULT)
    {
        Mcu_IrqEnable();
        return;   /* already latched */
    }
    if (AnySourceLatched() != 0U)
    {
        EnterFault(SM_FAULT_GENERAL);
    }
    else if ((g_state == SM_STATE_FIRING) && (PFM_GetState() == PFM_STATE_STOPPED))
    {
        /* The PFM engine stops itself at the end of the table. A fault stop
           also leaves it STOPPED, but a fault is always latched in one of
           the sources and is caught by the branch above first. */
        SM_NotifyShotComplete();
    }
    Mcu_IrqEnable();
}

void SM_ReportGeneralFault(void)
{
    Mcu_IrqDisable();
    g_softFaultLatched = 1U;
    if (g_state != SM_STATE_FAULT)
    {
        EnterFault(SM_FAULT_GENERAL);
    }
    Mcu_IrqEnable();
}

uint8_t SM_ClearFault(void)
{
    uint8_t cleared = 0U;

    if (g_state != SM_STATE_FAULT)
    {
        return 0U;
    }
    /* Each re-validates its own physical input and re-latches if the
       condition is still present (GateDriver_FaultClear() re-reads the 12
       pins; the HRTIM fault flag re-trips from the pin). */
    HRTIM1_FaultClear();
    GateDriver_FaultClear();
    PFM_ClearUnderrun();

    Mcu_IrqDisable();
    g_softFaultLatched = 0U;
    if ((HRTIM1_FaultIsTripped() == 0U) && (GateDriver_FaultIsLatched() == 0U))
    {
        g_state     = SM_STATE_IDLE;
        g_faultType = SM_FAULT_NONE;
        cleared     = 1U;
    }
    Mcu_IrqEnable();
    return cleared;
}
