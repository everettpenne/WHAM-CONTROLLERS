/*
 * test_settings_codec.c -- the saved-settings data model (src/app/settings/settings_codec.c): the field table,
 * the tagged-record encoding, decoding (buffered and as a stream), and the frozen field-id contract.
 */
#include "settings_codec.h"
#include "test_util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* table points must fit one record (<= 255 bytes) */
typedef char shot_chunk_fits[(SET_SHOT_CHUNK_POINTS * SET_SHOT_POINT_BYTES <= 255U) ? 1 : -1];
typedef char target_chunk_fits[(SET_TARGET_CHUNK_POINTS * SET_TARGET_POINT_BYTES <= 255U) ? 1 : -1];
typedef char curve_chunk_fits[(SET_CURVE_CHUNK_POINTS * SET_CURVE_POINT_BYTES <= 255U) ? 1 : -1];

static void FillAll(settings_snapshot_t *s)
{
    SettingsCodec_Clear(s);
    s->supplyMode = 2U;
    s->pidRateHz = 8000U;
    s->adcScans = 3U;
    s->adcSampleX10 = 925U;
    s->slewPerTick = 12.5f;
    s->maxVolts = 950.0f;
    s->fbChannel = 4U;
    s->fbScale = 620.69f;
    s->fbOffsetV = 1.45f;
    s->fbFilterHz = 250.0f;
    s->fbClipMs = 2.0f;
    s->loopMode = 1U;
    s->clHvpsDuty = 47.5f;
    s->clLimiterFreq = 90000.0f;
    s->clResonanceHz = 19250.0f;
    s->clVpeak = 1.5f;
    s->clVscale = 900.0f;
    s->clBranch = 0U;
    s->plant[SET_PLANT_HVPS].kp = 4000.0f;
    s->plant[SET_PLANT_HVPS].ki = 200000.0f;
    s->plant[SET_PLANT_HVPS].kd = 0.001f;
    s->plant[SET_PLANT_LIMITER].kp = 0.167f;
    s->plant[SET_PLANT_LIMITER].ki = 6.667f;
    s->plant[SET_PLANT_LIMITER].kd = 0.0f;
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        for (uint8_t c = 0U; c < ((kSettingsFields[fi].perPlant != 0U) ? SET_PLANT_COUNT : 1U); c++)
        {
            SettingsCodec_SetPresent(s, fi, c);
        }
    }
}

static int AllEqual(const settings_snapshot_t *a, const settings_snapshot_t *b)
{
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        for (uint8_t c = 0U; c < ((kSettingsFields[fi].perPlant != 0U) ? SET_PLANT_COUNT : 1U); c++)
        {
            if (SettingsCodec_IsPresent(a, fi, c) != SettingsCodec_IsPresent(b, fi, c))
            {
                return 0;
            }
            if (SettingsCodec_IsPresent(a, fi, c) != 0U && SettingsCodec_ValueEqual(a, b, fi, c) == 0U)
            {
                return 0;
            }
        }
    }
    return 1;
}

static void TestFieldTable(void)
{
    /* ids are a frozen contract: these numbers may never change (see the header) */
    CHECK(SETF_SUPPLY_MODE == 1 && SETF_PID_RATE_HZ == 2 && SETF_ADC_SCANS == 3 && SETF_ADC_SAMPLE_X10 == 4);
    CHECK(SETF_SLEW_PER_TICK == 5 && SETF_MAX_VOLTS == 6 && SETF_FB_CHANNEL == 7 && SETF_FB_SCALE == 8);
    CHECK(SETF_FB_OFFSET_V == 9 && SETF_FB_FILTER_HZ == 10 && SETF_FB_CLIP_MS == 11 && SETF_LOOP_MODE == 12);
    CHECK(SETF_CL_HVPS_DUTY == 13 && SETF_CL_LIMITER_FREQ == 14 && SETF_CL_RESONANCE_HZ == 15);
    CHECK(SETF_CL_VPEAK == 16 && SETF_CL_VSCALE == 17 && SETF_CL_BRANCH == 18);
    CHECK(SETF_KP == 32 && SETF_KI == 33 && SETF_KD == 34);
    CHECK(SETF_SHOT_COUNT == 64 && SETF_SHOT_POINTS == 65 && SETF_TARGET_COUNT == 66);
    CHECK(SETF_TARGET_POINTS == 67 && SETF_CURVE_COUNT == 68 && SETF_CURVE_POINTS == 69);
    CHECK(SET_PLANT_HVPS == 0U && SET_PLANT_LIMITER == 1U);

    /* every id is unique, none is a table id, every field has a name */
    for (uint8_t i = 0U; i < kSettingsFieldCount; i++)
    {
        CHECK(kSettingsFields[i].name != NULL && kSettingsFields[i].name[0] != '\0');
        CHECK(SettingsCodec_IsTableId(kSettingsFields[i].id) == 0U);
        for (uint8_t j = (uint8_t)(i + 1U); j < kSettingsFieldCount; j++)
        {
            CHECK(kSettingsFields[i].id != kSettingsFields[j].id);
        }
    }
    CHECK(SettingsCodec_IsTableId(63U) == 0U && SettingsCodec_IsTableId(64U) == 1U);
    CHECK(SettingsCodec_IsTableId(69U) == 1U && SettingsCodec_IsTableId(70U) == 0U);
}

static void TestRoundTrip(void)
{
    settings_snapshot_t a, b;
    settings_decode_info_t info;
    uint8_t buf[512];
    uint16_t n;

    FillAll(&a);
    n = SettingsCodec_Encode(&a, buf, sizeof(buf));
    CHECK(n > 0U);
    CHECK(SettingsCodec_Decode(buf, n, &b, &info) == 1U);
    CHECK(info.unknown == 0U && info.outOfRange == 0U && info.tables == 0U);
    CHECK(info.records == (uint16_t)(18U + 3U * SET_PLANT_COUNT));
    CHECK(AllEqual(&a, &b));
    CHECK(b.pidRateHz == 8000U && b.supplyMode == 2U && b.clBranch == 0U);
    CHECK(b.plant[SET_PLANT_LIMITER].kp == 0.167f && b.plant[SET_PLANT_HVPS].ki == 200000.0f);

    /* the encoder reports a payload that does not fit */
    CHECK(SettingsCodec_Encode(&a, buf, n - 1U) == 0U);
    CHECK(SettingsCodec_Encode(&a, buf, n) == n);

    /* absent values are neither encoded nor present after decoding */
    SettingsCodec_Clear(&a);
    a.pidRateHz = 1234U;
    SettingsCodec_SetPresent(&a, 1U, 0U);   /* PIDRate is field index 1 */
    n = SettingsCodec_Encode(&a, buf, sizeof(buf));
    CHECK(n == 3U + 4U);
    CHECK(SettingsCodec_Decode(buf, n, &b, &info) == 1U && b.pidRateHz == 1234U && info.records == 1U);
    CHECK(SettingsCodec_IsPresent(&b, 0U, 0U) == 0U && SettingsCodec_IsPresent(&b, 1U, 0U) == 1U);
    CHECK(SettingsCodec_Decode(buf, 0U, &b, &info) == 1U && info.records == 0U);       /* empty payload is fine */
}

static void TestDecodeRules(void)
{
    settings_snapshot_t s;
    settings_decode_info_t info;
    uint8_t buf[64];
    uint16_t n = 0U;

    /* unknown ids, table ids and a plant beyond the count are skipped, counted, and the rest still decoded */
    buf[n++] = 99U; buf[n++] = 0xFFU; buf[n++] = 2U; buf[n++] = 1U; buf[n++] = 2U;                 /* unknown id */
    buf[n++] = SETF_SHOT_COUNT; buf[n++] = 0xFFU; buf[n++] = 2U; buf[n++] = 5U; buf[n++] = 0U;       /* a table record */
    buf[n++] = SETF_KP; buf[n++] = 7U; buf[n++] = 4U; memset(&buf[n], 0, 4U); n = (uint16_t)(n + 4U);  /* plant 7 */
    buf[n++] = SETF_PID_RATE_HZ; buf[n++] = 0xFFU; buf[n++] = 4U; buf[n++] = 0x10U; buf[n++] = 0x27U; buf[n++] = 0U; buf[n++] = 0U;
    CHECK(SettingsCodec_Decode(buf, n, &s, &info) == 1U);
    CHECK(info.unknown == 1U && info.tables == 1U && info.outOfRange == 1U && info.records == 1U && s.pidRateHz == 10000U);

    /* malformed: truncated record, wrong length for a known field, wrong channel shape */
    CHECK(SettingsCodec_Decode(buf, (uint16_t)(n - 1U), &s, &info) == 0U);
    {
        uint8_t bad[] = { SETF_PID_RATE_HZ, 0xFFU, 2U, 1U, 2U };                  /* a u32 field with 2 bytes */
        CHECK(SettingsCodec_Decode(bad, sizeof(bad), &s, &info) == 0U);
    }
    {
        uint8_t bad[] = { SETF_PID_RATE_HZ, 0U, 4U, 1U, 2U, 3U, 4U };             /* a system field with a plant */
        CHECK(SettingsCodec_Decode(bad, sizeof(bad), &s, &info) == 0U);
    }
    {
        uint8_t bad[] = { SETF_KP, 0xFFU, 4U, 1U, 2U, 3U, 4U };                   /* a plant field as system */
        CHECK(SettingsCodec_Decode(bad, sizeof(bad), &s, &info) == 0U);
    }
    {
        uint8_t bad[] = { SETF_PID_RATE_HZ, 0xFFU };                               /* a header cut short */
        CHECK(SettingsCodec_Decode(bad, sizeof(bad), &s, &info) == 0U);
    }
}

/* the stream reader sees the same records as the buffered decoder, whatever the source's read pattern */
typedef struct { const uint8_t *buf; uint16_t reads; uint16_t failAt; } src_t;

static uint8_t SrcRead(void *ctx, uint16_t offset, void *out, uint16_t n)
{
    src_t *s = (src_t *)ctx;

    s->reads++;
    if (s->failAt != 0U && s->reads == s->failAt)
    {
        return 0U;
    }
    memcpy(out, &s->buf[offset], n);
    return 1U;
}

static void TestStreamReader(void)
{
    settings_snapshot_t a, b;
    settings_decode_info_t info = {0U, 0U, 0U, 0U};
    settings_record_t rec;
    uint8_t buf[512];
    uint16_t n, offset = 0U;
    src_t src;
    settings_rec_status_t st;
    uint16_t records = 0U;

    FillAll(&a);
    n = SettingsCodec_Encode(&a, buf, sizeof(buf));
    src.buf = buf; src.reads = 0U; src.failAt = 0U;
    SettingsCodec_Clear(&b);
    while ((st = SettingsCodec_NextRecord(SrcRead, &src, n, &offset, &rec)) == SETREC_OK)
    {
        CHECK(SettingsCodec_StoreRecord(&b, &rec, &info) == 1U);
        records++;
    }
    CHECK(st == SETREC_END && offset == n && records == (uint16_t)(18U + 3U * SET_PLANT_COUNT));
    CHECK(AllEqual(&a, &b));

    /* a failed read, and a total shorter than the records, are SETREC_BAD */
    src.reads = 0U; src.failAt = 3U; offset = 0U;
    while ((st = SettingsCodec_NextRecord(SrcRead, &src, n, &offset, &rec)) == SETREC_OK) { }
    CHECK(st == SETREC_BAD);
    src.failAt = 0U; offset = 0U;
    while ((st = SettingsCodec_NextRecord(SrcRead, &src, (uint16_t)(n - 2U), &offset, &rec)) == SETREC_OK) { }
    CHECK(st == SETREC_BAD);
}

typedef struct { uint16_t bytes; uint16_t calls; uint16_t failAfter; } count_t;

static uint8_t CountSink(void *ctx, const uint8_t *data, uint16_t n)
{
    count_t *c = (count_t *)ctx;

    (void)data;
    c->calls++;
    if (c->failAfter != 0U && c->calls > c->failAfter)
    {
        return 0U;
    }
    c->bytes = (uint16_t)(c->bytes + n);
    return 1U;
}

static void TestSinkAndFormat(void)
{
    settings_snapshot_t a;
    count_t c = { 0U, 0U, 0U };
    uint8_t buf[512];
    uint16_t n;
    char out[40];
    uint8_t value[255];

    FillAll(&a);
    n = SettingsCodec_Encode(&a, buf, sizeof(buf));
    CHECK(SettingsCodec_EncodeScalars(&a, CountSink, &c) == 1U && c.bytes == n);       /* sizing pass == writing pass */
    c.bytes = 0U; c.calls = 0U; c.failAfter = 4U;
    CHECK(SettingsCodec_EncodeScalars(&a, CountSink, &c) == 0U);                        /* a sink can abort */

    memset(value, 0xA5, sizeof(value));
    c.bytes = 0U; c.calls = 0U; c.failAfter = 0U;
    CHECK(SettingsCodec_EmitRecord(CountSink, &c, SETF_SHOT_POINTS, 3U, value, 240U) == 1U && c.bytes == 243U);

    SettingsCodec_FormatName(1U, 0U, out, sizeof(out));
    CHECK_STR(out, "PIDRate");
    SettingsCodec_FormatName((uint8_t)(kSettingsFieldCount - 3U), SET_PLANT_LIMITER, out, sizeof(out));
    CHECK_STR(out, "lim.kp");
    SettingsCodec_FormatName((uint8_t)(kSettingsFieldCount - 1U), SET_PLANT_HVPS, out, sizeof(out));
    CHECK_STR(out, "hvps.kd");
    SettingsCodec_FormatValue(&a, 1U, 0U, out, sizeof(out));
    CHECK_STR(out, "8000");
    SettingsCodec_FormatValue(&a, 4U, 0U, out, sizeof(out));
    CHECK_STR(out, "12.5");
    SettingsCodec_FormatValue(&a, 0U, 0U, out, sizeof(out));
    CHECK_STR(out, "2");
}

static void TestCrcAndHelpers(void)
{
    const uint8_t v[] = "123456789";
    uint8_t p[4];

    CHECK(SettingsCodec_Crc32(0U, v, 9U) == 0xCBF43926UL);                                 /* the standard check value */
    CHECK(SettingsCodec_Crc32(SettingsCodec_Crc32(0U, v, 4U), &v[4], 5U) == 0xCBF43926UL);  /* continued */
    CHECK(SettingsCodec_Crc32(0U, NULL, 0U) == 0U);
    SettingsCodec_Put32(p, 0x12345678UL);
    CHECK(p[0] == 0x78U && p[3] == 0x12U && SettingsCodec_Get32(p) == 0x12345678UL);
    SettingsCodec_Put16(p, 0xBEEFU);
    CHECK(p[0] == 0xEFU && p[1] == 0xBEU && SettingsCodec_Get16(p) == 0xBEEFU);
}

/* the preview text of a float matches printf's %g (which the target's newlib-nano cannot print) */
static void TestFormatFloat(void)
{
    static const float typical[] = { 0.167f, 6.667f, 4000.0f, 200000.0f, 0.001f, 12.5f, 620.69f, 1.45f, 250.0f, 47.5f,
        90000.0f, 19250.0f, 1.5f, 900.0f, 1e-5f, 1e7f, 123456.7f, 0.000123f, 0.0f, -1.5f, -0.001f, 1000000.0f, 999999.0f,
        999999.5f, 0.0001f, 0.00009f, 100.0f, 2.9f, 1e10f, 1.5e-8f, 3.14159265f, 10.0f, 0.5f, 123456789.0f, 5e-5f, 62069.0f };
    char mine[40], ref[40];

    for (size_t i = 0U; i < sizeof(typical) / sizeof(typical[0]); i++)
    {
        SettingsCodec_FormatFloat(typical[i], mine, sizeof(mine));
        snprintf(ref, sizeof(ref), "%g", (double)typical[i]);
        CHECK_STR(mine, ref);
    }
    SettingsCodec_FormatFloat(NAN, mine, sizeof(mine));
    CHECK_STR(mine, "nan");
    SettingsCodec_FormatFloat(INFINITY, mine, sizeof(mine));
    CHECK_STR(mine, "inf");
    SettingsCodec_FormatFloat(-INFINITY, mine, sizeof(mine));
    CHECK_STR(mine, "-inf");
    SettingsCodec_FormatFloat(1.0f, mine, 2U);                         /* a short buffer is cut, never overrun */
    CHECK(mine[0] == '1' && mine[1] == '\0');
    SettingsCodec_FormatFloat(123.456f, mine, 3U);
    CHECK_STR(mine, "12");

    /* arbitrary values: 6 significant digits means up to 5e-6 relative error, no more */
    srand(1);
    for (int i = 0; i < 20000; i++)
    {
        float v = (float)(((double)rand() / RAND_MAX) * pow(10.0, (rand() % 24) - 10));
        double back;

        if ((rand() & 1) != 0) { v = -v; }
        SettingsCodec_FormatFloat(v, mine, sizeof(mine));
        back = strtod(mine, NULL);
        CHECK(fabs(back - (double)v) <= fabs((double)v) * 6.0e-6);
    }
}

int main(void)
{
    TestFormatFloat();
    TestFieldTable();
    TestRoundTrip();
    TestDecodeRules();
    TestStreamReader();
    TestSinkAndFormat();
    TestCrcAndHelpers();
    return test_report("test_settings_codec");
}
