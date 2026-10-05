/*
 * test_settings.c -- the saved-profile glue (src/app/settings/settings.c): a whole profile -- scalars and the
 * shot, target and HVPS-curve tables -- saved to the RAM fake flash and recalled, the order of application, the
 * partial-failure rules, and a power cut during a save. The control layer is fake_control.c.
 */
#include "settings.h"
#include "fake_nvstore.h"
#include "control_runtime.h"
#include "supply_mode.h"
#include "shot_profile.h"
#include "target_profile.h"
#include "hvps_curve.h"
#include "pfm.h"
#include "test_util.h"
#include <string.h>

void FakeControl_Reset(void);

static int SnapEqual(const settings_snapshot_t *a, const settings_snapshot_t *b)
{
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        for (uint8_t c = 0U; c < ((kSettingsFields[fi].perPlant != 0U) ? SET_PLANT_COUNT : 1U); c++)
        {
            if (SettingsCodec_IsPresent(a, fi, c) != SettingsCodec_IsPresent(b, fi, c)) { return 0; }
            if (SettingsCodec_IsPresent(a, fi, c) != 0U && SettingsCodec_ValueEqual(a, b, fi, c) == 0U) { return 0; }
        }
    }
    return 1;
}

static int TablesEqual(const settings_tables_t *a, const settings_tables_t *b)
{
    return a->shot.count == b->shot.count && a->shot.crc == b->shot.crc &&
           a->target.count == b->target.count && a->target.crc == b->target.crc &&
           a->curve.count == b->curve.count && a->curve.crc == b->curve.crc;
}

static void ResetAll(void)
{
    FakeNv_Reset();
    FakeControl_Reset();
    ShotProfile_Clear();
    Target_Clear();
    (void)SupplyMode_Set(SUPPLY_MODE_NONE);
}

/* an HVPS-legal shot of n points over (n-1)*step us, frequency and duty ramping inside the HVPS limits */
static int BuildShot(uint16_t n, uint32_t stepUs)
{
    for (uint16_t i = 0U; i < n; i++)
    {
        uint32_t f = 10000000UL + (uint32_t)i * 37U;                 /* mHz */
        uint16_t d = (uint16_t)(4000U + (i % 50U) * 40U);            /* 40 % .. 59.6 % */

        if (ShotProfile_Append(i * stepUs, f, d) != SHOT_APPEND_OK) { return 0; }
    }
    return 1;
}

static int BuildTarget(uint16_t n)
{
    for (uint16_t i = 0U; i < n; i++)
    {
        if (Target_Append(i * 20000U, (int32_t)(200 + 150 * (int32_t)i), 1000000) != TARGET_APPEND_OK) { return 0; }
    }
    return 1;
}

static void BuildCurve(float peakHz)
{
    hvps_curve_t *c = &Control_Loop()->cfg.curve;

    HvpsCurve_Clear(c);
    for (int f = 10000; f <= 30000; f += 1000)
    {
        float x = ((float)f - peakHz) / 3000.0f;
        (void)HvpsCurve_Append(c, (float)f, 1.5f / (1.0f + x * x));
    }
    Control_Loop()->cfg.curveCustom = 1U;
}

static void SetLiveState(void)
{
    (void)SupplyMode_Set(SUPPLY_MODE_HVPS);
    (void)Control_SetMaxVolts(900.0f);
    (void)Control_SetSampleTimeX10(925U);
    (void)Control_SetScansPerTick(3U);
    (void)Control_SetRateHz(8000U);
    (void)Control_SetSlewPerTick(12.5f);
    (void)Control_SetFeedbackChannel(4U);
    (void)Control_SetFeedbackScale(620.69f);
    (void)Control_SetFeedbackOffset(1.45f);
    (void)Control_SetFeedbackFilterHz(250.0f);
    (void)Control_SetClipMs(2.0f);
    (void)Control_SetHvpsDuty(47.5f);
    (void)Control_SetLimiterFreq(90000.0f);
    (void)Control_SetHvpsResonance(19250.0f);
    (void)Control_SetHvpsVpeak(1.5f);
    (void)Control_SetLimiterVscale(900.0f);
    (void)Control_SetHvpsAbove(0U);
    (void)Control_SetGainsFor(CL_PLANT_HVPS, 4000.0f, 200000.0f, 0.001f);
    (void)Control_SetGainsFor(CL_PLANT_LIMITER, 0.167f, 6.667f, 0.0f);
    Control_SetLoopMode(1U);
}

static void ScrambleLiveState(void)
{
    ShotProfile_Clear();
    Target_Clear();
    Control_Loop()->cfg.curveCustom = 0U;
    (void)SupplyMode_Set(SUPPLY_MODE_LIMITER);
    (void)Control_SetRateHz(10000U);
    (void)Control_SetScansPerTick(4U);
    (void)Control_SetSampleTimeX10(475U);
    (void)Control_SetMaxVolts(4.0f);
    (void)Control_SetSlewPerTick(0.0f);
    (void)Control_SetFeedbackChannel(1U);
    (void)Control_SetFeedbackScale(1.0f);
    (void)Control_SetFeedbackOffset(0.0f);
    (void)Control_SetFeedbackFilterHz(0.0f);
    (void)Control_SetClipMs(10.0f);
    (void)Control_SetHvpsDuty(50.0f);
    (void)Control_SetLimiterFreq(100000.0f);
    (void)Control_SetHvpsResonance(19000.0f);
    (void)Control_SetHvpsVpeak(1.0f);
    (void)Control_SetLimiterVscale(1.0f);
    (void)Control_SetHvpsAbove(1U);
    (void)Control_SetGainsFor(CL_PLANT_HVPS, 1.0f, 2.0f, 3.0f);
    (void)Control_SetGainsFor(CL_PLANT_LIMITER, 4.0f, 5.0f, 6.0f);
    Control_SetLoopMode(0U);
}

static void TestRoundTrip(void)
{
    settings_snapshot_t before, after, inspected;
    settings_tables_t tBefore, tAfter, tInspected;
    settings_recall_result_t res;
    settings_decode_info_t dinfo;
    profile_info_t info;

    ResetAll();
    SetLiveState();
    CHECK(BuildShot(500U, 1900U) == 1);
    CHECK(BuildTarget(6U) == 1);
    BuildCurve(21000.0f);
    Settings_Capture(&before);
    Settings_LiveTables(&tBefore);
    CHECK(tBefore.shot.count == 500U && tBefore.target.count == 6U && tBefore.curve.count == 21U);

    CHECK(Settings_Save(2U, "round", "v0.9") == PROFILE_OK);
    CHECK(Settings_Inspect(2U, &inspected, &tInspected, &dinfo, &info) == PROFILE_OK);
    CHECK(SnapEqual(&before, &inspected) && TablesEqual(&tBefore, &tInspected));
    CHECK(dinfo.unknown == 0U && dinfo.outOfRange == 0U && dinfo.tables > 0U);
    CHECK_STR(info.label, "round");
    CHECK(info.payloadLen > 500U * SET_SHOT_POINT_BYTES);

    ScrambleLiveState();
    Settings_Capture(&after);
    CHECK(!SnapEqual(&before, &after));

    CHECK(Settings_Recall(2U, &res) == PROFILE_OK);
    CHECK(res.apply.failed == 0U && res.ignored == 0U && res.modeChanged == 1U);
    Settings_Capture(&after);
    Settings_LiveTables(&tAfter);
    CHECK(SnapEqual(&before, &after));
    CHECK(TablesEqual(&tBefore, &tAfter));
    CHECK(Control_Loop()->cfg.curveCustom == 1U);
    CHECK(SupplyMode_Get() == SUPPLY_MODE_HVPS && Control_GetRateHz() == 8000U && Control_GetScansPerTick() == 3U);
    CHECK(ShotProfile_IsComplete() != 0U && Target_IsComplete() != 0U);
    CHECK(PFM_GetSource() == PFM_SOURCE_PROFILE);             /* FIRE plays the recalled shot, as after SHOT:END */

    /* recalling the same profile again changes nothing and does not report the mode as changed */
    CHECK(Settings_Recall(2U, &res) == PROFILE_OK && res.apply.failed == 0U && res.modeChanged == 0U);
    Settings_LiveTables(&tAfter);
    CHECK(TablesEqual(&tBefore, &tAfter));

    /* an empty slot is reported and nothing live is touched */
    CHECK(Settings_Recall(3U, &res) == PROFILE_ERR_EMPTY);
    Settings_LiveTables(&tAfter);
    CHECK(TablesEqual(&tBefore, &tAfter));
}

/* ---- hand-made payloads, for the failure rules ---- */

typedef struct { uint8_t buf[2000]; uint16_t pos; } payload_t;

static uint8_t PSink(void *ctx, const uint8_t *data, uint16_t n)
{
    payload_t *p = (payload_t *)ctx;

    memcpy(&p->buf[p->pos], data, n);
    p->pos = (uint16_t)(p->pos + n);
    return 1U;
}

static void PScalars(payload_t *p, uint8_t mode)
{
    settings_snapshot_t s;

    SettingsCodec_Clear(&s);
    s.supplyMode = mode;
    SettingsCodec_SetPresent(&s, 0U, 0U);
    p->pos = 0U;
    CHECK(SettingsCodec_EncodeScalars(&s, PSink, p) == 1U);
}

static void PCount(payload_t *p, uint8_t id, uint16_t count)
{
    uint8_t c[2];

    SettingsCodec_Put16(c, count);
    CHECK(SettingsCodec_EmitRecord(PSink, p, id, SET_CHANNEL_SYSTEM, c, 2U) == 1U);
}

static void PShotPoint(uint8_t *out, uint32_t t, uint32_t fMilli, uint16_t duty)
{
    SettingsCodec_Put32(&out[0], t);
    SettingsCodec_Put32(&out[4], fMilli);
    SettingsCodec_Put16(&out[8], duty);
}

static void TestFailureRules(void)
{
    payload_t p;
    settings_recall_result_t res;
    uint8_t pts[3U * SET_SHOT_POINT_BYTES];
    settings_tables_t t0, t1;

    /* a shot with an out-of-limits point: that table is left EMPTY (never half loaded), the rest still applies */
    ResetAll();
    SetLiveState();
    CHECK(BuildTarget(6U) == 1);
    BuildCurve(21000.0f);
    PScalars(&p, (uint8_t)SUPPLY_MODE_HVPS);
    PCount(&p, SETF_SHOT_COUNT, 3U);
    PShotPoint(&pts[0], 0U, 20000000UL, 5000U);
    PShotPoint(&pts[10], 1000U, 21000000UL, 5000U);
    PShotPoint(&pts[20], 2000U, 22000000UL, 9000U);                 /* 90 % is outside the HVPS 40-60 % */
    CHECK(SettingsCodec_EmitRecord(PSink, &p, SETF_SHOT_POINTS, 0U, pts, sizeof(pts)) == 1U);
    CHECK(ProfileStore_Write(0U, "bad", "v", p.buf, p.pos) == PROFILE_OK);
    CHECK(BuildShot(10U, 1000U) == 1);                              /* a live shot that must be replaced by the failure */
    CHECK(Settings_Recall(0U, &res) == PROFILE_OK);
    CHECK(res.apply.failed == 1U && strstr(res.apply.failedNames, "SHOT") != NULL);
    CHECK(ShotProfile_Count() == 0U);
    CHECK(PFM_GetSource() == PFM_SOURCE_TABLE);               /* a failed shot does not become the source */
    CHECK(Target_Count() == 6U && Control_Loop()->cfg.curveCustom == 1U);   /* tables the save did not mention are left alone */

    /* a count that the chunks do not add up to */
    ResetAll();
    (void)SupplyMode_Set(SUPPLY_MODE_HVPS);
    PScalars(&p, (uint8_t)SUPPLY_MODE_HVPS);
    PCount(&p, SETF_SHOT_COUNT, 5U);
    CHECK(SettingsCodec_EmitRecord(PSink, &p, SETF_SHOT_POINTS, 0U, pts, 2U * SET_SHOT_POINT_BYTES) == 1U);
    CHECK(ProfileStore_Write(0U, "short", "v", p.buf, p.pos) == PROFILE_OK);
    pts[8] = 0U; pts[9] = 0U;
    PShotPoint(&pts[0], 0U, 20000000UL, 5000U);
    PShotPoint(&pts[10], 1000U, 21000000UL, 5000U);
    CHECK(Settings_Recall(0U, &res) == PROFILE_OK);
    CHECK(res.apply.failed == 1U && strstr(res.apply.failedNames, "SHOT") != NULL && ShotProfile_Count() == 0U);

    /* a point record that is not a whole number of points */
    ResetAll();
    PScalars(&p, (uint8_t)SUPPLY_MODE_HVPS);
    PCount(&p, SETF_SHOT_COUNT, 2U);
    CHECK(SettingsCodec_EmitRecord(PSink, &p, SETF_SHOT_POINTS, 0U, pts, 15U) == 1U);
    CHECK(ProfileStore_Write(0U, "ragged", "v", p.buf, p.pos) == PROFILE_OK);
    CHECK(Settings_Recall(0U, &res) == PROFILE_OK && res.apply.failed == 1U && ShotProfile_Count() == 0U);

    /* a curve with two peaks is refused and the live curve is left untouched; a good one replaces it */
    ResetAll();
    BuildCurve(21000.0f);
    Settings_LiveTables(&t0);
    {
        uint8_t cp[4U * SET_CURVE_POINT_BYTES];
        float hz[4] = { 10000.0f, 15000.0f, 20000.0f, 25000.0f };
        float v[4]  = { 0.1f, 1.0f, 0.2f, 0.9f };                  /* up, down, up: two peaks */

        for (int i = 0; i < 4; i++)
        {
            uint32_t u;
            memcpy(&u, &hz[i], 4U); SettingsCodec_Put32(&cp[i * 8], u);
            memcpy(&u, &v[i], 4U);  SettingsCodec_Put32(&cp[i * 8 + 4], u);
        }
        PScalars(&p, (uint8_t)SUPPLY_MODE_HVPS);
        PCount(&p, SETF_CURVE_COUNT, 4U);
        CHECK(SettingsCodec_EmitRecord(PSink, &p, SETF_CURVE_POINTS, 0U, cp, sizeof(cp)) == 1U);
        CHECK(ProfileStore_Write(0U, "peaks", "v", p.buf, p.pos) == PROFILE_OK);
        CHECK(Settings_Recall(0U, &res) == PROFILE_OK);
        CHECK(res.apply.failed == 1U && strstr(res.apply.failedNames, "CURVE") != NULL);
        Settings_LiveTables(&t1);
        CHECK(TablesEqual(&t0, &t1));
    }

    /* empty tables in a save (count 0) clear the live ones; a curve count of 0 returns to the linear default */
    ResetAll();
    (void)SupplyMode_Set(SUPPLY_MODE_HVPS);
    CHECK(BuildShot(10U, 1000U) == 1 && BuildTarget(4U) == 1);
    BuildCurve(21000.0f);
    PScalars(&p, (uint8_t)SUPPLY_MODE_HVPS);
    PCount(&p, SETF_SHOT_COUNT, 0U);
    PCount(&p, SETF_TARGET_COUNT, 0U);
    PCount(&p, SETF_CURVE_COUNT, 0U);
    CHECK(ProfileStore_Write(0U, "empty", "v", p.buf, p.pos) == PROFILE_OK);
    CHECK(Settings_Recall(0U, &res) == PROFILE_OK && res.apply.failed == 0U);
    CHECK(ShotProfile_Count() == 0U && Target_Count() == 0U && Control_Loop()->cfg.curveCustom == 0U);

    /* an older save with scalars only leaves the live tables alone; unknown ids are counted as ignored */
    ResetAll();
    (void)SupplyMode_Set(SUPPLY_MODE_HVPS);
    CHECK(BuildShot(10U, 1000U) == 1 && BuildTarget(4U) == 1);
    PScalars(&p, (uint8_t)SUPPLY_MODE_HVPS);
    {
        uint8_t v[2] = { 1U, 2U };
        CHECK(SettingsCodec_EmitRecord(PSink, &p, 99U, SET_CHANNEL_SYSTEM, v, 2U) == 1U);
    }
    CHECK(ProfileStore_Write(0U, "old", "v", p.buf, p.pos) == PROFILE_OK);
    CHECK(Settings_Recall(0U, &res) == PROFILE_OK && res.apply.failed == 0U && res.ignored == 1U);
    CHECK(ShotProfile_Count() == 10U && Target_Count() == 4U);

    /* a bad scalar is named and counted, the others still apply (resonance outside the HVPS band) */
    ResetAll();
    {
        settings_snapshot_t s;

        SettingsCodec_Clear(&s);
        s.clResonanceHz = 99999.0f;
        s.pidRateHz = 5000U;
        for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
        {
            if (kSettingsFields[fi].id == SETF_CL_RESONANCE_HZ || kSettingsFields[fi].id == SETF_PID_RATE_HZ)
            {
                SettingsCodec_SetPresent(&s, fi, 0U);
            }
        }
        p.pos = 0U;
        CHECK(SettingsCodec_EncodeScalars(&s, PSink, &p) == 1U);
        CHECK(ProfileStore_Write(0U, "badres", "v", p.buf, p.pos) == PROFILE_OK);
        CHECK(Settings_Recall(0U, &res) == PROFILE_OK);
        CHECK(res.apply.applied == 1U && res.apply.failed == 1U);
        CHECK(strstr(res.apply.failedNames, "CLOSEDloop:RESonance") != NULL && Control_GetRateHz() == 5000U);
    }

    /* a mode other than LIMITER/HVPS is refused */
    ResetAll();
    PScalars(&p, 7U);
    CHECK(ProfileStore_Write(0U, "badmode", "v", p.buf, p.pos) == PROFILE_OK);
    CHECK(Settings_Recall(0U, &res) == PROFILE_OK && res.apply.failed == 1U && SupplyMode_Get() == SUPPLY_MODE_NONE);
}

/* PIDRate, ADC:AVERage and ADC:SAMPle depend on each other: values that are legal together but refused in the
   order they are applied (a faster rate while the scan is still long) are retried in the other order */
static void TestTimingOrder(void)
{
    payload_t p;
    settings_snapshot_t s;
    settings_recall_result_t res;

    ResetAll();
    (void)Control_SetSampleTimeX10(6405U);       /* a slow, long scan at the default 10 kHz is refused: use a rate that fits */
    (void)Control_SetRateHz(1000U);
    (void)Control_SetSampleTimeX10(6405U);
    CHECK(Control_GetRateHz() == 1000U && Control_GetSampleTimeX10() == 6405U);

    SettingsCodec_Clear(&s);
    s.pidRateHz = 10000U; s.adcSampleX10 = 475U; s.adcScans = 4U;        /* the defaults: only legal in the right order */
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        uint8_t id = kSettingsFields[fi].id;

        if (id == SETF_PID_RATE_HZ || id == SETF_ADC_SAMPLE_X10 || id == SETF_ADC_SCANS)
        {
            SettingsCodec_SetPresent(&s, fi, 0U);
        }
    }
    p.pos = 0U;
    CHECK(SettingsCodec_EncodeScalars(&s, PSink, &p) == 1U);
    CHECK(ProfileStore_Write(1U, "timing", "v", p.buf, p.pos) == PROFILE_OK);
    CHECK(Settings_Recall(1U, &res) == PROFILE_OK);
    CHECK(res.apply.failed == 0U && res.apply.applied == 3U);
    CHECK(Control_GetRateHz() == 10000U && Control_GetSampleTimeX10() == 475U && Control_GetScansPerTick() == 4U);
}

/* A power cut at any point of a save leaves the slot holding exactly the old profile or exactly the new one. */
static void TestSaveCut(void)
{
    settings_tables_t told, tnew, t;
    settings_recall_result_t res;
    uint32_t total;

    ResetAll();
    SetLiveState();
    CHECK(BuildShot(40U, 1900U) == 1 && BuildTarget(3U) == 1);
    BuildCurve(21000.0f);
    CHECK(Settings_Save(1U, "old", "v") == PROFILE_OK);
    Settings_LiveTables(&told);
    CHECK(BuildTarget(1U) == 0);                                    /* (already populated: appending t=0 again is refused) */
    Target_Clear();
    CHECK(BuildTarget(5U) == 1);
    BuildCurve(17000.0f);
    Settings_LiveTables(&tnew);
    CHECK(!TablesEqual(&told, &tnew));

    FakeNv_CutPowerAfter(0xFFFFFFFFUL);
    CHECK(Settings_Save(1U, "new", "v") == PROFILE_OK);
    total = FakeNv_UnitsUsed();

    for (uint32_t cut = 0U; cut <= total; cut += (cut < 6U || cut + 6U > total) ? 1U : 29U)
    {
        int isOld, isNew;

        ResetAll();
        SetLiveState();
        CHECK(BuildShot(40U, 1900U) == 1 && BuildTarget(3U) == 1);
        BuildCurve(21000.0f);
        CHECK(Settings_Save(1U, "old", "v") == PROFILE_OK);
        Target_Clear();
        CHECK(BuildTarget(5U) == 1);
        BuildCurve(17000.0f);
        FakeNv_CutPowerAfter(cut);
        (void)Settings_Save(1U, "new", "v");
        FakeNv_RestorePower();

        ScrambleLiveState();
        CHECK(Settings_Recall(1U, &res) == PROFILE_OK && res.apply.failed == 0U);
        Settings_LiveTables(&t);
        isOld = TablesEqual(&t, &told);
        isNew = TablesEqual(&t, &tnew);
        CHECK(isOld != isNew);
    }
}

int main(void)
{
    TestRoundTrip();
    TestFailureRules();
    TestTimingOrder();
    TestSaveCut();
    return test_report("test_settings");
}
