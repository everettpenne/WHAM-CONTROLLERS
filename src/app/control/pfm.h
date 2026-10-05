#ifndef __PFM_H__
#define __PFM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "ctrlr_config.h"
#include <stdint.h>

/*
 * pfm.h / pfm.c
 *
 * Ported from the sibling PFM-STM32G474 project's pfm.c -- the table
 * PLAYBACK engine only, per project decision (2026-08-31):
 *
 *   PORTED:   PFM_Step_t, the table itself, and everything that walks
 *             it and writes HRTIM registers each cycle (PFM_Init(),
 *             PFM_ApplyCurrentStep(), PFM_CycleBoundaryHandler(),
 *             PFM_Restart(), PFM_ResetIndices(), PFM_GetState(),
 *             PFM_GetCurrentStep(), PFM_SetPhaseEnabled()/
 *             PFM_GetPhaseEnabled(), PFM_Phase120()/PFM_Phase240()).
 *
 *   NOT PORTED: the sibling project's SWEEP segment/track builder
 *             (PFM_BuildTable(), PFM_FreqInit/Seg, PFM_DutyInit_x/
 *             Seg_x, the interpolation math) and everything
 *             FEEDBACK-mode related. No on-controller construction
 *             logic exists, and none is planned -- see below.
 *
 *   ADDED (2026-09-04), NOT a port: PFM_TableReset()/PFM_AppendStep()/
 *             PFM_GetEntryCount() -- minimal, generic primitives so a
 *             serial command layer (commands.c's TABle:* commands) can
 *             write a complete table built entirely off-controller, by
 *             a host-side Python script (python/pfm_table_upload.py).
 *             Per project decision: table CONSTRUCTION lives on the
 *             host, not the firmware -- these three functions do no
 *             validation of what a step "means" (no frequency/duty
 *             math, no bounds tied to a supply type, none of that
 *             exists here), only bounds-checking that the table itself
 *             isn't overrun. commands.c's cmd_table_step() owns
 *             whatever validation is appropriate at the protocol
 *             layer. If a real on-controller builder is ever added
 *             later, it would use this exact same API.
 *
 *   GENERALIZED (2026-09-08): PFM_Step_t's per-channel compare values
 *             (previously named cmpA/cmpB/cmpC, hardcoded to 3
 *             channels) are now a `cmp[HRTIM_NUM_CHANNELS]` array
 *             sized by the compile-time channel count in
 *             ctrlr_config.h -- see that file and hrtim.h for the full
 *             explanation. PFM_Phase120()/PFM_Phase240() (3-channel-
 *             only) became PFM_PhaseForChannel() (any channel, any N).
 *             PFM_Phase_t/PFM_SetPhaseEnabled()/PFM_GetPhaseEnabled()
 *             switched from a 3-member enum to a plain 0..N-1 channel
 *             index -- PFM_PHASE_U/V/W remain as named conveniences
 *             for callers that want them, valid whenever N is large
 *             enough.
 *
 * Also not ported: PFM_SetStartupFreq()/Duty()/PulseLength() and their
 * g_startup* state -- those exist in the sibling project purely to feed
 * the table builder (SET FREQ/DUTY/PULSE commands write them, and the
 * builder falls back to them), so they have no purpose here without a
 * builder to feed. Same reasoning for supply_config.h's ACTIVE_*
 * default macros -- this project doesn't have a SUPPLY_TYPE concept at
 * all yet.
 */

/* --------------------------------------------------------------------------
 * Hold periods
 * Number of PWM cycles to hold each table entry before stepping to next
   1 = update every period
   N = update every N periods.
 * -------------------------------------------------------------------------- */
#define PFM_HOLD_PERIODS                         (1U)

/* --------------------------------------------------------------------------
 * Table size: PFM_TABLE_SIZE (ctrlr_config.h, 100 entries by default). The
 * table is a RING of per-period entries -- {per, one compare per channel}, 2 +
 * 2 * HRTIM_NUM_CHANNELS bytes each -- played back by the per-period
 * interrupt and kept filled by a lower-priority one (PFM_RefillHandler()),
 * so a shot may be far longer than the table (a 1 s shot at 20 kHz is
 * 20,000 periods). Source of the entries (PFM_SelectSource()):
 *   PFM_SOURCE_PROFILE  the time-series shot (shot_profile.h) walked by the
 *                       generator, refilled during the shot -- the normal way
 *   PFM_SOURCE_TABLE    a table uploaded entry by entry (TABle:*), at most
 *                       PFM_TABLE_SIZE entries, played once -- bench use
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * PFM_Step_t -- one playback entry. `cmp[]` holds one compare value per
 * active channel (HRTIM_NUM_CHANNELS of them, see ctrlr_config.h) --
 * sized exactly for whatever channel count this build is configured
 * for, no waste. See the sibling project's pfm.h for the RAM-budget
 * history behind the original 3-channel layout this generalizes
 * (phaseB/phaseC/freqHz were removed there in favor of recomputing
 * them from `per` at apply time; the same principle applies here --
 * phase is still never stored, only per-channel duty).
 * -------------------------------------------------------------------------- */
typedef struct
{
    uint16_t per;
    uint16_t cmp[HRTIM_NUM_CHANNELS];
} PFM_Step_t;

/* Pure function of `per` and a channel index: the Master-timer phase
 * offset (in HRTIM counts) for that channel at this period, evenly
 * spaced across HRTIM_NUM_CHANNELS channels. `channel` 0 has no phase
 * register of its own (it's the Master-PER reference channel, 0 deg by
 * definition) -- only call this for `channel` in 1..HRTIM_NUM_CHANNELS-1. */
uint16_t PFM_PhaseForChannel(uint16_t per, uint8_t channel);

typedef enum
{
    PFM_STATE_RUNNING = 0U,
    PFM_STATE_STOPPED
} PFM_State_t;

/* Named conveniences for the first three channels -- still meaningful
 * whenever HRTIM_NUM_CHANNELS >= 3 (true for every profile this
 * project has used so far). PFM_SetPhaseEnabled()/PFM_GetPhaseEnabled()
 * take a plain uint8_t channel index (0..HRTIM_NUM_CHANNELS-1), not
 * this enum type, so callers with more than 3 channels configured can
 * still address channel 3, 4, etc. directly by number. */
typedef enum
{
    PFM_PHASE_U = 0U,
    PFM_PHASE_V,
    PFM_PHASE_W
} PFM_Phase_t;

typedef enum
{
    PFM_SOURCE_TABLE = 0U,
    PFM_SOURCE_PROFILE,
    PFM_SOURCE_LIVE       /* closed loop: the controller hands over the next {per, cmp} every tick */
} PFM_Source_t;

/* Chooses where the next shot's entries come from. Called when an upload
   completes (SHOT:END or TABle:END); never while FIRING. */
void PFM_SelectSource(PFM_Source_t source);
PFM_Source_t PFM_GetSource(void);

/* Closed loop (control_runtime.c). PFM_SOURCE_LIVE bypasses the ring: the per-period interrupt
   applies the LAST {per, cmp} given to PFM_LiveSet() (one atomic 32-bit store, so the interrupt
   never sees a half-updated pair; the same compare goes to every phase). PFM_LiveSet() before the
   shot sets the opening step; during it, the controller calls it once per control tick. */
void PFM_LiveSet(uint16_t per, uint16_t cmp);

/* Ends a live shot at the next period boundary, a coherent stop like the end of a table. */
void PFM_LiveRequestStop(void);

/* Sources: selecting PFM_SOURCE_LIVE remembers the previous open-loop source; this restores it
   (an open-loop FIRE after a closed-loop run must play the open-loop shot again). */
void PFM_RestoreOpenSource(void);

/* 1 if the selected source holds something to fire (a complete time series,
   or at least one table entry). */
uint8_t PFM_ShotReady(void);

/* Body of the refill interrupt (Core/Src/stm32g4xx_it.c FMAC_IRQHandler()):
   tops the ring up from the generator. Priority 2, so the per-period
   interrupt (1) preempts it; it gets the CPU the per-period interrupt leaves. */
void PFM_RefillHandler(void);

/* The ring ran empty while the shot was not over: the refill fell behind the
   playback. The shot is stopped and this latches until PFM_ClearUnderrun()
   (FAULT:CLEAR); the state machine treats it as a fault (state_machine.c). */
uint8_t PFM_UnderrunLatched(void);
void    PFM_ClearUnderrun(void);

/* Ring diagnostics of the last shot: the fewest entries ready at any period
   (0 would be an underrun), the number of refill interrupts that ran, and the
   entries generated, and how long the refill runs took in wall-clock cycles
   (summed, and the longest one -- they include the time the per-period
   interrupt preempted them). minReady is counted only while the generator is
   still running (0xFFFFFFFF if it never was: a shot shorter than the ring). */
void PFM_GetDiagRing(uint32_t *minReady, uint32_t *refills, uint32_t *produced,
                     uint32_t *refillWallCycles, uint32_t *refillWallMaxCycles, uint32_t *prefillCycles);

void PFM_Init(void);
void PFM_ApplyCurrentStep(void);
void PFM_CycleBoundaryHandler(void);
void PFM_Restart(void);          /* re-arm from external trigger: resets indices, applies step 0, starts outputs */
void PFM_ResetIndices(void);     /* quiescent index reset only, no register/output writes (e.g. fault clear) */
PFM_State_t PFM_GetState(void);  /* optional, for debug/status */

/* Immediately stops HRTIM output (HRTIM1_PWM_Stop()) and marks
 * PFM_GetState() STOPPED -- the exact pair of actions
 * PFM_CycleBoundaryHandler() already performs, from ISR context, when
 * it notices a latched HRTIM1_FLT6 fault (hrtim.h) or an empty table.
 * Extracted as its own function (2026-09-08) so any OTHER fault source
 * -- e.g. gate_driver.c's GateDriver_CheckFault(), called from the
 * GateDriverStatus EXTI interrupts, which run independently of the
 * HRTIM master-repetition ISR -- can force the same clean stop and
 * keep PFM_GetState() truthful, instead of calling HRTIM1_PWM_Stop()
 * directly and leaving g_pfmState stale at RUNNING. Does NOT reset
 * g_pfmIndex (matching the two existing early-stop call sites inside
 * PFM_CycleBoundaryHandler(), which never did either) and does NOT
 * clear any fault latch itself -- callers own their own latch. Safe to
 * call from ISR context; safe to call repeatedly (HRTIM1_PWM_Stop() is
 * idempotent). */
void PFM_ForceStop(void);

/* Per-channel output enable -- standalone, mode-independent, durable
 * state. Not reset by PFM_Init() or PFM_ResetIndices() -- only changes
 * via an explicit call to PFM_SetPhaseEnabled(). Defaults to all
 * enabled. `channel` out of range (>= HRTIM_NUM_CHANNELS) is a no-op
 * for Set and returns 0 for Get. */
void PFM_SetPhaseEnabled(uint8_t channel, uint8_t enabled);
uint8_t PFM_GetPhaseEnabled(uint8_t channel);

/* The same state as a bit mask (bit k = channel k = phase U, V, W...): all phases at once. The mask is
   read when a shot starts (PFM_Restart()), so a change applies from the NEXT start -- cmd_phase.c only
   allows it while IDLE. */
uint8_t PFM_GetPhaseMask(void);
void    PFM_SetPhaseMask(uint8_t mask);

const PFM_Step_t *PFM_GetCurrentStep(void);
const PFM_Step_t *PFM_GetStepByIndex(uint16_t index);

/* --------------------------------------------------------------------------
 * Table upload primitives -- see the ADDED note in this file's header
 * comment. Generic on purpose: no notion of "upload session" state
 * (whether a TABLE:BEGIN was sent) lives here -- that's commands.c's
 * job, matching how the rest of this codebase keeps protocol-layer
 * state (e.g. cmd_boot()'s checks) out of the modules it calls into.
 * -------------------------------------------------------------------------- */

/* Empties the table (g_pfmEntryCount = 0) without touching HRTIM or
 * PFM_GetState() -- purely a data-structure reset, safe to call at any
 * time. Does NOT reset g_pfmIndex/g_holdCounter (PFM_ResetIndices()
 * already does that, separately, for the fault-clear/quiescent case;
 * this function is deliberately narrower). */
void PFM_TableReset(void);

/* Appends one entry at g_pfmTable[g_pfmEntryCount], then increments
 * g_pfmEntryCount. `cmp` must point to exactly HRTIM_NUM_CHANNELS
 * values -- the caller (commands.c's cmd_table_step()) is responsible
 * for having parsed that many. Returns 1 on success, 0 if the table is
 * already at PFM_TABLE_SIZE capacity (entry NOT written in that case --
 * the caller's count of "how many actually got in" should stop
 * advancing on a 0 return, not silently keep calling). No
 * range-checking on the values themselves (per/cmp accepted as given --
 * the eventual apply-time HRTIM1_ClampCompare() in hrtim.c is the last
 * line of defense there, but callers should validate before this, not
 * rely on that clamp as anything other than a backstop). */
uint8_t PFM_AppendStep(uint16_t per, const uint16_t *cmp);

/* Current g_pfmEntryCount -- how many entries PFM_AppendStep() has
 * actually written since the last PFM_TableReset(). */
uint16_t PFM_GetEntryCount(void);

/* Diagnostic, added 2026-09-09 (see pfm.c's own comment on
 * g_diagMaxGapCycles) -- real inter-call timing for
 * PFM_CycleBoundaryHandler() during the CURRENT/most recent shot,
 * reset at every PFM_Restart(). `*callCount` is how many times the
 * handler has run since that reset; `*maxGapCycles` is the largest
 * gap seen between two consecutive calls, in CPU cycles (170 MHz,
 * same clock as HRTIM_TIMER_CLK_HZ -- hrtim.h -- so directly
 * comparable to HRTIM tick counts). A gap far larger than one real
 * period's worth of cycles is direct evidence the ISR was starved for
 * that stretch, not just that the table happened to hold one entry
 * for a while. Either pointer may be NULL to skip that output. */
void PFM_GetDiagCounters(uint32_t *callCount, uint32_t *maxGapCycles);

/* Worst-case and total time (CPU cycles, 170 MHz) spent inside PFM_CycleBoundaryHandler()
   this shot -- total / (calls x period) is the CPU share the per-period interrupt uses. */
void PFM_GetDiagBusy(uint32_t *maxCycles, uint32_t *sumCycles);

/* Diagnostic, added 2026-09-09 (second round -- see pfm.c's own
 * comment on g_diagGapLog): the RAW inter-call gap, in CPU cycles, for
 * every PFM_CycleBoundaryHandler() call this shot, up to
 * PFM_INPUT_MAX_PERIODS (pfm_input.h) of them -- unlike
 * PFM_GetDiagCounters()'s single max-gap number, this lets a caller
 * see EXACTLY which table entries had delays, not just whether the
 * single worst one was large. Returns a pointer to the internal log
 * array (valid until the next PFM_Restart()) and writes the number of
 * valid entries in it (== min(call count, PFM_INPUT_MAX_PERIODS)) to
 * *count if non-NULL. */
const uint32_t *PFM_GetDiagGapLog(uint16_t *count);

#ifdef __cplusplus
}
#endif

#endif /* __PFM_H__ */
