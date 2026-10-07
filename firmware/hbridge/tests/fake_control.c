/*
 * fake_control.c -- the part of control_runtime.h that settings.c uses, with the same validation ranges as
 * the real one but no hardware behind it, so the saved-profile glue can be tested on the PC.
 */
#include "control_runtime.h"
#include "supply_mode.h"
#include "pfm.h"

static PFM_Source_t g_fakeSource = PFM_SOURCE_TABLE;
void PFM_SelectSource(PFM_Source_t s) { g_fakeSource = s; }
PFM_Source_t PFM_GetSource(void) { return g_fakeSource; }

static closed_loop_t g_cl;
static uint8_t  g_init = 0U;
static uint32_t g_rate = 10000U;
static uint16_t g_scans = 4U;
static uint16_t g_sample = 475U;
static float    g_slew = 0.0f, g_maxV = 4.0f, g_scale = 1.0f, g_offset = 0.0f, g_filter = 0.0f, g_clip = 10.0f;
static uint8_t  g_channel = 1U, g_loop = 0U;

closed_loop_t *Control_Loop(void)
{
    if (g_init == 0U)
    {
        Cl_Init(&g_cl, 10000U, 2.9f);
        g_init = 1U;
    }
    return &g_cl;
}

void FakeControl_Reset(void)
{
    g_init = 0U;
    g_fakeSource = PFM_SOURCE_TABLE;
    g_rate = 10000U; g_scans = 4U; g_sample = 475U;
    g_slew = 0.0f; g_maxV = 4.0f; g_scale = 1.0f; g_offset = 0.0f; g_filter = 0.0f; g_clip = 10.0f;
    g_channel = 1U; g_loop = 0U;
    (void)Control_Loop();
}

/* the real rule: ticks of g_rate must leave room for g_scans scans of 7 conversions at g_sample clocks (~25 clocks
   of overhead each) on the 42.5 MHz ADC clock */
static int Fits(uint32_t rate, uint16_t scans, uint16_t sampleX10)
{
    float scanUs = 7.0f * ((float)sampleX10 / 10.0f + 12.5f) / 42.5f;

    return (rate >= 1000U) && (rate <= 20000U) && (scans >= 1U) && (scans <= 64U) &&
           ((float)scans * scanUs * 1.0f) < (1.0e6f / (float)rate);
}

ctl_set_result_t Control_SetRateHz(uint32_t hz)
{
    if (!Fits(hz, g_scans, g_sample)) { return CTL_SET_RANGE; }
    g_rate = hz;
    return CTL_SET_OK;
}
uint32_t Control_GetRateHz(void) { return g_rate; }

ctl_set_result_t Control_SetScansPerTick(uint16_t n)
{
    if (!Fits(g_rate, n, g_sample)) { return CTL_SET_RANGE; }
    g_scans = n;
    return CTL_SET_OK;
}
uint16_t Control_GetScansPerTick(void) { return g_scans; }

ctl_set_result_t Control_SetSampleTimeX10(uint16_t x10)
{
    static const uint16_t ok[8] = { 25U, 65U, 125U, 245U, 475U, 925U, 2475U, 6405U };
    int found = 0;

    for (int i = 0; i < 8; i++) { found |= (ok[i] == x10); }
    if (!found || !Fits(g_rate, g_scans, x10)) { return CTL_SET_RANGE; }
    g_sample = x10;
    return CTL_SET_OK;
}
uint16_t Control_GetSampleTimeX10(void) { return g_sample; }

ctl_set_result_t Control_SetSlewPerTick(float v) { if (v < 0.0f) { return CTL_SET_RANGE; } g_slew = v; return CTL_SET_OK; }
float Control_GetSlewPerTick(void) { return g_slew; }
ctl_set_result_t Control_SetMaxVolts(float v) { if (v <= 0.0f || v > 10000.0f) { return CTL_SET_RANGE; } g_maxV = v; return CTL_SET_OK; }
float Control_GetMaxVolts(void) { return g_maxV; }
ctl_set_result_t Control_SetFeedbackChannel(uint8_t c) { if (c > 5U) { return CTL_SET_RANGE; } g_channel = c; return CTL_SET_OK; }
uint8_t Control_GetFeedbackChannel(void) { return g_channel; }
ctl_set_result_t Control_SetFeedbackScale(float v) { if (v <= 0.0f || v > 1.0e6f) { return CTL_SET_RANGE; } g_scale = v; return CTL_SET_OK; }
float Control_GetFeedbackScale(void) { return g_scale; }
ctl_set_result_t Control_SetFeedbackOffset(float v) { if (v < 0.0f || v > 2.9f) { return CTL_SET_RANGE; } g_offset = v; return CTL_SET_OK; }
float Control_GetFeedbackOffset(void) { return g_offset; }
ctl_set_result_t Control_SetFeedbackFilterHz(float v)
{
    if (v < 0.0f || v >= (float)g_rate / 2.0f) { return CTL_SET_RANGE; }
    g_filter = v;
    return CTL_SET_OK;
}
float Control_GetFeedbackFilterHz(void) { return g_filter; }
ctl_set_result_t Control_SetClipMs(float v) { if (v < 0.0f || v > 1000.0f) { return CTL_SET_RANGE; } g_clip = v; return CTL_SET_OK; }
float Control_GetClipMs(void) { return g_clip; }
void Control_SetLoopMode(uint8_t m) { g_loop = (m != 0U) ? 1U : 0U; }
uint8_t Control_GetLoopMode(void) { return g_loop; }

ctl_set_result_t Control_SetGainsFor(uint8_t p, float kp, float ki, float kd)
{
    if (p >= CL_PLANT_COUNT || kp < 0.0f || ki < 0.0f || kd < 0.0f) { return CTL_SET_RANGE; }
    Control_Loop()->cfg.kp[p] = kp; Control_Loop()->cfg.ki[p] = ki; Control_Loop()->cfg.kd[p] = kd;
    return CTL_SET_OK;
}
void Control_GetGainsFor(uint8_t p, float *kp, float *ki, float *kd)
{
    *kp = Control_Loop()->cfg.kp[p]; *ki = Control_Loop()->cfg.ki[p]; *kd = Control_Loop()->cfg.kd[p];
}

ctl_set_result_t Control_SetHvpsDuty(float v)
{
    supply_limits_t lim;
    (void)SupplyMode_GetLimits(SUPPLY_MODE_HVPS, &lim);
    if (v < (float)lim.dutyMinPct || v > (float)lim.dutyMaxPct) { return CTL_SET_RANGE; }
    Control_Loop()->cfg.hvpsDutyPct = v;
    return CTL_SET_OK;
}
float Control_GetHvpsDuty(void) { return Control_Loop()->cfg.hvpsDutyPct; }
ctl_set_result_t Control_SetLimiterFreq(float v)
{
    supply_limits_t lim;
    (void)SupplyMode_GetLimits(SUPPLY_MODE_LIMITER, &lim);
    if (v < (float)lim.freqMinHz || v > (float)lim.freqMaxHz) { return CTL_SET_RANGE; }
    Control_Loop()->cfg.limiterFreqHz = v;
    return CTL_SET_OK;
}
float Control_GetLimiterFreq(void) { return Control_Loop()->cfg.limiterFreqHz; }
ctl_set_result_t Control_SetHvpsResonance(float v)
{
    supply_limits_t lim;
    (void)SupplyMode_GetLimits(SUPPLY_MODE_HVPS, &lim);
    if (v < (float)lim.freqMinHz || v > (float)lim.freqMaxHz) { return CTL_SET_RANGE; }
    Control_Loop()->cfg.hvpsResonanceHz = v;
    return CTL_SET_OK;
}
float Control_GetHvpsResonance(void) { return Control_Loop()->cfg.hvpsResonanceHz; }
ctl_set_result_t Control_SetHvpsVpeak(float v) { if (v <= 0.0f) { return CTL_SET_RANGE; } Control_Loop()->cfg.hvpsVpeak = v; return CTL_SET_OK; }
float Control_GetHvpsVpeak(void) { return Control_Loop()->cfg.hvpsVpeak; }
ctl_set_result_t Control_SetLimiterVscale(float v) { if (v <= 0.0f) { return CTL_SET_RANGE; } Control_Loop()->cfg.limiterVscale = v; return CTL_SET_OK; }
float Control_GetLimiterVscale(void) { return Control_Loop()->cfg.limiterVscale; }
ctl_set_result_t Control_SetHvpsAbove(uint8_t a) { Control_Loop()->cfg.hvpsAbove = (a != 0U) ? 1U : 0U; return CTL_SET_OK; }
uint8_t Control_GetHvpsAbove(void) { return Control_Loop()->cfg.hvpsAbove; }
