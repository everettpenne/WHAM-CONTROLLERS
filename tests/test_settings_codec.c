/*
 * test_settings_codec.c -- snapshot encode/decode/compare
 * (src/app/settings/settings_codec.c), on the host.
 */
#include "settings_codec.h"
#include "test_util.h"
#include <string.h>

static uint8_t FieldIndexById(uint8_t id)
{
    for (uint8_t i = 0U; i < kSettingsFieldCount; i++)
    {
        if (kSettingsFields[i].id == id)
        {
            return i;
        }
    }
    return 0xFFU;
}

/* Every field present, distinctive values. */
static void FillFull(settings_snapshot_t *s)
{
    SettingsCodec_Clear(s);
    s->pidRateHz = 2000U; s->turnonHz = 6000U; s->maxFreqHz = 90000U;
    s->slewHzPerTick = 12.5f; s->faultRampS = 0.75f;
    s->polWater = 1U; s->polTemp = 0U; s->polEnerpro = 1U; s->polOcp = 0U;
    s->gpoutEnable = 1U; s->pwmAltEnable = 0U;
    s->rampUpMs = 1000U; s->flatTopMs = 2500U; s->rampDownMs = 800U;
    for (uint8_t c = 0U; c < HRTIM_NUM_CHANNELS; c++)
    {
        s->ch[c].kp = 0.1f * (float)(c + 1U);
        s->ch[c].ki = 250.0f + (float)c;
        s->ch[c].kd = 0.001f * (float)c;
        s->ch[c].closedLoop = (uint8_t)(c & 1U);
        s->ch[c].maxCurrentA = 6000.0f - (float)c;
        s->ch[c].demandCurrentA = 1234.5f + (float)c;
        strcpy(s->ch[c].nickname, (c == 1U) ? "INKYWINKY123456" : "");
    }
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        uint8_t n = (kSettingsFields[fi].perChannel != 0U) ? (uint8_t)HRTIM_NUM_CHANNELS : 1U;
        for (uint8_t c = 0U; c < n; c++)
        {
            SettingsCodec_SetPresent(s, fi, c);
        }
    }
}

static int SameEverything(const settings_snapshot_t *a, const settings_snapshot_t *b)
{
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        uint8_t n = (kSettingsFields[fi].perChannel != 0U) ? (uint8_t)HRTIM_NUM_CHANNELS : 1U;
        for (uint8_t c = 0U; c < n; c++)
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

static void TestRoundTrip(void)
{
    settings_snapshot_t a, b;
    settings_decode_info_t info;
    uint8_t buf[1024];
    uint16_t n;

    FillFull(&a);
    n = SettingsCodec_Encode(&a, buf, sizeof(buf));
    CHECK(n > 0U);
    CHECK(n < 600U);
    CHECK(SettingsCodec_Decode(buf, n, &b, &info) == 1U);
    CHECK(SameEverything(&a, &b));
    CHECK(info.unknown == 0U && info.outOfRange == 0U);
    CHECK(info.records == (uint16_t)(14U + 7U * HRTIM_NUM_CHANNELS));
    CHECK_STR(b.ch[1].nickname, "INKYWINKY123456");
    CHECK(b.ch[0].nickname[0] == '\0');
}

static void TestAbsentFieldsStayAbsent(void)
{
    settings_snapshot_t a, b;
    uint8_t buf[1024];
    uint16_t n;

    FillFull(&a);
    /* timing never set: drop its three fields */
    memset(a.present, 0, sizeof(a.present));
    SettingsCodec_SetPresent(&a, FieldIndexById(SETF_PID_RATE_HZ), 0U);
    SettingsCodec_SetPresent(&a, FieldIndexById(SETF_KP), 2U);
    n = SettingsCodec_Encode(&a, buf, sizeof(buf));
    CHECK(n == (uint16_t)((3U + 4U) + (3U + 4U)));
    CHECK(SettingsCodec_Decode(buf, n, &b, NULL) == 1U);
    CHECK(SettingsCodec_IsPresent(&b, FieldIndexById(SETF_PID_RATE_HZ), 0U) == 1U);
    CHECK(SettingsCodec_IsPresent(&b, FieldIndexById(SETF_KP), 2U) == 1U);
    CHECK(SettingsCodec_IsPresent(&b, FieldIndexById(SETF_KP), 1U) == 0U);
    CHECK(SettingsCodec_IsPresent(&b, FieldIndexById(SETF_RAMP_UP_MS), 0U) == 0U);
    CHECK(SameEverything(&a, &b));
}

static void TestUnknownAndMalformed(void)
{
    settings_snapshot_t s;
    settings_decode_info_t info;
    /* unknown id 200 (len 2), then PIDRate = 1500 */
    static const uint8_t withUnknown[] = { 200, 0xFF, 2, 0xAA, 0xBB, SETF_PID_RATE_HZ, 0xFF, 4, 0xDC, 0x05, 0, 0 };
    static const uint8_t truncated[]   = { SETF_PID_RATE_HZ, 0xFF, 4, 0xDC, 0x05 };
    static const uint8_t wrongLen[]    = { SETF_PID_RATE_HZ, 0xFF, 2, 0xDC, 0x05 };
    static const uint8_t wrongScope[]  = { SETF_KP, 0xFF, 4, 0, 0, 0, 0 };
    static const uint8_t sysAsChan[]   = { SETF_PID_RATE_HZ, 0x00, 4, 0, 0, 0, 0 };
    static const uint8_t badChan[]     = { SETF_KP, 9, 4, 0, 0, 0, 0 };
    static const uint8_t longName[]    = { SETF_NICKNAME, 0, 16, 'a','a','a','a','a','a','a','a','a','a','a','a','a','a','a','a' };

    CHECK(SettingsCodec_Decode(withUnknown, sizeof(withUnknown), &s, &info) == 1U);
    CHECK(info.unknown == 1U && info.records == 1U);
    CHECK(s.pidRateHz == 1500U);
    CHECK(SettingsCodec_Decode(truncated, sizeof(truncated), &s, NULL) == 0U);
    CHECK(SettingsCodec_Decode(wrongLen, sizeof(wrongLen), &s, NULL) == 0U);
    CHECK(SettingsCodec_Decode(wrongScope, sizeof(wrongScope), &s, NULL) == 0U);
    CHECK(SettingsCodec_Decode(sysAsChan, sizeof(sysAsChan), &s, NULL) == 0U);
    CHECK(SettingsCodec_Decode(longName, sizeof(longName), &s, NULL) == 0U);
    CHECK(SettingsCodec_Decode(badChan, sizeof(badChan), &s, &info) == 1U);
    CHECK(info.outOfRange == 1U && info.records == 0U);
    CHECK(SettingsCodec_Decode(NULL, 0U, &s, &info) == 1U);   /* empty payload: valid, nothing in it */
    CHECK(info.records == 0U);
}

static void TestEncodeTooSmall(void)
{
    settings_snapshot_t a;
    uint8_t buf[16];

    FillFull(&a);
    CHECK(SettingsCodec_Encode(&a, buf, sizeof(buf)) == 0U);
}

static void TestFormatAndCompare(void)
{
    settings_snapshot_t a, b;
    char out[32];

    FillFull(&a);
    b = a;
    SettingsCodec_FormatName(FieldIndexById(SETF_PID_RATE_HZ), 0U, out, sizeof(out));
    CHECK_STR(out, "PIDRate");
    SettingsCodec_FormatName(FieldIndexById(SETF_KI), 2U, out, sizeof(out));
    CHECK_STR(out, "ch3.ki");
    SettingsCodec_FormatValue(&a, FieldIndexById(SETF_PID_RATE_HZ), 0U, out, sizeof(out));
    CHECK_STR(out, "2000");
    SettingsCodec_FormatValue(&a, FieldIndexById(SETF_SLEW_HZ_PER_TICK), 0U, out, sizeof(out));
    CHECK_STR(out, "12.5");
    SettingsCodec_FormatValue(&a, FieldIndexById(SETF_POL_WATER), 0U, out, sizeof(out));
    CHECK_STR(out, "1");
    SettingsCodec_FormatValue(&a, FieldIndexById(SETF_NICKNAME), 0U, out, sizeof(out));
    CHECK_STR(out, "-");
    SettingsCodec_FormatValue(&a, FieldIndexById(SETF_NICKNAME), 1U, out, sizeof(out));
    CHECK_STR(out, "INKYWINKY123456");

    CHECK(SettingsCodec_ValueEqual(&a, &b, FieldIndexById(SETF_KP), 1U) == 1U);
    b.ch[1].kp += 1.0f;
    CHECK(SettingsCodec_ValueEqual(&a, &b, FieldIndexById(SETF_KP), 1U) == 0U);
    CHECK(SettingsCodec_ValueEqual(&a, &b, FieldIndexById(SETF_KP), 0U) == 1U);
    strcpy(b.ch[0].nickname, "X");
    CHECK(SettingsCodec_ValueEqual(&a, &b, FieldIndexById(SETF_NICKNAME), 0U) == 0U);
}

static void TestCrc(void)
{
    static const uint8_t v[] = "123456789";
    uint32_t whole = SettingsCodec_Crc32(0U, v, 9U);
    uint32_t split = SettingsCodec_Crc32(SettingsCodec_Crc32(0U, v, 4U), v + 4, 5U);

    CHECK(whole == 0xCBF43926UL);   /* the standard CRC-32 check value */
    CHECK(split == whole);
}

static void TestFieldIdsUnique(void)
{
    for (uint8_t i = 0U; i < kSettingsFieldCount; i++)
    {
        for (uint8_t j = (uint8_t)(i + 1U); j < kSettingsFieldCount; j++)
        {
            CHECK(kSettingsFields[i].id != kSettingsFields[j].id);
            CHECK(strcmp(kSettingsFields[i].name, kSettingsFields[j].name) != 0);
        }
    }
}

int main(void)
{
    TestRoundTrip();
    TestAbsentFieldsStayAbsent();
    TestUnknownAndMalformed();
    TestEncodeTooSmall();
    TestFormatAndCompare();
    TestCrc();
    TestFieldIdsUnique();
    return test_report("test_settings_codec");
}
