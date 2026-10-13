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
static float    g_slew = 0.0f, g_maxV = 4.0f, g_filter = 0.0f, g_clip = 10.0f;
/* per loop, as in the real control_runtime.c: the defaults are U on PA0, V on PA1, W on PB0 */
static float    g_scale[CTL_LOOPS], g_offset[CTL_LOOPS];
static uint8_t  g_channel[CTL_LOOPS], g_loop[CTL_LOOPS];
static float    g_gain[CTL_LOOPS][CL_PLANT_COUNT][3];

static void LoopDefaults(void)
{
    static const uint8_t kCh[CTL_LOOPS] = { 0U, 1U, 3U };

    for (uint8_t l = 0U; l < CTL_LOOPS; l++)
    {
        g_scale[l] = 1.0f; g_offset[l] = 0.0f; g_channel[l] = kCh[l]; g_loop[l] = 0U;
        for (uint8_t p = 0U; p < CL_PLANT_COUNT; p++) { g_gain[l][p][0] = g_gain[l][p][1] = g_gain[l][p][2] = 0.0f; }
    }
}

/* the loops' own selection of "one loop or every loop", as the real setters take it */
static int LoopRange(uint8_t loop, uint8_t *lo, uint8_t *hi)
{
    if (loop == CTL_ALL_LOOPS) { *lo = 0U; *hi = CTL_LOOPS; return 1; }
    if (loop >= CTL_LOOPS) { return 0; }
    *lo = loop; *hi = (uint8_t)(loop + 1U);
    return 1;
}

closed_loop_t *Control_Loop(void)
{
    if (g_init == 0U)
    {
        Cl_Init(&g_cl, 10000U, 2.9f);
        LoopDefaults();
        g_init = 1U;
    }
    return &g_cl;
}

void FakeControl_Reset(void)
{
    g_init = 0U;
    g_fakeSource = PFM_SOURCE_TABLE;
    g_rate = 10000U; g_scans = 4U; g_sample = 475U;
    g_slew = 0.0f; g_maxV = 4.0f; g_filter = 0.0f; g_clip = 10.0f;
    LoopDefaults();
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
ctl_set_result_t Control_SetFeedbackChannel(uint8_t l, uint8_t c)
{
    uint8_t lo, hi;
    if (!LoopRange(l, &lo, &hi) || c > 5U) { return CTL_SET_RANGE; }
    for (uint8_t i = lo; i < hi; i++) { g_channel[i] = c; }
    return CTL_SET_OK;
}
uint8_t Control_GetFeedbackChannel(uint8_t l) { return g_channel[l % CTL_LOOPS]; }
ctl_set_result_t Control_SetFeedbackScale(uint8_t l, float v)
{
    uint8_t lo, hi;
    if (!LoopRange(l, &lo, &hi) || v <= 0.0f || v > 1.0e6f) { return CTL_SET_RANGE; }
    for (uint8_t i = lo; i < hi; i++) { g_scale[i] = v; }
    return CTL_SET_OK;
}
float Control_GetFeedbackScale(uint8_t l) { return g_scale[l % CTL_LOOPS]; }
ctl_set_result_t Control_SetFeedbackOffset(uint8_t l, float v)
{
    uint8_t lo, hi;
    if (!LoopRange(l, &lo, &hi) || v < 0.0f || v > 2.9f) { return CTL_SET_RANGE; }
    for (uint8_t i = lo; i < hi; i++) { g_offset[i] = v; }
    return CTL_SET_OK;
}
float Control_GetFeedbackOffset(uint8_t l) { return g_offset[l % CTL_LOOPS]; }
ctl_set_result_t Control_SetFeedbackFilterHz(float v)
{
    if (v < 0.0f || v >= (float)g_rate / 2.0f) { return CTL_SET_RANGE; }
    g_filter = v;
    return CTL_SET_OK;
}
float Control_GetFeedbackFilterHz(void) { return g_filter; }
ctl_set_result_t Control_SetClipMs(float v) { if (v < 0.0f || v > 1000.0f) { return CTL_SET_RANGE; } g_clip = v; return CTL_SET_OK; }
float Control_GetClipMs(void) { return g_clip; }
void Control_SetLoopMode(uint8_t l, uint8_t m)
{
    uint8_t lo, hi;
    if (!LoopRange(l, &lo, &hi)) { return; }
    for (uint8_t i = lo; i < hi; i++) { g_loop[i] = (m != 0U) ? 1U : 0U; }
}
uint8_t Control_GetLoopMode(uint8_t l) { return g_loop[l % CTL_LOOPS]; }

ctl_set_result_t Control_SetGainsFor(uint8_t l, uint8_t p, float kp, float ki, float kd)
{
    uint8_t lo, hi;
    if (!LoopRange(l, &lo, &hi) || p >= CL_PLANT_COUNT || kp < 0.0f || ki < 0.0f || kd < 0.0f) { return CTL_SET_RANGE; }
    for (uint8_t i = lo; i < hi; i++) { g_gain[i][p][0] = kp; g_gain[i][p][1] = ki; g_gain[i][p][2] = kd; }
    return CTL_SET_OK;
}
void Control_GetGainsFor(uint8_t l, uint8_t p, float *kp, float *ki, float *kd)
{
    *kp = g_gain[l % CTL_LOOPS][p][0]; *ki = g_gain[l % CTL_LOOPS][p][1]; *kd = g_gain[l % CTL_LOOPS][p][2];
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
