/*
 * settings.c -- see settings.h.
 */
#include "settings.h"
#include "control_runtime.h"
#include "supply_mode.h"
#include "shot_profile.h"
#include "target_profile.h"
#include "hvps_curve.h"
#include "pfm.h"
#include <stdio.h>
#include <string.h>

/* the codec's plant numbering is the controller's */
typedef char settings_plant_hvps_check[(SET_PLANT_HVPS == CL_PLANT_HVPS) ? 1 : -1];
typedef char settings_plant_lim_check[(SET_PLANT_LIMITER == CL_PLANT_LIMITER) ? 1 : -1];
typedef char settings_plant_count_check[(SET_PLANT_COUNT == CL_PLANT_COUNT) ? 1 : -1];

static settings_snapshot_t g_snap;          /* working snapshot for Save/Recall (not on the stack) */
static hvps_curve_t        g_curveStage;    /* a recalled curve is built here and committed only if valid */

static uint8_t Present(const settings_snapshot_t *s, uint8_t id, uint8_t ch)
{
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        if (kSettingsFields[fi].id == id)
        {
            return SettingsCodec_IsPresent(s, fi, ch);
        }
    }
    return 0U;
}

static void MarkPresent(settings_snapshot_t *s, uint8_t id, uint8_t ch)
{
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        if (kSettingsFields[fi].id == id)
        {
            SettingsCodec_SetPresent(s, fi, ch);
            return;
        }
    }
}

void Settings_Capture(settings_snapshot_t *s)
{
    SettingsCodec_Clear(s);

    s->supplyMode    = (uint8_t)SupplyMode_Get();
    s->pidRateHz     = Control_GetRateHz();
    s->adcScans      = Control_GetScansPerTick();
    s->adcSampleX10  = Control_GetSampleTimeX10();
    s->slewPerTick   = Control_GetSlewPerTick();
    s->maxVolts      = Control_GetMaxVolts();
    s->fbChannel     = Control_GetFeedbackChannel(0U);
    s->fbScale       = Control_GetFeedbackScale(0U);
    s->fbOffsetV     = Control_GetFeedbackOffset(0U);
    s->fbFilterHz    = Control_GetFeedbackFilterHz();
    s->fbClipMs      = Control_GetClipMs();
    s->loopMode      = Control_GetLoopMode(0U);
    s->clHvpsDuty    = Control_GetHvpsDuty();
    s->clLimiterFreq = Control_GetLimiterFreq();
    s->clResonanceHz = Control_GetHvpsResonance();
    s->clVpeak       = Control_GetHvpsVpeak();
    s->clVscale      = Control_GetLimiterVscale();
    s->clBranch      = Control_GetHvpsAbove();

    /* Boot state has no supply mode (NONE); a profile saved then simply has no mode field. */
    if (s->supplyMode != (uint8_t)SUPPLY_MODE_NONE)
    {
        MarkPresent(s, SETF_SUPPLY_MODE, 0U);
    }
    MarkPresent(s, SETF_PID_RATE_HZ, 0U);
    MarkPresent(s, SETF_ADC_SCANS, 0U);
    MarkPresent(s, SETF_ADC_SAMPLE_X10, 0U);
    MarkPresent(s, SETF_SLEW_PER_TICK, 0U);
    MarkPresent(s, SETF_MAX_VOLTS, 0U);
    MarkPresent(s, SETF_FB_CHANNEL, 0U);
    MarkPresent(s, SETF_FB_SCALE, 0U);
    MarkPresent(s, SETF_FB_OFFSET_V, 0U);
    MarkPresent(s, SETF_FB_FILTER_HZ, 0U);
    MarkPresent(s, SETF_FB_CLIP_MS, 0U);
    MarkPresent(s, SETF_LOOP_MODE, 0U);
    MarkPresent(s, SETF_CL_HVPS_DUTY, 0U);
    MarkPresent(s, SETF_CL_LIMITER_FREQ, 0U);
    MarkPresent(s, SETF_CL_RESONANCE_HZ, 0U);
    MarkPresent(s, SETF_CL_VPEAK, 0U);
    MarkPresent(s, SETF_CL_VSCALE, 0U);
    MarkPresent(s, SETF_CL_BRANCH, 0U);

    for (uint8_t p = 0U; p < SET_PLANT_COUNT; p++)
    {
        Control_GetGainsFor(0U, p, &s->plant[p].kp, &s->plant[p].ki, &s->plant[p].kd);
        MarkPresent(s, SETF_KP, p);
        MarkPresent(s, SETF_KI, p);
        MarkPresent(s, SETF_KD, p);
    }

    /* loops 2 and 3 */
    for (uint8_t x = 0U; x < SET_XLOOPS; x++)
    {
        uint8_t loop = (uint8_t)(x + 1U);

        s->xloop[x].loopMode  = Control_GetLoopMode(loop);
        s->xloop[x].fbChannel = Control_GetFeedbackChannel(loop);
        s->xloop[x].fbScale   = Control_GetFeedbackScale(loop);
        s->xloop[x].fbOffsetV = Control_GetFeedbackOffset(loop);
        MarkPresent(s, SETF_X_LOOP_MODE, x);
        MarkPresent(s, SETF_X_FB_CHANNEL, x);
        MarkPresent(s, SETF_X_FB_SCALE, x);
        MarkPresent(s, SETF_X_FB_OFFSET_V, x);
        for (uint8_t p = 0U; p < SET_PLANT_COUNT; p++)
        {
            uint8_t c = (uint8_t)(x * SET_PLANT_COUNT + p);

            Control_GetGainsFor(loop, p, &s->xplant[c].kp, &s->xplant[c].ki, &s->xplant[c].kd);
            MarkPresent(s, SETF_X_KP, c);
            MarkPresent(s, SETF_X_KI, c);
            MarkPresent(s, SETF_X_KD, c);
        }
    }
}

static void Record(settings_apply_result_t *r, uint8_t ok, const char *name, uint8_t ch)
{
    static const char *const plant[SET_PLANT_COUNT] = { "hvps", "lim" };
    /* ch: SET_CHANNEL_SYSTEM, a plant (0..1), or 0x80 | extra-loop index for a loop 2/3 value (0x40 | plant + 2 x loop for gains) */

    if (ok != 0U)
    {
        r->applied++;
        return;
    }
    r->failed++;
    {
        size_t used = strlen(r->failedNames);
        char item[40];
        size_t n;

        if (ch == SET_CHANNEL_SYSTEM)
        {
            n = (size_t)snprintf(item, sizeof(item), "%s", name);
        }
        else if ((ch & 0x80U) != 0U)
        {
            n = (size_t)snprintf(item, sizeof(item), "loop%u.%s", (unsigned)((ch & 0x7FU) + 2U), name);
        }
        else if ((ch & 0x40U) != 0U)
        {
            uint8_t c = (uint8_t)(ch & 0x3FU);

            n = (size_t)snprintf(item, sizeof(item), "loop%u.%s.%s", (unsigned)(c / SET_PLANT_COUNT + 2U),
                                 plant[c % SET_PLANT_COUNT], name);
        }
        else
        {
            n = (size_t)snprintf(item, sizeof(item), "%s.%s", plant[(ch < SET_PLANT_COUNT) ? ch : 0U], name);
        }
        if (n < sizeof(item) && used + n + 2U < sizeof(r->failedNames))
        {
            if (used != 0U)
            {
                r->failedNames[used++] = ',';
            }
            memcpy(&r->failedNames[used], item, n + 1U);
        }
    }
}

#define OK(expr)  ((uint8_t)((expr) == CTL_SET_OK))

/* PIDRate, ADC:AVERage and ADC:SAMPle are checked against each other (the scans must fit in a tick), so a
   value refused against the OTHER settings' current values is retried after they have changed, in the other
   order. */
static void ApplyTiming(const settings_snapshot_t *s, settings_apply_result_t *r)
{
    uint8_t hasS = Present(s, SETF_ADC_SAMPLE_X10, 0U);
    uint8_t hasA = Present(s, SETF_ADC_SCANS, 0U);
    uint8_t hasR = Present(s, SETF_PID_RATE_HZ, 0U);
    uint8_t okS = (uint8_t)(hasS == 0U);
    uint8_t okA = (uint8_t)(hasA == 0U);
    uint8_t okR = (uint8_t)(hasR == 0U);

    for (uint8_t pass = 0U; pass < 4U && (okS == 0U || okA == 0U || okR == 0U); pass++)
    {
        if ((pass & 1U) == 0U)
        {
            if (okS == 0U) { okS = OK(Control_SetSampleTimeX10((uint16_t)s->adcSampleX10)); }
            if (okA == 0U) { okA = OK(Control_SetScansPerTick((uint16_t)s->adcScans)); }
            if (okR == 0U) { okR = OK(Control_SetRateHz(s->pidRateHz)); }
        }
        else
        {
            if (okR == 0U) { okR = OK(Control_SetRateHz(s->pidRateHz)); }
            if (okA == 0U) { okA = OK(Control_SetScansPerTick((uint16_t)s->adcScans)); }
            if (okS == 0U) { okS = OK(Control_SetSampleTimeX10((uint16_t)s->adcSampleX10)); }
        }
    }
    if (hasS != 0U) { Record(r, okS, "ADC:SAMPle", SET_CHANNEL_SYSTEM); }
    if (hasA != 0U) { Record(r, okA, "ADC:AVERage", SET_CHANNEL_SYSTEM); }
    if (hasR != 0U) { Record(r, okR, "PIDRate", SET_CHANNEL_SYSTEM); }
}

void Settings_Apply(const settings_snapshot_t *s, settings_apply_result_t *r, uint8_t *modeChanged)
{
    memset(r, 0, sizeof(*r));
    *modeChanged = 0U;

    /* the supply mode first: the tables are checked against it */
    if (Present(s, SETF_SUPPLY_MODE, 0U) != 0U)
    {
        uint8_t ok = (uint8_t)((s->supplyMode == (uint8_t)SUPPLY_MODE_LIMITER) ||
                               (s->supplyMode == (uint8_t)SUPPLY_MODE_HVPS));

        if (ok != 0U)
        {
            *modeChanged = SupplyMode_Set((supply_mode_t)s->supplyMode);
        }
        Record(r, ok, "MODE", SET_CHANNEL_SYSTEM);
    }

    ApplyTiming(s, r);

    if (Present(s, SETF_SLEW_PER_TICK, 0U) != 0U)
    {
        Record(r, OK(Control_SetSlewPerTick(s->slewPerTick)), "SLEWRate", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_MAX_VOLTS, 0U) != 0U)
    {
        Record(r, OK(Control_SetMaxVolts(s->maxVolts)), "MAXVolts", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_FB_CHANNEL, 0U) != 0U)
    {
        Record(r, OK(Control_SetFeedbackChannel(0U, s->fbChannel)), "FEEDback:CHANnel", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_FB_SCALE, 0U) != 0U)
    {
        Record(r, OK(Control_SetFeedbackScale(0U, s->fbScale)), "FEEDback:SCALE", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_FB_OFFSET_V, 0U) != 0U)
    {
        Record(r, OK(Control_SetFeedbackOffset(0U, s->fbOffsetV)), "FEEDback:OFFSet", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_FB_FILTER_HZ, 0U) != 0U)       /* after PIDRate: the cutoff is checked against it */
    {
        Record(r, OK(Control_SetFeedbackFilterHz(s->fbFilterHz)), "FEEDback:FILTer", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_FB_CLIP_MS, 0U) != 0U)
    {
        Record(r, OK(Control_SetClipMs(s->fbClipMs)), "FEEDback:CLIPTime", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_CL_HVPS_DUTY, 0U) != 0U)
    {
        Record(r, OK(Control_SetHvpsDuty(s->clHvpsDuty)), "CLOSEDloop:DUTY", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_CL_LIMITER_FREQ, 0U) != 0U)
    {
        Record(r, OK(Control_SetLimiterFreq(s->clLimiterFreq)), "CLOSEDloop:FREQuency", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_CL_RESONANCE_HZ, 0U) != 0U)
    {
        Record(r, OK(Control_SetHvpsResonance(s->clResonanceHz)), "CLOSEDloop:RESonance", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_CL_VPEAK, 0U) != 0U)
    {
        Record(r, OK(Control_SetHvpsVpeak(s->clVpeak)), "CLOSEDloop:VPEAK", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_CL_VSCALE, 0U) != 0U)
    {
        Record(r, OK(Control_SetLimiterVscale(s->clVscale)), "CLOSEDloop:VSCALE", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_CL_BRANCH, 0U) != 0U)
    {
        Record(r, OK(Control_SetHvpsAbove(s->clBranch)), "CLOSEDloop:BRANCH", SET_CHANNEL_SYSTEM);
    }
    for (uint8_t p = 0U; p < SET_PLANT_COUNT; p++)
    {
        if (Present(s, SETF_KP, p) != 0U && Present(s, SETF_KI, p) != 0U && Present(s, SETF_KD, p) != 0U)
        {
            Record(r, OK(Control_SetGainsFor(0U, p, s->plant[p].kp, s->plant[p].ki, s->plant[p].kd)), "Gains", p);
        }
    }
    if (Present(s, SETF_LOOP_MODE, 0U) != 0U)
    {
        Control_SetLoopMode(0U, s->loopMode);
        Record(r, 1U, "LOOPMODE", SET_CHANNEL_SYSTEM);
    }

    /* loops 2 and 3, from the saves that have them */
    for (uint8_t x = 0U; x < SET_XLOOPS; x++)
    {
        uint8_t loop = (uint8_t)(x + 1U);
        uint8_t tag = (uint8_t)(0x80U | x);

        if (Present(s, SETF_X_FB_CHANNEL, x) != 0U)
        {
            Record(r, OK(Control_SetFeedbackChannel(loop, s->xloop[x].fbChannel)), "FEEDback:CHANnel", tag);
        }
        if (Present(s, SETF_X_FB_SCALE, x) != 0U)
        {
            Record(r, OK(Control_SetFeedbackScale(loop, s->xloop[x].fbScale)), "FEEDback:SCALE", tag);
        }
        if (Present(s, SETF_X_FB_OFFSET_V, x) != 0U)
        {
            Record(r, OK(Control_SetFeedbackOffset(loop, s->xloop[x].fbOffsetV)), "FEEDback:OFFSet", tag);
        }
        for (uint8_t p = 0U; p < SET_PLANT_COUNT; p++)
        {
            uint8_t c = (uint8_t)(x * SET_PLANT_COUNT + p);

            if (Present(s, SETF_X_KP, c) != 0U && Present(s, SETF_X_KI, c) != 0U && Present(s, SETF_X_KD, c) != 0U)
            {
                Record(r, OK(Control_SetGainsFor(loop, p, s->xplant[c].kp, s->xplant[c].ki, s->xplant[c].kd)), "Gains",
                       (uint8_t)(0x40U | c));
            }
        }
        if (Present(s, SETF_X_LOOP_MODE, x) != 0U)
        {
            Control_SetLoopMode(loop, s->xloop[x].loopMode);
            Record(r, 1U, "LOOPMODE", tag);
        }
    }
}

/* ---- the tables ---- */

enum { TBL_SHOT = 0, TBL_TARGET, TBL_CURVE, TBL_COUNT };

/* Each table has up to two wire formats. Format 0 is the original (one duty / voltage for every phase); format 1
   carries one per phase and is written only when the phases differ, so an ordinary profile is saved byte for byte as
   before. A recall accepts either. */
#define TBL_FORMATS  (2U)

typedef struct
{
    uint8_t  countId[TBL_FORMATS];
    uint8_t  pointsId[TBL_FORMATS];
    uint8_t  pointBytes[TBL_FORMATS];
    uint8_t  chunkPoints[TBL_FORMATS];
    uint8_t  formats;
    const char *name;
} table_spec_t;

static const table_spec_t kTable[TBL_COUNT] =
{
    { { SETF_SHOT_COUNT,   SETF_SHOT3_COUNT   }, { SETF_SHOT_POINTS,   SETF_SHOT3_POINTS   },
      { SET_SHOT_POINT_BYTES,   SET_SHOT3_POINT_BYTES   }, { SET_SHOT_CHUNK_POINTS,   SET_SHOT3_CHUNK_POINTS   }, 2U, "SHOT"   },
    { { SETF_TARGET_COUNT, SETF_TARGET3_COUNT }, { SETF_TARGET_POINTS, SETF_TARGET3_POINTS },
      { SET_TARGET_POINT_BYTES, SET_TARGET3_POINT_BYTES }, { SET_TARGET_CHUNK_POINTS, SET_TARGET3_CHUNK_POINTS }, 2U, "TARGET" },
    { { SETF_CURVE_COUNT,  0U                 }, { SETF_CURVE_POINTS,  0U                  },
      { SET_CURVE_POINT_BYTES,  0U                      }, { SET_CURVE_CHUNK_POINTS,  0U                       }, 1U, "CURVE"  }
};

#define MAX_POINT_BYTES  (16U)       /* the largest of the formats' points */
typedef char settings_point_bytes_check[((SET_SHOT3_POINT_BYTES <= MAX_POINT_BYTES) && (SET_TARGET3_POINT_BYTES <= MAX_POINT_BYTES)) ? 1 : -1];

/* The format a save of the live table would use. */
static uint8_t FormatFor(uint8_t t)
{
    if (t == TBL_SHOT)
    {
        return (ShotProfile_IsUniform() != 0U) ? 0U : 1U;
    }
    if (t == TBL_TARGET)
    {
        return (Target_IsUniform() != 0U) ? 0U : 1U;
    }
    return 0U;
}

static void PackFloat(uint8_t *p, float v)
{
    uint32_t u;

    memcpy(&u, &v, sizeof(u));
    SettingsCodec_Put32(p, u);
}

static float UnpackFloat(const uint8_t *p)
{
    uint32_t u = SettingsCodec_Get32(p);
    float v;

    memcpy(&v, &u, sizeof(v));
    return v;
}

/* The live table's point count, and point i packed into out (pointBytes of the format long). */
static uint16_t LiveCount(uint8_t t)
{
    switch (t)
    {
    case TBL_SHOT:   return ShotProfile_Count();
    case TBL_TARGET: return Target_Count();
    default:         return (Control_Loop()->cfg.curveCustom != 0U) ? Control_Loop()->cfg.curve.n : 0U;
    }
}

static void LivePack(uint8_t t, uint8_t fmt, uint16_t i, uint8_t *out)
{
    memset(out, 0, kTable[t].pointBytes[fmt]);
    if (t == TBL_SHOT)
    {
        shot_setpoint_t sp;

        if (ShotProfile_Get(i, &sp) != 0U)
        {
            SettingsCodec_Put32(&out[0], sp.tUs);
            SettingsCodec_Put32(&out[4], sp.freqMilliHz);
            for (uint8_t ch = 0U; ch < ((fmt == 0U) ? 1U : HRTIM_NUM_CHANNELS); ch++)
            {
                SettingsCodec_Put16(&out[8U + 2U * ch], sp.dutyHundredths[ch]);
            }
        }
    }
    else if (t == TBL_TARGET)
    {
        target_setpoint_t sp;

        if (Target_Get(i, &sp) != 0U)
        {
            SettingsCodec_Put32(&out[0], sp.tUs);
            for (uint8_t ch = 0U; ch < ((fmt == 0U) ? 1U : HRTIM_NUM_CHANNELS); ch++)
            {
                SettingsCodec_Put32(&out[4U + 4U * ch], (uint32_t)sp.milliVolts[ch]);
            }
        }
    }
    else
    {
        const hvps_curve_t *c = &Control_Loop()->cfg.curve;

        if (i < c->n)
        {
            PackFloat(&out[0], c->hz[i]);
            PackFloat(&out[4], c->volts[i]);
        }
    }
}

/* Emits one table: its COUNT record, then POINTS records of up to chunkPoints points each. */
static uint8_t EmitTable(settings_sink_fn sink, void *ctx, uint8_t t)
{
    const table_spec_t *sp = &kTable[t];
    uint8_t fmt = FormatFor(t);
    uint16_t count = LiveCount(t);
    uint8_t cnt[2];
    uint8_t chunk[255];
    uint16_t i = 0U;
    uint8_t ci = 0U;

    SettingsCodec_Put16(cnt, count);
    if (SettingsCodec_EmitRecord(sink, ctx, sp->countId[fmt], SET_CHANNEL_SYSTEM, cnt, 2U) == 0U)
    {
        return 0U;
    }
    while (i < count)
    {
        uint8_t n = 0U;

        while (n < sp->chunkPoints[fmt] && i < count)
        {
            LivePack(t, fmt, i, &chunk[(uint16_t)n * sp->pointBytes[fmt]]);
            n++;
            i++;
        }
        if (SettingsCodec_EmitRecord(sink, ctx, sp->pointsId[fmt], ci, chunk, (uint8_t)(n * sp->pointBytes[fmt])) == 0U)
        {
            return 0U;
        }
        ci++;
    }
    return 1U;
}

static uint8_t EmitAll(const settings_snapshot_t *s, settings_sink_fn sink, void *ctx)
{
    return (uint8_t)(SettingsCodec_EncodeScalars(s, sink, ctx) != 0U && EmitTable(sink, ctx, TBL_SHOT) != 0U &&
                     EmitTable(sink, ctx, TBL_TARGET) != 0U && EmitTable(sink, ctx, TBL_CURVE) != 0U);
}

void Settings_LiveTables(settings_tables_t *t)
{
    settings_table_sum_t *out[TBL_COUNT] = { &t->shot, &t->target, &t->curve };

    for (uint8_t k = 0U; k < TBL_COUNT; k++)
    {
        uint8_t pt[MAX_POINT_BYTES];
        uint8_t fmt = FormatFor(k);

        out[k]->count = LiveCount(k);
        out[k]->crc = 0U;
        for (uint16_t i = 0U; i < out[k]->count; i++)
        {
            LivePack(k, fmt, i, pt);
            out[k]->crc = SettingsCodec_Crc32(out[k]->crc, pt, kTable[k].pointBytes[fmt]);
        }
    }
}

/* ---- save ---- */

typedef struct
{
    uint32_t total;
    profile_writer_t *w;
    profile_status_t rc;
} save_ctx_t;

static uint8_t CountSink(void *ctx, const uint8_t *data, uint16_t n)
{
    (void)data;
    ((save_ctx_t *)ctx)->total += n;
    return 1U;
}

static uint8_t WriteSink(void *ctx, const uint8_t *data, uint16_t n)
{
    save_ctx_t *c = (save_ctx_t *)ctx;

    c->rc = ProfileWriter_Write(c->w, data, n);
    return (uint8_t)(c->rc == PROFILE_OK);
}

profile_status_t Settings_Save(uint8_t slot, const char *label, const char *fw)
{
    save_ctx_t c = { 0U, NULL, PROFILE_OK };
    profile_writer_t w;
    profile_status_t rc;

    Settings_Capture(&g_snap);
    (void)EmitAll(&g_snap, CountSink, &c);          /* pass 1: how long is it */
    if (c.total > PROFILE_PAYLOAD_MAX)
    {
        return PROFILE_ERR_TOO_BIG;
    }
    if ((rc = ProfileWriter_Begin(&w, slot, label, fw, (uint16_t)c.total)) != PROFILE_OK)
    {
        return rc;
    }
    c.w = &w;
    c.rc = PROFILE_OK;
    if (EmitAll(&g_snap, WriteSink, &c) == 0U)       /* pass 2: write it */
    {
        return (c.rc != PROFILE_OK) ? c.rc : PROFILE_ERR_USAGE;
    }
    return ProfileWriter_End(&w);
}

/* ---- reading a slot ---- */

typedef struct
{
    const profile_reader_t *r;
    uint8_t ioFailed;
} rd_ctx_t;

static uint8_t RdFn(void *ctx, uint16_t offset, void *buf, uint16_t n)
{
    rd_ctx_t *c = (rd_ctx_t *)ctx;

    if (ProfileReader_Read(c->r, offset, buf, n) != PROFILE_OK)
    {
        c->ioFailed = 1U;
        return 0U;
    }
    return 1U;
}

profile_status_t Settings_Inspect(uint8_t slot, settings_snapshot_t *s, settings_tables_t *t,
                                  settings_decode_info_t *dinfo, profile_info_t *info)
{
    profile_reader_t r;
    rd_ctx_t ctx;
    settings_record_t rec;
    settings_decode_info_t local = {0U, 0U, 0U, 0U};
    settings_table_sum_t *sums[TBL_COUNT] = { &t->shot, &t->target, &t->curve };
    uint16_t offset = 0U;
    settings_rec_status_t st;
    profile_status_t rc = ProfileReader_Open(slot, &r, info);

    if (rc != PROFILE_OK)
    {
        return rc;
    }
    ctx.r = &r;
    ctx.ioFailed = 0U;
    SettingsCodec_Clear(s);
    memset(t, 0, sizeof(*t));
    while ((st = SettingsCodec_NextRecord(RdFn, &ctx, r.len, &offset, &rec)) == SETREC_OK)
    {
        if (SettingsCodec_StoreRecord(s, &rec, &local) == 0U)
        {
            return PROFILE_ERR_CORRUPT;
        }
        for (uint8_t k = 0U; k < TBL_COUNT; k++)
        {
            for (uint8_t f = 0U; f < kTable[k].formats; f++)
            {
                if (rec.id == kTable[k].countId[f] && rec.len == 2U)
                {
                    sums[k]->count = SettingsCodec_Get16(rec.value);
                }
                else if (rec.id == kTable[k].pointsId[f])
                {
                    sums[k]->crc = SettingsCodec_Crc32(sums[k]->crc, rec.value, rec.len);
                }
            }
        }
    }
    if (st != SETREC_END)
    {
        return (ctx.ioFailed != 0U) ? PROFILE_ERR_STORAGE : PROFILE_ERR_CORRUPT;
    }
    if (dinfo != NULL)
    {
        *dinfo = local;
    }
    return PROFILE_OK;
}

/* ---- recall ---- */

typedef struct
{
    uint16_t expect;          /* from the COUNT record */
    uint16_t got;
    uint8_t  seen;            /* the COUNT record was there */
    uint8_t  fmt;             /* the wire format its COUNT record announced */
    uint8_t  failed;
} table_state_t;

static void FailTable(settings_apply_result_t *r, uint8_t t)
{
    Record(r, 0U, kTable[t].name, SET_CHANNEL_SYSTEM);
}

static void FinishTable(uint8_t t, table_state_t *ts, settings_apply_result_t *r)
{
    if (ts->seen == 0U)
    {
        return;                                  /* the save has no such table: leave the live one alone */
    }
    if (ts->failed == 0U && ts->got != ts->expect)
    {
        ts->failed = 1U;                         /* the chunks did not add up to the count */
    }
    if (t == TBL_CURVE)
    {
        if (ts->failed == 0U && ts->expect == 0U)
        {
            Control_Loop()->cfg.curveCustom = 0U;                        /* back to the linear default */
        }
        else if (ts->failed == 0U && HvpsCurve_IsValid(&g_curveStage) != 0U)
        {
            Control_Loop()->cfg.curve = g_curveStage;
            Control_Loop()->cfg.curveCustom = 1U;
        }
        else
        {
            ts->failed = 1U;                                              /* the live curve is left untouched */
        }
    }
    else if (ts->failed != 0U)
    {
        if (t == TBL_SHOT) { ShotProfile_Clear(); } else { Target_Clear(); }   /* never leave a half-loaded series */
    }
    else if (t == TBL_SHOT && ShotProfile_IsComplete() != 0U)
    {
        PFM_SelectSource(PFM_SOURCE_PROFILE);    /* what SHOT:END does: FIRE plays the shot just loaded */
    }
    if (ts->failed != 0U)
    {
        FailTable(r, t);
    }
    else
    {
        r->applied++;
    }
}

static uint8_t ApplyPoint(uint8_t t, uint8_t fmt, const uint8_t *p)
{
    if (t == TBL_SHOT)
    {
        if (fmt == 0U)
        {
            return (uint8_t)(ShotProfile_AppendUniform(SettingsCodec_Get32(&p[0]), SettingsCodec_Get32(&p[4]),
                                                       SettingsCodec_Get16(&p[8])) == SHOT_APPEND_OK);
        }
        {
            uint16_t d[HRTIM_NUM_CHANNELS];

            for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
            {
                d[ch] = SettingsCodec_Get16(&p[8U + 2U * ch]);
            }
            return (uint8_t)(ShotProfile_Append(SettingsCodec_Get32(&p[0]), SettingsCodec_Get32(&p[4]), d) == SHOT_APPEND_OK);
        }
    }
    if (t == TBL_TARGET)
    {
        int32_t limit = (int32_t)(Control_GetMaxVolts() * 1000.0f);

        if (fmt == 0U)
        {
            return (uint8_t)(Target_AppendUniform(SettingsCodec_Get32(&p[0]), (int32_t)SettingsCodec_Get32(&p[4]), limit) ==
                             TARGET_APPEND_OK);
        }
        {
            int32_t v[HRTIM_NUM_CHANNELS];

            for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
            {
                v[ch] = (int32_t)SettingsCodec_Get32(&p[4U + 4U * ch]);
            }
            return (uint8_t)(Target_Append(SettingsCodec_Get32(&p[0]), v, limit) == TARGET_APPEND_OK);
        }
    }
    return (uint8_t)(HvpsCurve_Append(&g_curveStage, UnpackFloat(&p[0]), UnpackFloat(&p[4])) == HVPS_CURVE_OK);
}

profile_status_t Settings_Recall(uint8_t slot, settings_recall_result_t *res)
{
    profile_reader_t r;
    rd_ctx_t ctx;
    settings_record_t rec;
    settings_decode_info_t dinfo = {0U, 0U, 0U, 0U};
    table_state_t ts[TBL_COUNT];
    uint16_t offset = 0U;
    settings_rec_status_t st;
    profile_status_t rc = ProfileReader_Open(slot, &r, NULL);

    memset(res, 0, sizeof(*res));
    if (rc != PROFILE_OK)
    {
        return rc;
    }
    ctx.r = &r;
    ctx.ioFailed = 0U;

    /* pass 1: the scalars (the whole payload is framed and its tables skipped, so nothing is applied from a
       payload that turns out to be malformed) */
    SettingsCodec_Clear(&g_snap);
    while ((st = SettingsCodec_NextRecord(RdFn, &ctx, r.len, &offset, &rec)) == SETREC_OK)
    {
        if (SettingsCodec_StoreRecord(&g_snap, &rec, &dinfo) == 0U)
        {
            return PROFILE_ERR_CORRUPT;
        }
    }
    if (st != SETREC_END)
    {
        return (ctx.ioFailed != 0U) ? PROFILE_ERR_STORAGE : PROFILE_ERR_CORRUPT;
    }
    res->ignored = (uint16_t)(dinfo.unknown + dinfo.outOfRange);
    Settings_Apply(&g_snap, &res->apply, &res->modeChanged);

    /* pass 2: the tables, applied through their own validated append functions in the order they were saved */
    memset(ts, 0, sizeof(ts));
    HvpsCurve_Clear(&g_curveStage);
    offset = 0U;
    while ((st = SettingsCodec_NextRecord(RdFn, &ctx, r.len, &offset, &rec)) == SETREC_OK)
    {
        for (uint8_t k = 0U; k < TBL_COUNT; k++)
        {
            for (uint8_t f = 0U; f < kTable[k].formats; f++)
            {
                if (rec.id == kTable[k].countId[f] && rec.len == 2U)
                {
                    ts[k].expect = SettingsCodec_Get16(rec.value);
                    ts[k].seen = 1U;
                    ts[k].got = 0U;
                    ts[k].failed = 0U;
                    ts[k].fmt = f;
                    if (k == TBL_SHOT) { ShotProfile_Clear(); }
                    else if (k == TBL_TARGET) { Target_Clear(); }
                    else { HvpsCurve_Clear(&g_curveStage); }
                }
                else if (rec.id == kTable[k].pointsId[f] && ts[k].seen != 0U && ts[k].failed == 0U && ts[k].fmt == f)
                {
                    uint8_t pb = kTable[k].pointBytes[f];
                    uint8_t n = (uint8_t)(rec.len / pb);

                    if ((rec.len % pb) != 0U)
                    {
                        ts[k].failed = 1U;
                        continue;
                    }
                    for (uint8_t i = 0U; i < n; i++)
                    {
                        if (ApplyPoint(k, f, &rec.value[(uint16_t)i * pb]) == 0U)
                        {
                            ts[k].failed = 1U;
                            break;
                        }
                        ts[k].got++;
                    }
                }
            }
        }
    }
    if (st != SETREC_END)
    {
        return (ctx.ioFailed != 0U) ? PROFILE_ERR_STORAGE : PROFILE_ERR_CORRUPT;
    }
    for (uint8_t k = 0U; k < TBL_COUNT; k++)
    {
        FinishTable(k, &ts[k], &res->apply);
    }
    return PROFILE_OK;
}
