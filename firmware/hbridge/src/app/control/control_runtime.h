/*
 * control_runtime.h / control_runtime.c -- the closed loop's runtime: it owns the controller
 * (closed_loop.h), the ADC sampler's block interrupt that is the fixed-rate control tick, the
 * hand-over of each tick's output step to the PFM engine, the logging, and every setting the
 * operator can change (cmd_control.c). Added 2026-10-02.
 *
 *   ADC DMA block interrupt (adc.h) -> OnBlock() = one control tick
 *        feedback input -> Cl_Tick() -> PFM_LiveSet(per, cmp)       (per-period interrupt applies it)
 *        faults -> SM_ReportControlFault() (output stopped at once); end of profile -> a stop at the
 *        next period boundary (the HRTIM outputs are then disabled)
 *
 * The feedback can come from the ADC (the real thing) or from a firmware plant model
 * (sim_plant.h) so the whole loop, the output and the profile can be tested with nothing wired.
 *
 * THREE loops, one per phase (2026-10-13): loop 1 = phase U (feedback on PA0), loop 2 = V (PA1),
 * loop 3 = W (PB0). In code a loop is 0..2 (CTL_LOOPS of them; the commands say 1..3, 0 = all).
 * Each has its own mode (open or closed), gains, feedback input and calibration, hold setpoint and
 * target series. A run may mix them: a closed phase's duty comes from its controller, an open
 * phase follows its duty in the shot (SHOT:*), and the carrier is the shot's frequency if any
 * phase is open, else the fixed closed-loop carrier. The HVPS has ONE frequency loop (loop 1): its
 * actuator is shared by every phase, so loops 2 and 3 do not apply to it.
 */
#ifndef CONTROL_RUNTIME_H
#define CONTROL_RUNTIME_H

#include <stdint.h>
#include "closed_loop.h"
#include "sim_plant.h"
#include "ctrlr_config.h"

#define CTL_LOOPS        (HRTIM_NUM_CHANNELS)
#define CTL_ALL_LOOPS    (0xFFU)             /* for the setters that take a loop: every loop */

typedef enum
{
    CTL_OK = 0,
    CTL_NO_MODE,        /* no supply mode selected */
    CTL_NO_TARGET,      /* a profile run with no target profile uploaded */
    CTL_BAD_CONFIG,     /* e.g. the HVPS resonance leaves no usable band */
    CTL_NO_FEEDBACK,    /* the ADC sampler is not running */
    CTL_REF_BAD,        /* VREF+ is not what ANALOG_VREF_MV says (the buffer is out of regulation) */
    CTL_NO_METER,       /* the loopback source needs a free PFM_Input_01 duty meter (no PFMIN capture running) */
    CTL_BUSY,           /* a closed-loop run is in progress */
    CTL_NO_SHOT,        /* an enabled phase is open loop and there is no shot to follow */
    CTL_NEED_CLOSED     /* a hold (SOURce:RUN) needs every enabled phase closed loop */
} ctl_result_t;

typedef enum
{
    CTL_SET_OK = 0,
    CTL_SET_RANGE,      /* value outside the allowed range */
    CTL_SET_BUSY        /* not allowed while a run is in progress */
} ctl_set_result_t;

/* Once at start-up, after Adc_Init(): builds the loop with its defaults and starts the ADC sampler
   at CONTROL_RATE_HZ_DEFAULT. */
void Control_Init(void);

/* ---- running ---- */
/* 1 if FIRE must run a closed loop: the HVPS with loop 1 closed, or the Limiter with a closed loop on an
   enabled phase. 0: the plain open-loop shot (the ring). */
uint8_t Control_ClosedRunWanted(void);

ctl_result_t Control_Prepare(uint8_t useProfile);   /* computes the opening step, selects the live source */
void Control_Go(void);                              /* the shot has started: begin ticking */
void Control_Stop(void);                            /* operator stop: ends the run at a period boundary */
uint8_t Control_IsRunning(void);

/* per loop (0..CTL_LOOPS-1; CTL_ALL_LOOPS for every loop in the setters) */
uint8_t Control_GetLoopMode(uint8_t loop);          /* 0 open loop, 1 closed loop */
void    Control_SetLoopMode(uint8_t loop, uint8_t mode);

void  Control_SetHoldVolts(uint8_t loop, float volts);   /* the SOURce:SETpoint hold target; live while holding */
float Control_GetHoldVolts(uint8_t loop);

/* the last tick's setpoint, measurement (process volts) and actuator value (Hz for HVPS, % for Limiter) of a loop */
void Control_GetStatus(uint8_t loop, float *sp, float *pv, float *out);

/* The loop that stopped the last run with a fault and what it was: kind 0 none, 1 overvoltage, 2 ADC full scale,
   3 ADC at the bottom of the range. Cleared when a closed-loop run starts. */
void Control_GetFault(uint8_t *loop, uint8_t *kind);

/* Control-tick timing of the last runs, in CPU cycles: ticks counted, the longest tick and the total; and the cycles
   one tick period is worth. A tick that takes more than its period is an overrun (the next block is late). */
void Control_GetDiag(uint32_t *ticks, uint32_t *maxCycles, uint32_t *sumCycles, uint32_t *budgetCycles);
uint8_t Control_ActuatorIsHz(void);                 /* 1 while the HVPS plant is the selected one */

/* ---- settings (each returns a ctl_set_result_t; getters return the present value) ---- */
ctl_set_result_t Control_SetRateHz(uint32_t hz);          /* ticks per second */
uint32_t         Control_GetRateHz(void);
ctl_set_result_t Control_SetScansPerTick(uint16_t n);     /* ADC scans averaged per tick */
uint16_t         Control_GetScansPerTick(void);
ctl_set_result_t Control_SetSampleTimeX10(uint16_t x10);  /* ADC sampling time, clocks x10 */
uint16_t         Control_GetSampleTimeX10(void);

/* gains of a loop, for the plant selected by the supply mode; units Hz/V (HVPS), %/V (Limiter) */
ctl_set_result_t Control_SetGains(uint8_t loop, float kp, float ki, float kd);
void             Control_GetGains(uint8_t loop, float *kp, float *ki, float *kd);
ctl_set_result_t Control_SetGainsFor(uint8_t loop, uint8_t plant, float kp, float ki, float kd);   /* a plant by index (CL_PLANT_*), whatever the mode */
void             Control_GetGainsFor(uint8_t loop, uint8_t plant, float *kp, float *ki, float *kd);

/* the closed-loop plant parameters (CONFig:CLOSEDloop:*), validated against the supply modes' own limits */
ctl_set_result_t Control_SetHvpsDuty(float pct);        float   Control_GetHvpsDuty(void);
ctl_set_result_t Control_SetLimiterFreq(float hz);      float   Control_GetLimiterFreq(void);
ctl_set_result_t Control_SetHvpsResonance(float hz);    float   Control_GetHvpsResonance(void);
ctl_set_result_t Control_SetHvpsVpeak(float volts);     float   Control_GetHvpsVpeak(void);
ctl_set_result_t Control_SetLimiterVscale(float volts); float   Control_GetLimiterVscale(void);
ctl_set_result_t Control_SetHvpsAbove(uint8_t above);   uint8_t Control_GetHvpsAbove(void);

/* the feedback of a loop: defaults U = PA0 (index 0), V = PA1 (1), W = PB0 (3) */
ctl_set_result_t Control_SetFeedbackChannel(uint8_t loop, uint8_t channelIndex);   /* 0..5 */
uint8_t          Control_GetFeedbackChannel(uint8_t loop);
ctl_set_result_t Control_SetFeedbackScale(uint8_t loop, float scale);              /* process volts per ADC volt, > 0 */
float            Control_GetFeedbackScale(uint8_t loop);
ctl_set_result_t Control_SetFeedbackOffset(uint8_t loop, float adcVolts);          /* ADC volts that read as 0 V */
float            Control_GetFeedbackOffset(uint8_t loop);
ctl_set_result_t Control_SetFeedbackRange(uint8_t loop, float vmax);               /* bipolar +-vmax: -vmax -> ADC 0 V, +vmax -> full scale */
float            Control_GetFeedbackRange(uint8_t loop);                           /* process volts at the top of the ADC range */
ctl_set_result_t Control_SetFeedbackFilterHz(float hz);              /* 0 = off */
float            Control_GetFeedbackFilterHz(void);
ctl_set_result_t Control_SetFeedbackSim(uint8_t sim);                /* 0 ADC, 1 simulated plant, 2 plant through the DAC (PA5) and the real ADC, 3 loopback: duty measured on PFM_Input_01 */
ctl_set_result_t Control_SetDacRatio(float ratio);                   /* ADC volts per DAC volt of the DAC loopback, 0.05-2 */
float            Control_GetDacRatio(void);
uint8_t          Control_GetFeedbackSim(void);

ctl_set_result_t Control_SetMaxVolts(float volts);
float            Control_GetMaxVolts(void);
ctl_set_result_t Control_SetClipMs(float ms);                        /* ADC full scale this long = fault; 0 = off */
float            Control_GetClipMs(void);
ctl_set_result_t Control_SetSlewPerTick(float perTick);              /* actuator units per tick, 0 = off */
float            Control_GetSlewPerTick(void);

closed_loop_t *Control_Loop(void);        /* loop 1's controller: the HVPS's (curve, branch, ...); the plant parameters are copied to all loops */
closed_loop_t *Control_LoopN(uint8_t loop);
sim_plant_t   *Control_SimPlant(void);    /* loop 1's simulated plant (SIM:PLANT:* sets every loop's) */
sim_plant_t   *Control_SimPlantN(uint8_t loop);

/* ---- logging (LOG:*): every `decim`-th tick appends {setpoint, measurement, actuator} of the logged loop(s) ----
   One pool serves one loop (up to 1000 samples) or all of them (a third of that each). */
#define CONTROL_LOG_POOL_WORDS  (3000U)
uint16_t Control_LogMaxSamples(uint8_t loopOrAll);       /* the most samples LOG:ARM can ask for */
void     Control_LogArm(uint8_t loopOrAll, uint16_t maxSamples, uint16_t decim);
uint16_t Control_LogCount(void);
uint32_t Control_LogRateHz(void);         /* the sample rate: tick rate / decim */
/* sample i of a loop as millivolts, millivolts, and milli-Hz or milli-percent; 0 if that loop was not logged */
uint8_t  Control_LogGet(uint8_t loop, uint16_t i, int32_t *spMilli, int32_t *pvMilli, int32_t *outMilli);

#endif /* CONTROL_RUNTIME_H */
