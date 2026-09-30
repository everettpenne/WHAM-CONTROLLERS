#ifndef INC_PFM_INPUT_H_
#define INC_PFM_INPUT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * pfm_input.h
 *
 * Per-period PERIOD measurement (rising-edge-to-rising-edge only, no
 * duty cycle) on the 6 PFM_Input_01..06 pins (docs/pin_mapping_v4.csv
 * -- PA15/TIM2_CH1, PD4/TIM2_CH2, PB2/TIM5_CH1, PC12/TIM5_CH2,
 * PB4/TIM3_CH1, PD12/TIM4_CH1), added 2026-09-08. Each channel is
 * measured completely independently -- different PFM_Input pins can
 * be watching entirely unrelated signals -- via plain
 * TIM_ICPOLARITY_RISING input capture, armed once at init and never
 * changed again (see pfm_input.c).
 *
 * DECISION, 2026-09-08: this module originally also measured duty
 * cycle (time-high per period), via TIM_ICPOLARITY_BOTHEDGE and later
 * an alternating-single-polarity technique -- both real-hardware
 * attempts consistently produced data offset by exactly one real
 * period, for reasons never root-caused (see pfm_input.c's top
 * comment and docs/changelog.txt for the full diagnostic history).
 * Duty-cycle measurement was dropped rather than continue chasing that
 * bug: period-only, rising-edge-only capture never changes polarity
 * inside the ISR at all, which removes the one mechanism every failed
 * attempt had in common.
 *
 * Capture is synchronized to PFM shot lifetime, not a standalone
 * blocking command: PfmInput_Arm() only sets up a target period count
 * per channel; the actual hardware capture starts inside
 * PFM_Restart() (pfm.c) -- the same function that starts HRTIM output
 * -- via PfmInput_OnShotStart(), and stops (with whatever partial
 * count was reached) wherever pfm.c stops a shot, via
 * PfmInput_OnShotEnd(). A capture can therefore never run longer than
 * the shot that started it -- see AGENTS.md/docs/changelog.txt for the
 * full design writeup, including what got corrected from this
 * feature's first draft plan (originally a blocking PFMIN:CAPTURE with
 * no tie to FIRE at all).
 *
 * This module is self-contained and removable: PFM_INPUT_FEATURE_ENABLED
 * below is the single point of control, matching boot_jump.h/
 * qspi_test.h's established pattern. Disabled: every function below
 * becomes a no-op/returns-nothing (see pfm_input.c's #else branch);
 * pfm.c's calls into PfmInput_OnShotStart()/OnShotEnd() stay
 * unconditional either way (always resolve to something or nothing),
 * same as every other removable module's call sites in this project.
 */

#ifndef PFM_INPUT_FEATURE_ENABLED
#define PFM_INPUT_FEATURE_ENABLED (1)
#endif

/* One entry per PFM_Input_01..06 -- channel index 0..5 = PFM_Input_01..06. */
#define PFM_INPUT_NUM_CHANNELS  (6U)

/* --------------------------------------------------------------------------
 * Active-channel mask (compile-time, hard limit)
 *
 * Added 2026-09-09, alongside the frequency-ramp capture investigation
 * (see docs/changelog.txt): only PFM_Input_01 (bit 0, PA15/TIM2_CH1)
 * is physically wired to a real signal (phase U) on this bench right
 * now. The other 5 channels were being fully initialized and armed on
 * every run for no benefit -- 3 extra timer peripherals (TIM3/TIM4/
 * TIM5) clocked and interrupt-enabled, 2 extra channels configured on
 * TIM2, all doing real (if idle) ISR/NVIC work -- adding CPU/interrupt
 * load for zero signal, and load is exactly what's under suspicion in
 * the still-open frequency-transition investigation (see
 * ctrlr_config.h's PFM_MAX_CARRIER_FREQ_HZ comment). Disabled here to
 * both save that compute and shrink the variables in play while that
 * investigation continues.
 *
 * Bit i (0-5) = PFM_Input_0(i+1) active. A timer with NO active
 * channel is left completely untouched by PfmInput_Init() -- no
 * HAL_TIM_IC_Init(), no clock enable, no NVIC IRQ enable, no GPIO AF
 * config -- not just "armed with M=0"; see pfm_input.c. Re-enable a
 * channel by setting its bit here; nothing else needs to change.
 *
 * Set to 0x3F (all 6 channels), 2026-09-09, step 6 (final) of the
 * DMA-based capture mechanism's incremental re-test (see this file's
 * own top comment and pfm_input.c's capture-technique history for the
 * full story). Steps 1-5 all confirmed clean, PFM:DIAG? max gap
 * growing gently and roughly linearly with channel count (about 17,
 * 42, 57, 74, 91 us) -- nowhere near the old interrupt-driven design's
 * 734 us stall, which broke at just 2 channels. This is the same
 * all-6 configuration that caused a real board lockup under the old
 * design. */
#define PFM_INPUT_ACTIVE_CHANNEL_MASK  (0x3FU)  /* all 6 channels */

/* Compile-time cap on periods storable per channel per run, sizing the
 * RAM arrays (6 x this x 2 x 4 bytes) -- see pfm_input.c. 200 is 10x
 * the example M=20 in the original request; cheap against this MCU's
 * 128 KiB SRAM and this project's existing 40 KB g_pfmTable[]
 * (pfm.c). Raise if a real need for more periods per run shows up. */
#define PFM_INPUT_MAX_PERIODS   (200U)

/* Called once at boot (main.c, alongside the other peripheral bring-up
 * calls) -- configures rising-edge input capture, GPIO AF pins, AND
 * (2026-09-09) the DMA channel that streams captured ticks into RAM
 * with no per-edge CPU involvement, for every channel enabled in
 * PFM_INPUT_ACTIVE_CHANNEL_MASK (above); a timer with no active
 * channel is left completely uninitialized (no clock, no NVIC, no
 * GPIO, no DMA) rather than merely unarmed. Does not arm or start any
 * capture. No-op when PFM_INPUT_FEATURE_ENABLED is 0. */
void PfmInput_Init(void);

/* Arms every ACTIVE channel (PFM_INPUT_ACTIVE_CHANNEL_MASK, above) to
 * capture up to `m` periods each on the NEXT shot (PFM_Restart(),
 * pfm.c) -- pure bookkeeping, touches no TIMx hardware, safe to call
 * whether or not a shot is currently running. An inactive channel is
 * left permanently at count 0, whatever `m` is passed. Single-shot:
 * consumed by the next PfmInput_OnShotStart() call, not sticky across
 * multiple shots -- arm again before every FIRE that should capture.
 * `m` is clamped to PFM_INPUT_MAX_PERIODS. No-op when disabled. */
void PfmInput_Arm(uint16_t m);

/* Called from PFM_Restart() (pfm.c), immediately BEFORE HRTIM1_PWM_Start()
 * -- starts HAL_TIM_IC_Start_DMA() for every armed channel (see
 * PfmInput_Arm()): a bounded, non-circular DMA fill of that channel's
 * result array, no per-edge or per-chunk CPU work. Started before the
 * outputs so nothing is missed (enabling four channels takes tens of
 * microseconds with the per-period interrupt running), which means the
 * capture includes the shot's start-up period. A channel never armed is
 * left untouched. No-op when disabled. */
void PfmInput_OnShotStart(void);

/* Called from every place pfm.c stops a shot (PFM_ForceStop() and the
 * normal end-of-table-exhaustion path in PFM_CycleBoundaryHandler()) --
 * for any channel still capturing, records how many edges the DMA
 * stored (requested minus the DMA's remaining count) and stops it.
 * Bounds every capture's runtime to the shot that started it; a channel
 * whose DMA already filled its array (and stopped itself) is a no-op
 * here. No-op when disabled. */
void PfmInput_OnShotEnd(void);

/* Current captured-period count for `channel` (0..5), valid whether or
 * not a capture is armed/running/finished. Returns 0 for an
 * out-of-range channel or when disabled. */
uint16_t PfmInput_GetCount(uint8_t channel);

/* Raw tick array for `channel` (0..5) -- period[i] (rising-to-rising,
 * in timer ticks) for i = 0..PfmInput_GetCount(channel)-1, in capture
 * order. Returns NULL for an out-of-range channel or when disabled; a
 * valid channel always returns a non-NULL pointer (into a fixed static
 * array), regardless of how many entries are actually valid --
 * callers must use PfmInput_GetCount() to know how many entries to
 * read. */
const uint32_t *PfmInput_GetPeriods(uint8_t channel);

/* Diagnostic: 1 if a hardware overcapture (CCxOF) was seen for `channel`
 * while its DMA was still filling the result array -- a later edge
 * arrived before the DMA had read the previous one, so an edge may be
 * missing or overwritten. Reported ONLY for a capture the end of the shot
 * cut short (fewer edges than requested): once the array is full nothing
 * reads the register and the flag is set by the signal carrying on, so it
 * is ignored then. Not a reliable loss detector (2026-09-30: flagged on
 * about 1 in 10 shot-end captures whose data was then clean) -- check the
 * data itself (periods that add up, alternating edges). Returns 0 for an
 * out-of-range channel or when disabled. */
uint16_t PfmInput_GetOvercaptureCount(uint8_t channel);

/* TEMPORARY debug aid, 2026-09-09 (see pfm_input.c's own comment on
 * g_dmaStartStatus) -- HAL_StatusTypeDef from the last
 * HAL_TIM_IC_Start_DMA() call for `channel` (0=HAL_OK, 1=HAL_ERROR,
 * 2=HAL_BUSY, 3=HAL_TIMEOUT), or 0xFF if never armed/started or the
 * channel is out of range. Remove once DMA capture is confirmed
 * reliable. */
uint8_t PfmInput_GetDmaStartStatus(uint8_t channel);

/* --------------------------------------------------------------------------
 * RAW EDGE MODE -- re-exploration of duty-cycle measurement, 2026-09-30.
 *
 * The 2026-09-08 duty attempts failed (a real signal read exactly one period
 * late, cause never found) with per-edge interrupts, a live GPIO read to tell
 * rising from falling, and a polarity flip inside the ISR. This mode keeps
 * none of those: a channel can be set to capture BOTH edges (polarity set
 * while the channel is stopped, never touched during a capture), the DMA
 * stores every edge's raw timer value untouched, and telling rising from
 * falling is left to whoever reads the stream (alternation from the level the
 * pin had just before capture started, PfmInput_GetStartLevel()).
 * Diagnostic: it exists to look at what the real edge stream is.
 *
 * Applied at the next PfmInput_Arm(). In BOTH mode the "count" of the channel
 * is EDGES (up to PFM_INPUT_MAX_EDGES), Arm(M) asks for 2M+1 edges (M periods
 * of a rising-first signal), and PfmInput_GetPeriods() is not filled.
 * -------------------------------------------------------------------------- */
#define PFM_INPUT_MAX_EDGES     (2U * PFM_INPUT_MAX_PERIODS + 2U)

/* bothEdges: 0 = rising only (default), 1 = both edges. Returns 0 for a bad
   channel. */
uint8_t PfmInput_SetEdgeMode(uint8_t channel, uint8_t bothEdges);
uint8_t PfmInput_GetEdgeMode(uint8_t channel);

/* The raw timer values of the last BOTH-mode capture, PfmInput_GetCount()
   entries (absolute ticks; the 16-bit timers wrap at 65536). NULL if the
   channel is not in BOTH mode. */
const uint32_t *PfmInput_GetRawEdges(uint8_t channel);

/* Pin level (0/1) read just before the last capture started: the first edge
   captured is the opposite transition. */
uint8_t PfmInput_GetStartLevel(uint8_t channel);


#ifdef __cplusplus
}
#endif

#endif /* INC_PFM_INPUT_H_ */
