/*
 * test_state_machine.c -- the operating-state machine
 * (src/app/control/state_machine.c) on the host, with the PFM engine,
 * HRTIM fault input, gate-driver latch and interrupt masking faked.
 */
#include "state_machine.h"
#include "pfm.h"
#include "hrtim.h"
#include "gate_driver.h"
#include "mcu.h"
#include "test_util.h"
#include <string.h>

/* ---- fakes ------------------------------------------------------------- */
static PFM_State_t f_pfmState = PFM_STATE_STOPPED;
static uint16_t    f_entries  = 10U;
static int         f_restarts, f_forceStops, f_irqDepth, f_irqMax;
static uint8_t     f_hrtimTripped, f_hrtimPin, f_gdsLatched, f_gdsPin;

void PFM_Restart(void)          { f_restarts++; f_pfmState = PFM_STATE_RUNNING; }
void PFM_ForceStop(void)        { f_forceStops++; f_pfmState = PFM_STATE_STOPPED; }
PFM_State_t PFM_GetState(void)  { return f_pfmState; }
uint8_t PFM_ShotReady(void)     { return (f_entries > 0U) ? 1U : 0U; }
static uint8_t f_underrun = 0U;
uint8_t PFM_UnderrunLatched(void) { return f_underrun; }
void PFM_ClearUnderrun(void)    { f_underrun = 0U; }
uint8_t HRTIM1_FaultIsTripped(void) { return f_hrtimTripped; }
void HRTIM1_FaultClear(void)    { f_hrtimTripped = f_hrtimPin; }   /* re-trips if the pin is still low */
uint8_t GateDriver_FaultIsLatched(void) { return f_gdsLatched; }
void GateDriver_FaultClear(void){ f_gdsLatched = f_gdsPin; }        /* re-checks the pins */
void Mcu_IrqDisable(void)       { f_irqDepth++; if (f_irqDepth > f_irqMax) { f_irqMax = f_irqDepth; } }
void Mcu_IrqEnable(void)        { f_irqDepth--; }

static void Reset(void)
{
    f_pfmState = PFM_STATE_STOPPED; f_entries = 10U;
    f_restarts = f_forceStops = f_irqDepth = f_irqMax = 0;
    f_hrtimTripped = f_hrtimPin = f_gdsLatched = f_gdsPin = 0U;
    SM_Init();
}

/* ---- tests ------------------------------------------------------------- */
static void TestNormalShot(void)
{
    Reset();
    CHECK(SM_GetState() == SM_STATE_IDLE);
    CHECK(SM_GetFaultType() == SM_FAULT_NONE);
    CHECK(SM_Fire() == 0U);                      /* not ARMED */
    CHECK(f_restarts == 0);
    CHECK(SM_Arm() == 1U);
    CHECK(SM_GetState() == SM_STATE_ARMED);
    CHECK(SM_Arm() == 0U);                       /* already ARMED */
    CHECK(SM_Fire() == 1U);
    CHECK(SM_GetState() == SM_STATE_FIRING && f_restarts == 1);
    CHECK(SM_Fire() == 0U && f_restarts == 1);   /* not re-fired while FIRING */
    SM_PollFaults();
    CHECK(SM_GetState() == SM_STATE_FIRING);     /* still playing */
    f_pfmState = PFM_STATE_STOPPED;              /* table exhausted */
    SM_PollFaults();
    CHECK(SM_GetState() == SM_STATE_IDLE);       /* a new shot needs a new ARM */
    CHECK(SM_Fire() == 0U);
    CHECK(f_irqDepth == 0);
}

static void TestDisarmAndEmptyTable(void)
{
    Reset();
    SM_Disarm();                                  /* no-op in IDLE */
    CHECK(SM_GetState() == SM_STATE_IDLE);
    CHECK(SM_Arm() == 1U);
    SM_Disarm();
    CHECK(SM_GetState() == SM_STATE_IDLE);
    CHECK(SM_Arm() == 1U);
    f_entries = 0U;
    CHECK(SM_Fire() == 0U);                      /* empty table */
    CHECK(SM_GetState() == SM_STATE_ARMED && f_restarts == 0);
}

static void TestFaultFromEachSourceAndState(void)
{
    for (int src = 0; src < 3; src++)
    {
        for (int st = 0; st < 3; st++)
        {
            Reset();
            if (st >= 1) { CHECK(SM_Arm() == 1U); }
            if (st >= 2) { CHECK(SM_Fire() == 1U); }
            if (src == 0) { f_hrtimTripped = 1U; }
            if (src == 1) { f_gdsLatched = 1U; }
            if (src == 2) { SM_ReportGeneralFault(); }
            else          { SM_PollFaults(); }
            CHECK(SM_GetState() == SM_STATE_FAULT);
            CHECK(SM_GetFaultType() == SM_FAULT_GENERAL);
            CHECK(f_forceStops >= 1);
            CHECK(f_pfmState == PFM_STATE_STOPPED);
            CHECK(SM_Arm() == 0U && SM_Fire() == 0U);
            SM_Disarm();
            CHECK(SM_GetState() == SM_STATE_FAULT);
            CHECK(SM_ClearFault() == 1U);
            CHECK(SM_GetState() == SM_STATE_IDLE);
            CHECK(SM_GetFaultType() == SM_FAULT_NONE);
            SM_PollFaults();
            CHECK(SM_GetState() == SM_STATE_IDLE);
            CHECK(f_irqDepth == 0);
        }
    }
}

static void TestClearRefusedWhileConditionPresent(void)
{
    Reset();
    f_gdsLatched = 1U; f_gdsPin = 1U;            /* pin still in its fault state */
    SM_PollFaults();
    CHECK(SM_ClearFault() == 0U);
    CHECK(SM_GetState() == SM_STATE_FAULT);
    f_gdsPin = 0U;
    CHECK(SM_ClearFault() == 1U);
    CHECK(SM_GetState() == SM_STATE_IDLE);

    Reset();
    f_hrtimTripped = 1U; f_hrtimPin = 1U;
    SM_PollFaults();
    CHECK(SM_ClearFault() == 0U && SM_GetState() == SM_STATE_FAULT);
    f_hrtimPin = 0U;
    CHECK(SM_ClearFault() == 1U && SM_GetState() == SM_STATE_IDLE);

    CHECK(SM_ClearFault() == 0U);                /* nothing to clear */
}

static void TestArmRefusedWithLatchedSource(void)
{
    Reset();
    f_gdsLatched = 1U;                            /* latched, not yet polled */
    CHECK(SM_Arm() == 0U);
    CHECK(SM_GetState() == SM_STATE_IDLE);
    Reset();
    CHECK(SM_Arm() == 1U);
    f_hrtimTripped = 1U;                          /* trips between ARM and FIRE */
    CHECK(SM_Fire() == 0U && f_restarts == 0);
}

static void TestFaultBeatsShotCompletion(void)
{
    /* The fault path leaves the PFM engine STOPPED too; the poll must report
       FAULT, not a completed shot. */
    Reset();
    CHECK(SM_Arm() == 1U);
    CHECK(SM_Fire() == 1U);
    f_gdsLatched = 1U;
    f_pfmState = PFM_STATE_STOPPED;
    SM_PollFaults();
    CHECK(SM_GetState() == SM_STATE_FAULT);
}

static void TestSoftFaultStaysUntilCleared(void)
{
    Reset();
    SM_ReportGeneralFault();
    SM_ReportGeneralFault();                      /* no-op while FAULT */
    SM_PollFaults();
    CHECK(SM_GetState() == SM_STATE_FAULT);
    CHECK(SM_ClearFault() == 1U);
    SM_PollFaults();
    CHECK(SM_GetState() == SM_STATE_IDLE);        /* the soft latch was cleared too */
    CHECK(f_irqMax <= 1);                         /* critical sections never nest */
}

int main(void)
{
    TestNormalShot();
    TestDisarmAndEmptyTable();
    TestFaultFromEachSourceAndState();
    TestClearRefusedWhileConditionPresent();
    TestArmRefusedWithLatchedSource();
    TestFaultBeatsShotCompletion();
    TestSoftFaultStaysUntilCleared();
    return test_report("test_state_machine");
}
