/*
 * shot_start.h -- start the shot from ARMED: the one routine behind both FIRE (serial command) and the external
 * trigger (a rising edge on PF15, shot_trigger.h).
 *
 * It used to live in cmd_fire(); it moved here so the command and the interrupt-driven trigger run exactly the same
 * checks and the same start sequence, and cannot drift apart. What it does, in order:
 *   1. refuses if a fault is latched, or the controller is not ARMED;
 *   2. closed loop on any enabled phase: prepares the loops (Control_Prepare), claims FIRING (SM_Fire), starts the
 *      loops (Control_Go);
 *   3. otherwise open loop: makes the uploaded shot the source, refuses if there is none, claims FIRING (SM_Fire).
 *
 * Callers: the main loop (cmd_fire) and the software interrupt of the external trigger (priority 2). The two cannot
 * run it at once: a claim flag, taken with interrupts masked, makes the second caller get SHOT_START_BUSY. An edge
 * that arrives while a FIRE command is being processed is therefore ignored (the FIRE is already starting the shot).
 */
#ifndef SHOT_START_H
#define SHOT_START_H

#include <stdint.h>
#include "control_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SHOT_START_OK = 0,        /* the shot is running (state FIRING) */
    SHOT_START_FAULT,         /* a fault is latched */
    SHOT_START_NOT_ARMED,     /* not in ARMED */
    SHOT_START_NO_SHOT,       /* nothing uploaded to play */
    SHOT_START_CONTROL,       /* the closed-loop start was refused: see `ctl` in the result */
    SHOT_START_BUSY,          /* another start is in progress (a FIRE command, or the trigger) */
    SHOT_START_CHANGED        /* the state changed between the checks and the start: nothing was started */
} shot_start_code_t;

typedef struct
{
    shot_start_code_t code;
    ctl_result_t      ctl;    /* meaningful when code == SHOT_START_CONTROL */
} shot_start_result_t;

shot_start_result_t ShotStart_Run(void);

/* Any fault latched: the hardware fault input, a gate-driver input, a table underrun, or a closed-loop fault.
   (The command layer's AnyFaultLatched() is this.) */
uint8_t ShotStart_FaultLatched(void);

#ifdef __cplusplus
}
#endif

#endif /* SHOT_START_H */
