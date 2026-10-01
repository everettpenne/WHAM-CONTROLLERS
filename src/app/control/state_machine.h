#ifndef __STATE_MACHINE_H__
#define __STATE_MACHINE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * state_machine.h / state_machine.c -- the top-level operating state,
 * added 2026-09-29 (Phase 3 of the port from WHAM-XREX-PFMG474, whose
 * state_machine.h this mirrors: same names, same states, a subset of its
 * features).
 *
 *   IDLE  --ARM-->  ARMED  --FIRE-->  FIRING  --table ends-->  IDLE
 *     ^               |                  |
 *     +----DISARM-----+                  |
 *   any state --fault--> FAULT --FAULT:CLEAR (sources clear)--> IDLE
 *
 * - ARM is refused unless IDLE and no fault source is latched. Nothing
 *   electrical happens on ARM.
 * - FIRE (SM_Fire()) is refused unless ARMED and the table is non-empty;
 *   it starts table playback (PFM_Restart()) and enters FIRING. When the
 *   table is exhausted the PFM engine stops the output by itself and the
 *   next SM_PollFaults() returns to IDLE -- every shot needs a fresh ARM.
 * - ONE fault type (SM_FAULT_GENERAL), by design for this product: every
 *   source -- PC10/HRTIM1_FLT6 (native HRTIM fault input) and the 12
 *   GateDriverStatus inputs (gate_driver.h) -- lands in the same FAULT
 *   state. The response is an immediate force-stop of the output
 *   (PFM_ForceStop()), no ramp-down. The output is already safe before
 *   this module even sees the fault: PC10 gates the HRTIM outputs in
 *   hardware, and GateDriver_CheckFault() force-stops from the EXTI
 *   interrupt; this module makes the state say so and keeps it latched.
 * - FAULT:CLEAR (SM_ClearFault()) clears both sources, which re-check the
 *   physical inputs; if either is still bad the fault re-latches and the
 *   state stays FAULT. Otherwise IDLE -- never straight back to ARMED.
 *
 * Not ported from XREX (not part of this product, or deferred): the PID
 * ramp-downs, per-channel fault types, the external enable/trigger inputs
 * (PF13/PF15), DEBUG:FAULT:BYPASS and telemetry events.
 */

typedef enum
{
    SM_STATE_IDLE = 0,
    SM_STATE_ARMED,
    SM_STATE_FIRING,
    SM_STATE_FAULT
} SM_State_t;

typedef enum
{
    SM_FAULT_NONE = 0,     /* reported by SM_GetFaultType() whenever not in FAULT */
    SM_FAULT_GENERAL       /* every fault source (see this file's header) */
} SM_FaultType_t;

/* Brings the state to IDLE with no fault. Called once from App_Init(),
   after the fault sources' own init and boot-time check, so the first
   SM_PollFaults() picks up a fault that was already present at boot. */
void SM_Init(void);

SM_State_t SM_GetState(void);
SM_FaultType_t SM_GetFaultType(void);

/* IDLE -> ARMED. Returns 1 on success, 0 if not IDLE or a fault source is
   latched (state unchanged). */
uint8_t SM_Arm(void);

/* ARMED -> IDLE. A no-op in any other state. */
void SM_Disarm(void);

/* ARMED -> FIRING: starts table playback. Returns 1 on success, 0 if not
   ARMED, a fault is latched, or the table is empty (state unchanged). */
uint8_t SM_Fire(void);

/* FIRING -> IDLE, called by SM_PollFaults() once the PFM engine has
   stopped by itself at the end of the table. A no-op in any other
   state. */
void SM_NotifyShotComplete(void);

/* Main-loop poll (task_faults.c): enters FAULT if either fault source is
   latched, otherwise ends a finished shot. Runs with interrupts masked so
   the check and the transition are one step. */
void SM_PollFaults(void);

/* Enters FAULT from software (GENERAL:TEST:FAULT, a bench test of the
   fault path). A no-op if already in FAULT. */
void SM_ReportGeneralFault(void);

/* Which sources currently hold a fault latched, as a bit mask -- the cause behind
   the single FAULT GENERAL state (FAULT:SOURces?). */
#define SM_FAULT_SRC_PC10       (0x01U)   /* PC10 / HRTIM1_FLT6 hardware fault input */
#define SM_FAULT_SRC_GATEDRV    (0x02U)   /* a GateDriverStatus input */
#define SM_FAULT_SRC_UNDERRUN   (0x04U)   /* the per-period table ran empty (pfm.h) */
#define SM_FAULT_SRC_TEST       (0x08U)   /* GENERAL:TEST:FAULT */
uint8_t SM_GetFaultSources(void);

/* FAULT -> IDLE if both fault sources clear (each re-checks its physical
   input). Returns 1 if the state is now IDLE, 0 if it stays FAULT or was
   not in FAULT. */
uint8_t SM_ClearFault(void);

#ifdef __cplusplus
}
#endif

#endif /* __STATE_MACHINE_H__ */
