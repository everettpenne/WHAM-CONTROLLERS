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
 * One control loop for now ("loop 1"); the structures are per-loop so three independent loops
 * (the Limiter's three outputs) can follow without a protocol change.
 */
#ifndef CONTROL_RUNTIME_H
#define CONTROL_RUNTIME_H

#include <stdint.h>
#include "closed_loop.h"
#include "sim_plant.h"

typedef enum
{
    CTL_OK = 0,
    CTL_NO_MODE,        /* no supply mode selected */
    CTL_NO_TARGET,      /* a profile run with no target profile uploaded */
    CTL_BAD_CONFIG,     /* e.g. the HVPS resonance leaves no usable band */
    CTL_NO_FEEDBACK,    /* the ADC sampler is not running */
    CTL_REF_BAD,        /* VREF+ is not what ANALOG_VREF_MV says (the buffer is out of regulation) */
    CTL_BUSY            /* a closed-loop run is in progress */
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
ctl_result_t Control_Prepare(uint8_t useProfile);   /* computes the opening step, selects the live source */
void Control_Go(void);                              /* the shot has started: begin ticking */
void Control_Stop(void);                            /* operator stop: ends the run at a period boundary */
uint8_t Control_IsRunning(void);

uint8_t Control_GetLoopMode(void);                  /* 0 open loop, 1 closed loop */
void    Control_SetLoopMode(uint8_t mode);

void  Control_SetHoldVolts(float volts);            /* the SOURce:SETpoint hold target; live while holding */
float Control_GetHoldVolts(void);

/* the last tick's setpoint, measurement (process volts) and actuator value (Hz for HVPS, % for Limiter) */
void Control_GetStatus(float *sp, float *pv, float *out);
uint8_t Control_ActuatorIsHz(void);                 /* 1 while the HVPS plant is the selected one */

/* ---- settings (each returns a ctl_set_result_t; getters return the present value) ---- */
ctl_set_result_t Control_SetRateHz(uint32_t hz);          /* ticks per second */
uint32_t         Control_GetRateHz(void);
ctl_set_result_t Control_SetScansPerTick(uint16_t n);     /* ADC scans averaged per tick */
uint16_t         Control_GetScansPerTick(void);
ctl_set_result_t Control_SetSampleTimeX10(uint16_t x10);  /* ADC sampling time, clocks x10 */
uint16_t         Control_GetSampleTimeX10(void);

/* gains of the plant selected by the supply mode; units Hz/V (HVPS), %/V (Limiter) */
ctl_set_result_t Control_SetGains(float kp, float ki, float kd);
void             Control_GetGains(float *kp, float *ki, float *kd);

ctl_set_result_t Control_SetFeedbackChannel(uint8_t channelIndex);   /* 0..5 */
uint8_t          Control_GetFeedbackChannel(void);
ctl_set_result_t Control_SetFeedbackScale(float scale);              /* process volts per ADC volt, > 0 */
float            Control_GetFeedbackScale(void);
ctl_set_result_t Control_SetFeedbackOffset(float adcVolts);          /* ADC volts that read as 0 V */
float            Control_GetFeedbackOffset(void);
ctl_set_result_t Control_SetFeedbackFilterHz(float hz);              /* 0 = off */
float            Control_GetFeedbackFilterHz(void);
ctl_set_result_t Control_SetFeedbackSim(uint8_t sim);                /* 0 ADC, 1 simulated plant */
uint8_t          Control_GetFeedbackSim(void);

ctl_set_result_t Control_SetMaxVolts(float volts);
float            Control_GetMaxVolts(void);
ctl_set_result_t Control_SetClipMs(float ms);                        /* ADC full scale this long = fault; 0 = off */
float            Control_GetClipMs(void);
ctl_set_result_t Control_SetSlewPerTick(float perTick);              /* actuator units per tick, 0 = off */
float            Control_GetSlewPerTick(void);

closed_loop_t *Control_Loop(void);        /* the plant parameters (duty, carrier, branch, resonance, ...) */
sim_plant_t   *Control_SimPlant(void);

/* ---- logging (LOG:*): every `decim`-th tick appends {setpoint, measurement, actuator} ---- */
void     Control_LogArm(uint16_t maxSamples, uint16_t decim);
uint16_t Control_LogCount(void);
uint32_t Control_LogRateHz(void);         /* the sample rate: tick rate / decim */
/* sample i as millivolts, millivolts, and milli-Hz or milli-percent */
uint8_t  Control_LogGet(uint16_t i, int32_t *spMilli, int32_t *pvMilli, int32_t *outMilli);

#endif /* CONTROL_RUNTIME_H */
