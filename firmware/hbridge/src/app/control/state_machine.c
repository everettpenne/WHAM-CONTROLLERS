/*
 * state_machine.c -- see state_machine.h for the states, transitions and
 * fault policy.
 */
#include "state_machine.h"
#include "pfm.h"
#include "hrtim.h"
#include "gate_driver.h"
#include "mcu.h"
#include "led_indicators.h"

static SM_State_t     g_state     = SM_STATE_IDLE;
static SM_FaultType_t g_faultType = SM_FAULT_NONE;

/* The ONLY place g_state is assigned: the state LEDs (led_indicators.h) are
   updated in the same step, so an LED is lit if and only if the machine is
   in that state, whichever context (main loop, EXTI, control interrupt)
   caused the transition. */
static void SetState(SM_State_t s)
{
    g_state = s;
    switch (s)
    {
        case SM_STATE_IDLE:   LedIndicators_ShowState(LED_STATE_IDLE);   break;
        case SM_STATE_ARMED:  LedIndicators_ShowState(LED_STATE_ARMED);  break;
        case SM_STATE_FIRING: LedIndicators_ShowState(LED_STATE_FIRING); break;
        case SM_STATE_FAULT:  LedIndicators_ShowState(LED_STATE_FAULT);  break;
        default:              LedIndicators_ShowState(LED_STATE_NONE);   break;
    }
}

/* Set by SM_ReportGeneralFault() (a software-reported fault has no
   hardware latch of its own); cleared by SM_ClearFault(). */
static uint8_t g_softFaultLatched = 0U;

/* The closed-loop faults (SM_FAULT_SRC_OVERVOLT / _FEEDBACK), latched until FAULT:CLEAR. */
static volatile uint8_t g_controlFaults = 0U;

/* DEBUG:FAULT:BYPASS (see SM_SetFaultBypassEnabled() in state_machine.h): a bench-only override, RAM-only, OFF at every
   boot. While ON the state machine ignores the two EXTERNAL fault inputs (PC10/HRTIM1_FLT6 and the 12 GateDriverStatus
   pins) and GENERAL:TEST:FAULT; it does NOT ignore the faults the controller finds in itself (the closed-loop
   overvoltage/feedback faults, a table underrun) -- those leave the output half-controlled, not merely unconfirmed. */
static volatile uint8_t g_faultBypass = 0U;

static uint8_t AnySourceLatched(void)
{
    uint8_t external = ((HRTIM1_FaultIsTripped() != 0U) || (GateDriver_FaultIsLatched() != 0U)) ? 1U : 0U;

    if (g_faultBypass != 0U)
    {
        external = 0U;
    }
    return ((external != 0U) || (PFM_UnderrunLatched() != 0U) ||
            (g_softFaultLatched != 0U) || (g_controlFaults != 0U)) ? 1U : 0U;
}

void SM_SetFaultBypassEnabled(uint8_t enabled)
{
    uint8_t wasEnabled = g_faultBypass;

    g_faultBypass = (enabled != 0U) ? 1U : 0U;

    /* Turning it OFF returns to normal operation. The GateDriverStatus pins are evaluated only on an EDGE (EXTI), so a
       pin that sat in its fault state the whole time the bypass was on would otherwise never be noticed again: re-scan
       once now, as app.c does at boot for the same reason. (PC10 is a hardware latch and shows up on its own.) */
    if ((wasEnabled != 0U) && (g_faultBypass == 0U))
    {
        GateDriver_CheckFault();
    }
}

uint8_t SM_GetFaultBypassEnabled(void)
{
    return g_faultBypass;
}

/* The single place a fault is latched into the state machine. Callers hold
   interrupts masked. The output is stopped immediately and unconditionally
   (idempotent if it was already stopped by the hardware/EXTI path). */
static void EnterFault(SM_FaultType_t type)
{
    SetState(SM_STATE_FAULT);
    g_faultType = type;
    PFM_ForceStop();
}

void SM_Init(void)
{
    SetState(SM_STATE_IDLE);
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
        SetState(SM_STATE_ARMED);
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
        SetState(SM_STATE_IDLE);
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
    SetState(SM_STATE_FIRING);
    PFM_Restart();
    return 1U;
}

void SM_NotifyShotComplete(void)
{
    if (g_state == SM_STATE_FIRING)
    {
        SetState(SM_STATE_IDLE);
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

uint8_t SM_GetFaultSources(void)
{
    uint8_t m = 0U;

    if (HRTIM1_FaultIsTripped() != 0U)    { m |= SM_FAULT_SRC_PC10; }
    if (GateDriver_FaultIsLatched() != 0U) { m |= SM_FAULT_SRC_GATEDRV; }
    if (PFM_UnderrunLatched() != 0U)       { m |= SM_FAULT_SRC_UNDERRUN; }
    if (g_softFaultLatched != 0U)          { m |= SM_FAULT_SRC_TEST; }
    m |= g_controlFaults;
    return m;
}

void SM_ReportControlFault(uint8_t src)
{
    Mcu_IrqDisable();
    g_controlFaults |= (uint8_t)(src & (SM_FAULT_SRC_OVERVOLT | SM_FAULT_SRC_FEEDBACK));
    if (g_state != SM_STATE_FAULT)
    {
        EnterFault(SM_FAULT_GENERAL);
    }
    Mcu_IrqEnable();
}

void SM_ReportGeneralFault(void)
{
    if (g_faultBypass != 0U)
    {
        return;   /* DEBUG:FAULT:BYPASS: an injected test fault is ignored like any other external one */
    }
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
    g_controlFaults = 0U;
    /* With DEBUG:FAULT:BYPASS on, an external input that is still bad no longer refuses the clear: the point of the
       bypass is to keep working with that fault present. (A physically tripped PC10 still holds the HRTIM outputs
       off in silicon whatever this says.) */
    if ((g_faultBypass != 0U) || ((HRTIM1_FaultIsTripped() == 0U) && (GateDriver_FaultIsLatched() == 0U)))
    {
        SetState(SM_STATE_IDLE);
        g_faultType = SM_FAULT_NONE;
        cleared     = 1U;
    }
    Mcu_IrqEnable();
    return cleared;
}
