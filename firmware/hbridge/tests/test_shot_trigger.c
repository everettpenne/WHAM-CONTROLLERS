/*
 * test_shot_trigger.c -- the shot start (src/app/control/shot_start.c) and the external trigger
 * (src/app/control/shot_trigger.c) on the host, with the state machine, PFM engine, control runtime, fault inputs,
 * the PF15 pin and the software interrupt faked.
 *
 * The properties that matter, each checked below:
 *   - FIRE and the trigger run the SAME start (one routine, ShotStart_Run) with the same refusals, in the same order;
 *   - an edge starts a shot only when the trigger is enabled, the controller is ARMED, and the pin is still HIGH;
 *   - the start is done by the pended software interrupt, never inside the edge handler;
 *   - a refused start leaves the controller ARMED and a new edge is needed;
 *   - two starts can never run at once: a trigger arriving while a FIRE is being processed (the interrupt
 *     preempting the main loop) is refused as BUSY, and the claim is released afterwards.
 */
#include "shot_trigger.h"
#include "shot_start.h"
#include "state_machine.h"
#include "pfm.h"
#include "hrtim.h"
#include "gate_driver.h"
#include "board_io.h"
#include "mcu.h"
#include "ctrlr_config.h"
#include "test_util.h"
#include <string.h>

/* ---- fakes ------------------------------------------------------------- */
static SM_State_t f_state;
static uint8_t    f_fireWorks, f_shotReady, f_closed, f_hrtimTripped, f_gdsLatched, f_underrun, f_srcMask, f_pin;
static ctl_result_t f_prepareResult;
static int  f_fires, f_prepares, f_goes, f_stops, f_restoreSrc, f_irqDepth, f_startIrqInit, f_startIrqPends;
static void (*f_inPrepare)(void);          /* runs inside Control_Prepare: stands for an interrupt preempting the caller */

SM_State_t SM_GetState(void)         { return f_state; }
uint8_t SM_GetFaultSources(void)     { return f_srcMask; }
uint8_t SM_Fire(void)
{
    f_fires++;
    if (f_state != SM_STATE_ARMED || f_fireWorks == 0U) { return 0U; }
    f_state = SM_STATE_FIRING;
    return 1U;
}
uint8_t HRTIM1_FaultIsTripped(void)  { return f_hrtimTripped; }
uint8_t GateDriver_FaultIsLatched(void) { return f_gdsLatched; }
uint8_t PFM_UnderrunLatched(void)    { return f_underrun; }
void    PFM_RestoreOpenSource(void)  { f_restoreSrc++; }
uint8_t PFM_ShotReady(void)          { return f_shotReady; }
uint8_t Control_ClosedRunWanted(void){ return f_closed; }
ctl_result_t Control_Prepare(uint8_t useProfile)
{
    (void)useProfile;
    f_prepares++;
    if (f_inPrepare != NULL) { void (*cb)(void) = f_inPrepare; f_inPrepare = NULL; cb(); }
    return f_prepareResult;
}
void Control_Go(void)                { f_goes++; }
void Control_Stop(void)              { f_stops++; }
void Mcu_IrqDisable(void)            { f_irqDepth++; }
void Mcu_IrqEnable(void)             { f_irqDepth--; }
void Mcu_StartIrqInit(void)          { f_startIrqInit++; }
void Mcu_StartIrqPend(void)          { f_startIrqPends++; }
uint8_t BoardIo_ReadExtTrigger(void) { return f_pin; }

static void Reset(void)
{
    f_state = SM_STATE_ARMED;
    f_fireWorks = 1U; f_shotReady = 1U; f_closed = 0U; f_hrtimTripped = f_gdsLatched = f_underrun = f_srcMask = 0U;
    f_pin = 1U;
    f_prepareResult = CTL_OK;
    f_fires = f_prepares = f_goes = f_stops = f_restoreSrc = f_irqDepth = f_startIrqPends = 0;
    f_inPrepare = NULL;
    ShotTrigger_SetEnabled(1U);
}

static shot_trigger_status_t Status(void)
{
    shot_trigger_status_t s;
    ShotTrigger_GetStatus(&s);
    return s;
}

/* ---- ShotStart_Run: the one start behind FIRE and the trigger -------------- */
static void TestStartOrderAndResults(void)
{
    shot_start_result_t r;

    Reset();                                       /* open loop, ARMED, shot ready: starts */
    r = ShotStart_Run();
    CHECK(r.code == SHOT_START_OK && f_state == SM_STATE_FIRING && f_fires == 1 && f_restoreSrc == 1);
    CHECK(f_prepares == 0 && f_goes == 0);         /* the open-loop path does not touch the control runtime */

    Reset(); f_hrtimTripped = 1U;                  /* each fault source refuses first, whatever else is wrong */
    f_state = SM_STATE_IDLE;
    CHECK(ShotStart_Run().code == SHOT_START_FAULT);
    Reset(); f_gdsLatched = 1U;   CHECK(ShotStart_Run().code == SHOT_START_FAULT);
    Reset(); f_underrun = 1U;     CHECK(ShotStart_Run().code == SHOT_START_FAULT);
    Reset(); f_srcMask = SM_FAULT_SRC_OVERVOLT;  CHECK(ShotStart_Run().code == SHOT_START_FAULT);
    Reset(); f_srcMask = SM_FAULT_SRC_FEEDBACK;  CHECK(ShotStart_Run().code == SHOT_START_FAULT);
    Reset(); f_srcMask = SM_FAULT_SRC_PC10;      CHECK(ShotStart_Run().code == SHOT_START_OK);   /* PC10/test bits are SM's own state, not read here */
    CHECK(f_fires == 1);

    Reset(); f_state = SM_STATE_IDLE;  CHECK(ShotStart_Run().code == SHOT_START_NOT_ARMED);
    Reset(); f_state = SM_STATE_FIRING; CHECK(ShotStart_Run().code == SHOT_START_NOT_ARMED);
    Reset(); f_state = SM_STATE_FAULT; CHECK(ShotStart_Run().code == SHOT_START_NOT_ARMED);
    CHECK(f_fires == 0);

    Reset(); f_shotReady = 0U;                     /* nothing to play: refused, SM_Fire not even tried */
    CHECK(ShotStart_Run().code == SHOT_START_NO_SHOT && f_fires == 0 && f_state == SM_STATE_ARMED);

    Reset(); f_fireWorks = 0U;                     /* the state changed under us */
    CHECK(ShotStart_Run().code == SHOT_START_CHANGED && f_state == SM_STATE_ARMED);
    CHECK(f_irqDepth == 0);
}

static void TestClosedLoop(void)
{
    shot_start_result_t r;

    Reset(); f_closed = 1U;
    r = ShotStart_Run();
    CHECK(r.code == SHOT_START_OK && f_prepares == 1 && f_fires == 1 && f_goes == 1 && f_stops == 0);
    CHECK(f_restoreSrc == 0);                      /* a closed-loop run does not touch the open-loop source */

    Reset(); f_closed = 1U; f_prepareResult = CTL_NO_TARGET;
    r = ShotStart_Run();
    CHECK(r.code == SHOT_START_CONTROL && r.ctl == CTL_NO_TARGET);
    CHECK(f_fires == 0 && f_goes == 0 && f_state == SM_STATE_ARMED);

    Reset(); f_closed = 1U; f_fireWorks = 0U;      /* prepared, but FIRING could not be claimed: the loops are stopped again */
    r = ShotStart_Run();
    CHECK(r.code == SHOT_START_CHANGED && f_goes == 0 && f_stops == 1);
    CHECK(f_irqDepth == 0);
}

static shot_start_result_t g_nested;
static void NestedRecord(void) { g_nested = ShotStart_Run(); }

static void TestOnlyOneStartAtATime(void)
{
    shot_start_result_t r;

    Reset(); f_closed = 1U;
    f_inPrepare = NestedRecord;                    /* an interrupt preempts the first start inside Control_Prepare */
    r = ShotStart_Run();
    CHECK(g_nested.code == SHOT_START_BUSY);       /* the nested one must not start anything */
    CHECK(r.code == SHOT_START_OK && f_fires == 1 && f_goes == 1 && f_prepares == 1);
    CHECK(ShotStart_Run().code == SHOT_START_NOT_ARMED);   /* the claim was released (a refusal, not BUSY) */
    CHECK(f_irqDepth == 0);
}

/* ---- the trigger -------------------------------------------------------- */
static void TestDefaultsAndInit(void)
{
    ShotTrigger_Init();
    CHECK(f_startIrqInit == 1);
    CHECK(ShotTrigger_GetEnabled() == ((SHOT_TRIGGER_DEFAULT_ENABLED != 0U) ? 1U : 0U));
    ShotTrigger_SetEnabled(0U);  CHECK(ShotTrigger_GetEnabled() == 0U);
    ShotTrigger_SetEnabled(7U);  CHECK(ShotTrigger_GetEnabled() == 1U);
}

static void TestEdgeStartsAShotOnlyWhenArmedEnabledAndHigh(void)
{
    shot_trigger_status_t s0, s;

    Reset();
    s0 = Status();

    ShotTrigger_SetEnabled(0U);                    /* off */
    ShotTrigger_OnEdge();
    s = Status();
    CHECK(s.edges == s0.edges + 1 && s.ignored_disabled == s0.ignored_disabled + 1 && f_startIrqPends == 0);
    ShotTrigger_RunPending();                      /* nothing was pended: nothing happens */
    CHECK(f_state == SM_STATE_ARMED && f_fires == 0);

    ShotTrigger_SetEnabled(1U);
    f_state = SM_STATE_IDLE; ShotTrigger_OnEdge();      /* not ARMED */
    f_state = SM_STATE_FIRING; ShotTrigger_OnEdge();
    f_state = SM_STATE_FAULT; ShotTrigger_OnEdge();
    s = Status();
    CHECK(s.ignored_state == s0.ignored_state + 3 && f_startIrqPends == 0);

    f_state = SM_STATE_ARMED; f_pin = 0U;               /* a glitch: the pin is LOW again by the time we look */
    ShotTrigger_OnEdge();
    s = Status();
    CHECK(s.ignored_glitch == s0.ignored_glitch + 1 && f_startIrqPends == 0 && f_state == SM_STATE_ARMED);

    f_pin = 1U;                                         /* a real edge */
    ShotTrigger_OnEdge();
    CHECK(f_startIrqPends == 1);
    CHECK(f_state == SM_STATE_ARMED && f_fires == 0);   /* the edge handler itself starts NOTHING */
    ShotTrigger_RunPending();                           /* the software interrupt does */
    s = Status();
    CHECK(f_state == SM_STATE_FIRING && f_fires == 1);
    CHECK(s.started == s0.started + 1 && s.attempts == s0.attempts + 1 && s.last.code == SHOT_START_OK);
    CHECK(s.edges == s0.edges + 6);

    ShotTrigger_RunPending();                           /* run again with no new edge: nothing more */
    CHECK(f_fires == 1);
    ShotTrigger_OnEdge();                               /* edge while FIRING: ignored */
    CHECK(f_startIrqPends == 1);
}

static void TestRefusedStartLeavesItArmedAndNeedsANewEdge(void)
{
    shot_trigger_status_t s0, s;

    Reset(); f_shotReady = 0U;
    s0 = Status();
    ShotTrigger_OnEdge(); ShotTrigger_RunPending();
    s = Status();
    CHECK(f_state == SM_STATE_ARMED && f_fires == 0);
    CHECK(s.refused == s0.refused + 1 && s.last.code == SHOT_START_NO_SHOT && s.started == s0.started);
    ShotTrigger_RunPending();                           /* the level staying HIGH is not a new edge */
    CHECK(Status().attempts == s.attempts);
    f_shotReady = 1U;                                   /* upload the shot, then a fresh edge */
    ShotTrigger_OnEdge(); ShotTrigger_RunPending();
    CHECK(f_state == SM_STATE_FIRING && Status().started == s0.started + 1);

    Reset(); f_closed = 1U; f_prepareResult = CTL_REF_BAD;   /* a closed-loop refusal is recorded with its reason */
    ShotTrigger_OnEdge(); ShotTrigger_RunPending();
    s = Status();
    CHECK(f_state == SM_STATE_ARMED && s.last.code == SHOT_START_CONTROL && s.last.ctl == CTL_REF_BAD);

    Reset(); f_srcMask = SM_FAULT_SRC_OVERVOLT;         /* a latched fault refuses the trigger too */
    ShotTrigger_OnEdge(); ShotTrigger_RunPending();
    CHECK(Status().last.code == SHOT_START_FAULT && f_fires == 0);
}

static void RunTriggerFromInterrupt(void) { ShotTrigger_OnEdge(); ShotTrigger_RunPending(); }

static void TestTriggerDuringAFireCommandIsRefusedAsBusy(void)
{
    shot_trigger_status_t s0, s;
    shot_start_result_t r;

    Reset(); f_closed = 1U;
    s0 = Status();
    /* the main loop is inside FIRE -> ShotStart_Run -> Control_Prepare when the edge interrupts it: the interrupt
       runs the real trigger path (edge handler, then the software interrupt) */
    f_inPrepare = RunTriggerFromInterrupt;
    r = ShotStart_Run();                                /* the FIRE command */
    s = Status();
    CHECK(r.code == SHOT_START_OK && f_fires == 1 && f_goes == 1);      /* FIRE started the one shot */
    CHECK(s.refused == s0.refused + 1 && s.busy == s0.busy + 1 && s.last.code == SHOT_START_BUSY);
    CHECK(s.started == s0.started);                                      /* the trigger started nothing */
}

int main(void)
{
    TestDefaultsAndInit();
    TestStartOrderAndResults();
    TestClosedLoop();
    TestOnlyOneStartAtATime();
    TestEdgeStartsAShotOnlyWhenArmedEnabledAndHigh();
    TestRefusedStartLeavesItArmedAndNeedsANewEdge();
    TestTriggerDuringAFireCommandIsRefusedAsBusy();
    return test_report("test_shot_trigger");
}
