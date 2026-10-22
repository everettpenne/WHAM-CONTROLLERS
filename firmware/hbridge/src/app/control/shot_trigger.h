/*
 * shot_trigger.h -- the external shot trigger: a RISING EDGE on PF15 ("Fiber_Enable") while the controller is ARMED
 * starts the shot, exactly as FIRE would (same checks, same start: shot_start.h). EXTernal:TRIGger turns it on and
 * off; it is ON at every boot (SHOT_TRIGGER_DEFAULT_ENABLED, ctrlr_config.h), as on XREX.
 *
 * INTERRUPT-DRIVEN, not polled. The chain, with no main-loop step in it:
 *   edge on PF15 -> EXTI15_10 interrupt (priority 1, the fault tier): ShotTrigger_OnEdge() -- a few instructions:
 *       count the edge; ignore it unless enabled, ARMED, and the pin still reads HIGH (a glitch shorter than the
 *       interrupt latency is rejected); then pend a software interrupt
 *   -> CORDIC_IRQHandler (the unused CORDIC's vector, priority 2): ShotTrigger_RunPending() -- ShotStart_Run().
 * The start itself is not done in the EXTI handler because it takes a few hundred microseconds (it fills the ring
 * of period entries and starts the HRTIM) and the EXTI vector is shared with the gate-driver fault inputs at the
 * priority of the per-period interrupt: running it there would hold up a fault stop, and the first period of the
 * shot being started. At priority 2 both can preempt it.
 *
 * Edge-triggered: a level already HIGH when ARM is sent produces no edge and starts nothing; after a refused start
 * (nothing uploaded, a closed-loop refusal) the controller stays ARMED and needs a new rising edge. An edge that
 * arrives while a FIRE command is being processed is ignored (FIRE is already starting the shot).
 *
 * Diagnostics: EXTernal:TRIGger:STATus? (counters and the last result), EXTernal:TRIGger:INPut? (the raw pin).
 */
#ifndef SHOT_TRIGGER_H
#define SHOT_TRIGGER_H

#include <stdint.h>
#include "shot_start.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Once, from App_Init(): enables the software interrupt. */
void ShotTrigger_Init(void);

void    ShotTrigger_SetEnabled(uint8_t on);
uint8_t ShotTrigger_GetEnabled(void);

/* From the EXTI15_10 handler (interrupt context, priority 1), after it has cleared the line's pending bit. */
void ShotTrigger_OnEdge(void);

/* From the software interrupt's handler (priority 2): starts the shot if an edge asked for it. */
void ShotTrigger_RunPending(void);

typedef struct
{
    uint32_t edges;             /* rising edges seen on PF15 since boot */
    uint32_t started;           /* shots started by an edge */
    uint32_t ignored_disabled;  /* edges ignored: EXTernal:TRIGger is off */
    uint32_t ignored_state;     /* edges ignored: not ARMED (a shot running, idle, faulted) */
    uint32_t ignored_glitch;    /* edges ignored: the pin was LOW again when the interrupt looked */
    uint32_t refused;           /* edges that tried to start a shot and were refused (see `last`) */
    uint32_t busy;              /* ... of which because a FIRE command was starting the shot */
    shot_start_result_t last;   /* the result of the most recent attempt; code is meaningful only if attempts > 0 */
    uint32_t attempts;          /* attempts to start since boot (started + refused) */
} shot_trigger_status_t;

void ShotTrigger_GetStatus(shot_trigger_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SHOT_TRIGGER_H */
