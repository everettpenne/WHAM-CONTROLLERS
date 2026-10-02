/*
 * closed_loop.h / closed_loop.c -- the closed-loop voltage controller: once per tick it takes the
 * feedback reading, compares it with the target voltage, and returns the next {per, cmp} for the
 * PFM/PWM output. Added 2026-10-02. Pure logic, no hardware: host-tested against simulated plants
 * (tests/test_closed_loop.c). The hardware around it (the ADC DMA tick, the output mailbox, the
 * faults) is control_task.c.
 *
 * MODULAR by seams:
 *   feedback source   feedback.h       raw ADC reading -> process volts
 *   controller        pid_core.h       the arithmetic (replaceable by anything with its shape)
 *   plant mapping     here, below      what the actuator IS for each supply
 *
 * The plant mapping is the one place the two supplies differ (selected by the supply mode):
 *   HVPS     the output voltage is set by the switching FREQUENCY (PFM). Duty is fixed (40-60 %,
 *            50 by default). The voltage peaks at a resonance (~19 kHz) and falls either side, so
 *            the loop works on ONE side of it, chosen by the operator: ABOVE (voltage falls as the
 *            frequency rises, the usual case) or BELOW. The actuator u is the frequency in Hz,
 *            limited to that band; the controller's polarity follows the branch, so the operator's
 *            gains are positive either way.
 *   LIMITER  the output voltage is set by the DUTY (PWM), V ~ Vscale * (2D - 1), at a fixed carrier
 *            frequency. u is the duty in percent, limited to the mode's duty range.
 *
 * Output = feedforward(target) + PID correction. The feedforward is a simple model (linear
 * between the band edges for HVPS, the 2D-1 relation for the Limiter) so the loop starts near the
 * answer; the PID removes what the model gets wrong. A calibrated feedforward would replace the
 * model without changing anything else.
 *
 * Faults the tick reports (the caller stops the output): the feedback's magnitude above the hard
 * limit (OVERVOLTAGE), and the ADC sitting at full scale for too long (the loop is blind there).
 */
#ifndef CLOSED_LOOP_H
#define CLOSED_LOOP_H

#include <stdint.h>
#include "pid_core.h"
#include "feedback.h"

typedef enum
{
    CL_RUNNING = 0,
    CL_FINISHED,            /* the target profile reached its end */
    CL_FAULT_OVERVOLTAGE,
    CL_FAULT_CLIPPED,       /* the ADC sat at full scale */
    CL_FAULT_CLIPPED_LOW    /* the ADC sat at the bottom of a mid-scale-zero range: a dead feedback line, or beyond -Vmax */
} cl_status_t;

enum { CL_PLANT_HVPS = 0, CL_PLANT_LIMITER = 1, CL_PLANT_COUNT = 2 };

typedef struct
{
    /* gains per plant (units: Hz per V for HVPS, percent per V for the Limiter), all >= 0 */
    float kp[CL_PLANT_COUNT];
    float ki[CL_PLANT_COUNT];            /* per second */
    float kd[CL_PLANT_COUNT];            /* seconds */
    float dFilter;
    float slewPerTick;                   /* actuator change limit per tick (Hz or %); 0 = off */
    float maxVolts;                      /* |process volts| above this is an overvoltage fault */
    uint32_t clipTicks;                  /* ticks at ADC full scale before the clipped fault; 0 = off */

    /* HVPS */
    float hvpsDutyPct;                   /* fixed duty, within the mode's range */
    float hvpsResonanceHz;
    uint8_t hvpsAbove;                   /* 1 = operate above resonance, 0 = below */
    float hvpsVpeak;                     /* process volts at resonance, for the feedforward */

    /* Limiter */
    float limiterFreqHz;                 /* the fixed carrier */
    float limiterVscale;                 /* process volts at |2D - 1| = 1 */
} cl_config_t;

typedef struct
{
    cl_config_t cfg;
    feedback_t fb;
    pid_core_t pid;

    uint32_t rateHz;                     /* ticks per second */
    float dt;
    uint32_t tick;                       /* ticks since Cl_Start() */
    uint8_t active;
    uint8_t plant;                       /* CL_PLANT_* of the run in progress */
    uint8_t useProfile;                  /* 1: follow the target profile; 0: hold holdVolts */
    float holdVolts;

    float uMin, uMax;                    /* actuator limits of this run */
    float polarity;                      /* +1, or -1 for HVPS above resonance */

    float sp, pv, out;                   /* the last tick's setpoint, measurement and actuator value */
} closed_loop_t;

/* Defaults for every setting (gains 0: inert until the operator sets them). `vrefV` is the ADC's
   full scale in volts. */
void Cl_Init(closed_loop_t *cl, uint32_t rateHz, float vrefV);

/* Applies the tick rate (and the feedback filter that depends on it). Only between runs. */
void Cl_SetRate(closed_loop_t *cl, uint32_t rateHz);

/* Begins a run for the SELECTED SUPPLY MODE: picks the plant, resets the controller, and writes
   the opening step (the feedforward for the target at t = 0, or for `holdVolts`) to *per / *cmp.
   Returns 0 (nothing written) with no supply mode, no target profile when `useProfile` is set,
   or an unusable HVPS band. */
uint8_t Cl_Start(closed_loop_t *cl, uint8_t useProfile, float holdVolts, uint16_t *per, uint16_t *cmp);

/* One control tick: `meanCode` is the feedback input's ADC reading averaged over the tick's
   samples. Writes the next step to *per / *cmp unless a fault or the end of the profile is reported
   (then nothing is written and the caller stops the output). */
cl_status_t Cl_Tick(closed_loop_t *cl, float meanCode, uint16_t *per, uint16_t *cmp);

/* Changes the hold setpoint of a running hold (not profile) run. */
void Cl_SetHoldVolts(closed_loop_t *cl, float volts);

void Cl_Stop(closed_loop_t *cl);

#endif /* CLOSED_LOOP_H */
