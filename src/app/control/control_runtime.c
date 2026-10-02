/*
 * control_runtime.c -- see control_runtime.h.
 */
#include "control_runtime.h"
#include "ctrlr_config.h"
#include "adc.h"
#include "pfm.h"
#include "state_machine.h"
#include "supply_mode.h"
#include "target_profile.h"
#include "mcu.h"
#include <stddef.h>

#define CONTROL_LOG_MAX_SAMPLES  (1000U)
#define REF_TOLERANCE_PCT        (3.0f)     /* VREF+ must be within this of ANALOG_VREF_MV to run closed loop on the ADC */

static closed_loop_t g_cl;
static sim_plant_t   g_sim;

static uint8_t  g_loopMode = 0U;
static uint8_t  g_fbChannel = 1U;                 /* PA1 = AnalogIn_02, the default feedback */
static uint8_t  g_fbSim = 0U;
static float    g_fbFilterHz = 0.0f;
static float    g_clipMs = 10.0f;
static uint32_t g_rateHz = CONTROL_RATE_HZ_DEFAULT;
static uint16_t g_scans = CONTROL_SCANS_PER_TICK_DEFAULT;
static float    g_holdVolts = 0.0f;

static volatile uint8_t g_run = 0U;
static volatile uint8_t g_skip = 0U;              /* the first block after the start has samples from before it */
static uint16_t g_lastPer = 0U;
static uint16_t g_lastCmp = 0U;

/* the log */
static int32_t  g_log[CONTROL_LOG_MAX_SAMPLES][3];
static volatile uint16_t g_logCount = 0U;
static uint16_t g_logMax = 0U;
static uint16_t g_logDecim = 1U;
static uint16_t g_logPhase = 0U;
static volatile uint8_t g_logArmed = 0U;

static void ApplyClip(void)
{
    g_cl.cfg.clipTicks = (uint32_t)(g_clipMs * (float)g_rateHz / 1000.0f + 0.5f);
}

static void ApplyFilter(void)
{
    Feedback_SetFilter(&g_cl.fb, g_fbFilterHz, g_cl.dt);
}

/* One control tick. Runs in the ADC DMA interrupt (priority 2). */
static void OnBlock(const float *means)
{
    float code;
    uint16_t per;
    uint16_t cmp;
    cl_status_t st;

    if (g_run == 0U)
    {
        return;
    }
    if (g_skip != 0U)
    {
        g_skip = 0U;
        return;
    }
    if (g_fbSim != 0U)
    {
        /* the plant model answers the step the controller commanded last; its voltage goes through the
           inverse of the feedback calibration so the loop sees what an ADC would read */
        float v = SimPlant_Step(&g_sim, (g_cl.plant == CL_PLANT_LIMITER) ? 1U : 0U, g_lastPer, g_lastCmp, g_cl.dt);
        float adcV = v / g_cl.fb.scale + g_cl.fb.offsetV;

        code = adcV / g_cl.fb.vrefV * 4095.0f;
        if (code < 0.0f) { code = 0.0f; }
        if (code > 4095.0f) { code = 4095.0f; }
    }
    else
    {
        code = means[g_fbChannel];
    }

    st = Cl_Tick(&g_cl, code, &per, &cmp);
    switch (st)
    {
        case CL_RUNNING:
            g_lastPer = per;
            g_lastCmp = cmp;
            PFM_LiveSet(per, cmp);
            break;
        case CL_FINISHED:
            g_run = 0U;
            PFM_LiveRequestStop();
            break;
        case CL_FAULT_OVERVOLTAGE:
            g_run = 0U;
            SM_ReportControlFault(SM_FAULT_SRC_OVERVOLT);
            break;
        case CL_FAULT_CLIPPED:
        case CL_FAULT_CLIPPED_LOW:
        default:
            g_run = 0U;
            SM_ReportControlFault(SM_FAULT_SRC_FEEDBACK);
            break;
    }

    if ((g_logArmed != 0U) && (g_logCount < g_logMax))
    {
        g_logPhase++;
        if (g_logPhase >= g_logDecim)
        {
            g_logPhase = 0U;
            g_log[g_logCount][0] = (int32_t)(g_cl.sp * 1000.0f);
            g_log[g_logCount][1] = (int32_t)(g_cl.pv * 1000.0f);
            g_log[g_logCount][2] = (int32_t)(g_cl.out * 1000.0f);
            g_logCount++;
        }
    }
}

/* (Re)starts the ADC sampler at the present rate, scans per tick and sampling time. */
static uint8_t RestartSampler(void)
{
    return Adc_StartSampler(g_rateHz * g_scans, g_scans, OnBlock);
}

void Control_Init(void)
{
    Cl_Init(&g_cl, g_rateHz, (float)ANALOG_VREF_MV / 1000.0f);
#if (FEEDBACK_VMAX_DEFAULT_V > 0)
    Feedback_SetBipolarRange(&g_cl.fb, (float)FEEDBACK_VMAX_DEFAULT_V);
#endif
    SimPlant_Init(&g_sim);
    ApplyClip();
    ApplyFilter();
    (void)RestartSampler();
}

static uint8_t PlantIndex(void)
{
    return (SupplyMode_Get() == SUPPLY_MODE_LIMITER) ? CL_PLANT_LIMITER : CL_PLANT_HVPS;
}

ctl_result_t Control_Prepare(uint8_t useProfile)
{
    uint16_t per;
    uint16_t cmp;

    if (g_run != 0U)
    {
        return CTL_BUSY;
    }
    if (SupplyMode_Get() == SUPPLY_MODE_NONE)
    {
        return CTL_NO_MODE;
    }
    if ((useProfile != 0U) && (Target_IsComplete() == 0U))
    {
        return CTL_NO_TARGET;
    }
    if (g_fbSim == 0U)
    {
        uint16_t code = 0U;
        uint16_t cal = 0U;

        if ((Adc_SamplerRunning() == 0U) || (Adc_ReadVrefint(&code, &cal) == 0U) || (code == 0U))
        {
            return CTL_NO_FEEDBACK;
        }
        /* the ADC's reference must be what the scaling assumes: the internal buffer is out of regulation
           if VDDA sags below its minimum for the selected voltage */
        {
            float vref = 3000.0f * (float)cal / (float)code;
            float want = (float)ANALOG_VREF_MV;
            float dev = (vref - want) / want * 100.0f;

            if ((dev > REF_TOLERANCE_PCT) || (dev < -REF_TOLERANCE_PCT))
            {
                return CTL_REF_BAD;
            }
        }
    }
    if (Cl_Start(&g_cl, useProfile, g_holdVolts, &per, &cmp) == 0U)
    {
        return CTL_BAD_CONFIG;
    }
    g_lastPer = per;
    g_lastCmp = cmp;
    SimPlant_Reset(&g_sim, 0.0f);
    PFM_LiveSet(per, cmp);
    PFM_SelectSource(PFM_SOURCE_LIVE);
    if (g_logArmed != 0U)
    {
        g_logCount = 0U;
        g_logPhase = 0U;
    }
    return CTL_OK;
}

void Control_Go(void)
{
    g_skip = 1U;
    g_run = 1U;
}

void Control_Stop(void)
{
    if (g_run != 0U)
    {
        g_run = 0U;
        Cl_Stop(&g_cl);
    }
    if (PFM_GetSource() == PFM_SOURCE_LIVE)
    {
        PFM_LiveRequestStop();
    }
}

uint8_t Control_IsRunning(void)
{
    return g_run;
}

uint8_t Control_GetLoopMode(void)
{
    return g_loopMode;
}

void Control_SetLoopMode(uint8_t mode)
{
    g_loopMode = (mode != 0U) ? 1U : 0U;
}

void Control_SetHoldVolts(float volts)
{
    g_holdVolts = volts;
    Cl_SetHoldVolts(&g_cl, volts);
}

float Control_GetHoldVolts(void)
{
    return g_holdVolts;
}

void Control_GetStatus(float *sp, float *pv, float *out)
{
    *sp = g_cl.sp;
    *pv = g_cl.pv;
    *out = g_cl.out;
}

uint8_t Control_ActuatorIsHz(void)
{
    return (PlantIndex() == CL_PLANT_HVPS) ? 1U : 0U;
}

/* ---- settings ---- */

/* The sampler and the loop timing are one setting group: the rate times the scans per tick is the
   scan rate, and the scan must fit in its period at the sampling time. */
static ctl_set_result_t ApplyTiming(uint32_t rateHz, uint16_t scans, uint16_t smpX10)
{
    float scanUs = Adc_ScanTimeUsFor(smpX10);

    if (g_run != 0U)
    {
        return CTL_SET_BUSY;
    }
    if ((rateHz < 1000U) || (rateHz > 20000U) || (scans < 1U) || (scans > ADC_MAX_BLOCK_SCANS) || (scanUs <= 0.0f) ||
        ((rateHz * scans) < 3000U) || ((rateHz * scans) > 400000U) ||
        ((scanUs * 1.15f) > (1.0e6f / (float)(rateHz * scans))))
    {
        return CTL_SET_RANGE;
    }
    g_rateHz = rateHz;
    g_scans = scans;
    (void)Adc_SetSampleTimeX10(smpX10);
    Cl_SetRate(&g_cl, g_rateHz);
    ApplyClip();
    ApplyFilter();
    return (RestartSampler() != 0U) ? CTL_SET_OK : CTL_SET_RANGE;
}

ctl_set_result_t Control_SetRateHz(uint32_t hz)
{
    return ApplyTiming(hz, g_scans, Adc_GetSampleTimeX10());
}

uint32_t Control_GetRateHz(void)
{
    return g_rateHz;
}

ctl_set_result_t Control_SetScansPerTick(uint16_t n)
{
    return ApplyTiming(g_rateHz, n, Adc_GetSampleTimeX10());
}

uint16_t Control_GetScansPerTick(void)
{
    return g_scans;
}

ctl_set_result_t Control_SetSampleTimeX10(uint16_t x10)
{
    return ApplyTiming(g_rateHz, g_scans, x10);
}

uint16_t Control_GetSampleTimeX10(void)
{
    return Adc_GetSampleTimeX10();
}

ctl_set_result_t Control_SetGains(float kp, float ki, float kd)
{
    uint8_t p = PlantIndex();

    if (g_run != 0U)
    {
        return CTL_SET_BUSY;
    }
    if ((kp < 0.0f) || (ki < 0.0f) || (kd < 0.0f))
    {
        return CTL_SET_RANGE;
    }
    g_cl.cfg.kp[p] = kp;
    g_cl.cfg.ki[p] = ki;
    g_cl.cfg.kd[p] = kd;
    return CTL_SET_OK;
}

void Control_GetGains(float *kp, float *ki, float *kd)
{
    uint8_t p = PlantIndex();

    *kp = g_cl.cfg.kp[p];
    *ki = g_cl.cfg.ki[p];
    *kd = g_cl.cfg.kd[p];
}

ctl_set_result_t Control_SetFeedbackChannel(uint8_t channelIndex)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if (channelIndex >= ADC_NUM_CHANNELS) { return CTL_SET_RANGE; }
    g_fbChannel = channelIndex;
    return CTL_SET_OK;
}

uint8_t Control_GetFeedbackChannel(void)
{
    return g_fbChannel;
}

ctl_set_result_t Control_SetFeedbackScale(float scale)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((scale <= 0.0f) || (scale > 1.0e6f)) { return CTL_SET_RANGE; }
    Feedback_SetCalibration(&g_cl.fb, scale, g_cl.fb.offsetV);
    return CTL_SET_OK;
}

float Control_GetFeedbackScale(void)
{
    return g_cl.fb.scale;
}

ctl_set_result_t Control_SetFeedbackOffset(float adcVolts)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((adcVolts < 0.0f) || (adcVolts > g_cl.fb.vrefV)) { return CTL_SET_RANGE; }
    Feedback_SetCalibration(&g_cl.fb, g_cl.fb.scale, adcVolts);
    return CTL_SET_OK;
}

float Control_GetFeedbackOffset(void)
{
    return g_cl.fb.offsetV;
}

ctl_set_result_t Control_SetFeedbackRange(float vmax)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((vmax <= 0.0f) || (vmax > 1.0e5f)) { return CTL_SET_RANGE; }
    Feedback_SetBipolarRange(&g_cl.fb, vmax);
    return CTL_SET_OK;
}

float Control_GetFeedbackRange(void)
{
    /* the process voltage at the top of the ADC range -- +Vmax for a bipolar calibration */
    return (g_cl.fb.vrefV - g_cl.fb.offsetV) * g_cl.fb.scale;
}

ctl_set_result_t Control_SetFeedbackFilterHz(float hz)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((hz < 0.0f) || (hz >= (float)g_rateHz / 2.0f)) { return CTL_SET_RANGE; }
    g_fbFilterHz = hz;
    ApplyFilter();
    return CTL_SET_OK;
}

float Control_GetFeedbackFilterHz(void)
{
    return g_fbFilterHz;
}

ctl_set_result_t Control_SetFeedbackSim(uint8_t sim)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    g_fbSim = (sim != 0U) ? 1U : 0U;
    return CTL_SET_OK;
}

uint8_t Control_GetFeedbackSim(void)
{
    return g_fbSim;
}

ctl_set_result_t Control_SetMaxVolts(float volts)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((volts <= 0.0f) || (volts > 10000.0f)) { return CTL_SET_RANGE; }
    g_cl.cfg.maxVolts = volts;
    return CTL_SET_OK;
}

float Control_GetMaxVolts(void)
{
    return g_cl.cfg.maxVolts;
}

ctl_set_result_t Control_SetClipMs(float ms)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((ms < 0.0f) || (ms > 1000.0f)) { return CTL_SET_RANGE; }
    g_clipMs = ms;
    ApplyClip();
    return CTL_SET_OK;
}

float Control_GetClipMs(void)
{
    return g_clipMs;
}

ctl_set_result_t Control_SetSlewPerTick(float perTick)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if (perTick < 0.0f) { return CTL_SET_RANGE; }
    g_cl.cfg.slewPerTick = perTick;
    return CTL_SET_OK;
}

float Control_GetSlewPerTick(void)
{
    return g_cl.cfg.slewPerTick;
}

closed_loop_t *Control_Loop(void)
{
    return &g_cl;
}

sim_plant_t *Control_SimPlant(void)
{
    return &g_sim;
}

/* ---- logging ---- */

void Control_LogArm(uint16_t maxSamples, uint16_t decim)
{
    g_logArmed = 0U;
    g_logCount = 0U;
    g_logPhase = 0U;
    g_logMax = (maxSamples > CONTROL_LOG_MAX_SAMPLES) ? CONTROL_LOG_MAX_SAMPLES : maxSamples;
    g_logDecim = (decim < 1U) ? 1U : decim;
    g_logArmed = (g_logMax > 0U) ? 1U : 0U;
}

uint16_t Control_LogCount(void)
{
    return g_logCount;
}

uint32_t Control_LogRateHz(void)
{
    return g_rateHz / g_logDecim;
}

uint8_t Control_LogGet(uint16_t i, int32_t *spMilli, int32_t *pvMilli, int32_t *outMilli)
{
    if (i >= g_logCount)
    {
        return 0U;
    }
    *spMilli = g_log[i][0];
    *pvMilli = g_log[i][1];
    *outMilli = g_log[i][2];
    return 1U;
}
