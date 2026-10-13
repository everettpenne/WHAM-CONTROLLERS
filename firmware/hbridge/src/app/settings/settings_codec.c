/*
 * settings_codec.c -- see settings_codec.h.
 */
#include "settings_codec.h"
#include "scpi_util.h"
#include <stdio.h>
#include <string.h>

#define SYS_OFF(member)  ((uint16_t)offsetof(settings_snapshot_t, member))
#define PL_OFF(member)   ((uint16_t)offsetof(settings_plant_t, member))
#define XL_OFF(member)   ((uint16_t)offsetof(settings_xloop_t, member))

/* Order here is only the display/encode order; ids are what identify a field. */
const settings_field_t kSettingsFields[] =
{
    { SETF_SUPPLY_MODE,     "MODE",             SETK_U8,  0U, SYS_OFF(supplyMode)    },
    { SETF_PID_RATE_HZ,     "PIDRate",          SETK_U32, SETS_SYSTEM, SYS_OFF(pidRateHz)     },
    { SETF_ADC_SCANS,       "ADC:AVERage",      SETK_U32, SETS_SYSTEM, SYS_OFF(adcScans)      },
    { SETF_ADC_SAMPLE_X10,  "ADC:SAMPle_x10",   SETK_U32, SETS_SYSTEM, SYS_OFF(adcSampleX10)  },
    { SETF_SLEW_PER_TICK,   "SLEWRate",         SETK_F32, SETS_SYSTEM, SYS_OFF(slewPerTick)   },
    { SETF_MAX_VOLTS,       "MAXVolts",         SETK_F32, SETS_SYSTEM, SYS_OFF(maxVolts)      },
    { SETF_FB_CHANNEL,      "FEEDback:CHANnel", SETK_U8,  0U, SYS_OFF(fbChannel)     },
    { SETF_FB_SCALE,        "FEEDback:SCALE",   SETK_F32, SETS_SYSTEM, SYS_OFF(fbScale)       },
    { SETF_FB_OFFSET_V,     "FEEDback:OFFSet",  SETK_F32, SETS_SYSTEM, SYS_OFF(fbOffsetV)     },
    { SETF_FB_FILTER_HZ,    "FEEDback:FILTer",  SETK_F32, SETS_SYSTEM, SYS_OFF(fbFilterHz)    },
    { SETF_FB_CLIP_MS,      "FEEDback:CLIPTime", SETK_F32, SETS_SYSTEM, SYS_OFF(fbClipMs)     },
    { SETF_LOOP_MODE,       "LOOPMODE",         SETK_U8,  0U, SYS_OFF(loopMode)      },
    { SETF_CL_HVPS_DUTY,    "CLOSEDloop:DUTY",  SETK_F32, SETS_SYSTEM, SYS_OFF(clHvpsDuty)    },
    { SETF_CL_LIMITER_FREQ, "CLOSEDloop:FREQuency", SETK_F32, SETS_SYSTEM, SYS_OFF(clLimiterFreq) },
    { SETF_CL_RESONANCE_HZ, "CLOSEDloop:RESonance", SETK_F32, SETS_SYSTEM, SYS_OFF(clResonanceHz) },
    { SETF_CL_VPEAK,        "CLOSEDloop:VPEAK", SETK_F32, SETS_SYSTEM, SYS_OFF(clVpeak)       },
    { SETF_CL_VSCALE,       "CLOSEDloop:VSCALE", SETK_F32, SETS_SYSTEM, SYS_OFF(clVscale)     },
    { SETF_CL_BRANCH,       "CLOSEDloop:BRANCH", SETK_U8, SETS_SYSTEM, SYS_OFF(clBranch)      },

    { SETF_KP,              "kp",               SETK_F32, SETS_PLANT, PL_OFF(kp)             },
    { SETF_KI,              "ki",               SETK_F32, SETS_PLANT, PL_OFF(ki)             },
    { SETF_KD,              "kd",               SETK_F32, SETS_PLANT, PL_OFF(kd)             },

    { SETF_X_LOOP_MODE,     "LOOPMODE",         SETK_U8,  SETS_XLOOP, XL_OFF(loopMode)       },
    { SETF_X_FB_CHANNEL,    "FEEDback:CHANnel", SETK_U8,  SETS_XLOOP, XL_OFF(fbChannel)      },
    { SETF_X_FB_SCALE,      "FEEDback:SCALE",   SETK_F32, SETS_XLOOP, XL_OFF(fbScale)        },
    { SETF_X_FB_OFFSET_V,   "FEEDback:OFFSet",  SETK_F32, SETS_XLOOP, XL_OFF(fbOffsetV)      },
    { SETF_X_KP,            "kp",               SETK_F32, SETS_XLOOP_PLANT, PL_OFF(kp)       },
    { SETF_X_KI,            "ki",               SETK_F32, SETS_XLOOP_PLANT, PL_OFF(ki)       },
    { SETF_X_KD,            "kd",               SETK_F32, SETS_XLOOP_PLANT, PL_OFF(kd)       }
};
const uint8_t kSettingsFieldCount = (uint8_t)(sizeof(kSettingsFields) / sizeof(kSettingsFields[0]));

static const char *const kPlantName[SET_PLANT_COUNT] = { "hvps", "lim" };

/* The presence bitmap must be big enough for every (field, instance). */
typedef char settings_present_bits_check[(sizeof(kSettingsFields) / sizeof(kSettingsFields[0])
                                          * SET_MAX_INSTANCES <= SET_PRESENT_BITS) ? 1 : -1];

uint8_t SettingsCodec_Instances(uint8_t fieldIdx)
{
    switch (kSettingsFields[fieldIdx].scope)
    {
    case SETS_PLANT:       return (uint8_t)SET_PLANT_COUNT;
    case SETS_XLOOP:       return (uint8_t)SET_XLOOPS;
    case SETS_XLOOP_PLANT: return (uint8_t)SET_MAX_INSTANCES;
    default:               return 1U;
    }
}

static uint8_t *ValuePtr(settings_snapshot_t *s, const settings_field_t *f, uint8_t ch)
{
    uint8_t *base = (uint8_t *)s;

    switch (f->scope)
    {
    case SETS_PLANT:       base = (uint8_t *)&s->plant[ch]; break;
    case SETS_XLOOP:       base = (uint8_t *)&s->xloop[ch]; break;
    case SETS_XLOOP_PLANT: base = (uint8_t *)&s->xplant[ch]; break;
    default:               break;
    }
    return base + f->offset;
}

static const uint8_t *ValuePtrC(const settings_snapshot_t *s, const settings_field_t *f, uint8_t ch)
{
    return ValuePtr((settings_snapshot_t *)(uintptr_t)s, f, ch);
}

static uint16_t PresentBit(uint8_t fieldIdx, uint8_t ch)
{
    return (uint16_t)((uint16_t)fieldIdx * SET_MAX_INSTANCES + ch);
}

static uint8_t ValueLen(const settings_field_t *f)
{
    return (f->kind == SETK_U8) ? 1U : 4U;
}

void SettingsCodec_Clear(settings_snapshot_t *s)
{
    memset(s, 0, sizeof(*s));
}

uint8_t SettingsCodec_IsPresent(const settings_snapshot_t *s, uint8_t fieldIdx, uint8_t ch)
{
    uint16_t bit = PresentBit(fieldIdx, ch);

    return (uint8_t)((s->present[bit / 8U] >> (bit % 8U)) & 1U);
}

void SettingsCodec_SetPresent(settings_snapshot_t *s, uint8_t fieldIdx, uint8_t ch)
{
    uint16_t bit = PresentBit(fieldIdx, ch);

    s->present[bit / 8U] = (uint8_t)(s->present[bit / 8U] | (1U << (bit % 8U)));
}

void SettingsCodec_Put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void SettingsCodec_Put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint16_t SettingsCodec_Get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t SettingsCodec_Get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint8_t SettingsCodec_EmitRecord(settings_sink_fn sink, void *ctx, uint8_t id, uint8_t ch,
                                 const void *value, uint8_t len)
{
    uint8_t rec[3U + 255U];

    rec[0] = id;
    rec[1] = ch;
    rec[2] = len;
    if (len != 0U)
    {
        memcpy(&rec[3], value, len);
    }
    return sink(ctx, rec, (uint16_t)(3U + len));
}

uint8_t SettingsCodec_EncodeScalars(const settings_snapshot_t *s, settings_sink_fn sink, void *ctx)
{
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        const settings_field_t *f = &kSettingsFields[fi];
        uint8_t count = SettingsCodec_Instances(fi);

        for (uint8_t c = 0U; c < count; c++)
        {
            if (SettingsCodec_IsPresent(s, fi, c) == 0U)
            {
                continue;
            }
            if (SettingsCodec_EmitRecord(sink, ctx, f->id, (f->scope != SETS_SYSTEM) ? c : SET_CHANNEL_SYSTEM,
                                         ValuePtrC(s, f, c), ValueLen(f)) == 0U)
            {
                return 0U;
            }
        }
    }
    return 1U;
}

typedef struct
{
    uint8_t *buf;
    uint16_t cap;
    uint16_t pos;
} buf_sink_t;

static uint8_t BufSink(void *ctx, const uint8_t *data, uint16_t n)
{
    buf_sink_t *b = (buf_sink_t *)ctx;

    if ((uint32_t)b->pos + n > b->cap)
    {
        return 0U;
    }
    memcpy(&b->buf[b->pos], data, n);
    b->pos = (uint16_t)(b->pos + n);
    return 1U;
}

uint16_t SettingsCodec_Encode(const settings_snapshot_t *s, uint8_t *buf, uint16_t cap)
{
    buf_sink_t b = { buf, cap, 0U };

    return (SettingsCodec_EncodeScalars(s, BufSink, &b) != 0U) ? b.pos : 0U;
}

settings_rec_status_t SettingsCodec_NextRecord(settings_read_fn rd, void *ctx, uint16_t total,
                                               uint16_t *offset, settings_record_t *rec)
{
    uint8_t head[3];

    if (*offset == total)
    {
        return SETREC_END;
    }
    if (((uint32_t)*offset + 3U) > total || rd(ctx, *offset, head, 3U) == 0U)
    {
        return SETREC_BAD;
    }
    rec->id = head[0];
    rec->ch = head[1];
    rec->len = head[2];
    if (((uint32_t)*offset + 3U + rec->len) > total ||
        (rec->len != 0U && rd(ctx, (uint16_t)(*offset + 3U), rec->value, rec->len) == 0U))
    {
        return SETREC_BAD;
    }
    *offset = (uint16_t)(*offset + 3U + rec->len);
    return SETREC_OK;
}

uint8_t SettingsCodec_IsTableId(uint8_t id)
{
    return (uint8_t)((id >= SETF_SHOT_COUNT) && (id <= SETF_TARGET3_POINTS));
}

uint8_t SettingsCodec_StoreRecord(settings_snapshot_t *s, const settings_record_t *rec, settings_decode_info_t *info)
{
    const settings_field_t *f = NULL;
    uint8_t fi;
    uint8_t c;

    if (SettingsCodec_IsTableId(rec->id) != 0U)
    {
        info->tables++;
        return 1U;
    }
    for (fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        if (kSettingsFields[fi].id == rec->id)
        {
            f = &kSettingsFields[fi];
            break;
        }
    }
    if (f == NULL)
    {
        info->unknown++;
        return 1U;
    }
    /* Known field: its shape must match exactly. */
    if ((f->scope != SETS_SYSTEM) == (rec->ch == SET_CHANNEL_SYSTEM) || rec->len != ValueLen(f))
    {
        return 0U;
    }
    if (f->scope != SETS_SYSTEM && rec->ch >= SettingsCodec_Instances(fi))
    {
        info->outOfRange++;
        return 1U;
    }
    c = (f->scope != SETS_SYSTEM) ? rec->ch : 0U;
    memcpy(ValuePtr(s, f, c), rec->value, rec->len);
    SettingsCodec_SetPresent(s, fi, c);
    info->records++;
    return 1U;
}

typedef struct
{
    const uint8_t *buf;
} mem_src_t;

static uint8_t MemRead(void *ctx, uint16_t offset, void *out, uint16_t n)
{
    memcpy(out, &((const mem_src_t *)ctx)->buf[offset], n);
    return 1U;
}

uint8_t SettingsCodec_Decode(const uint8_t *buf, uint16_t len, settings_snapshot_t *s,
                             settings_decode_info_t *info)
{
    mem_src_t src = { buf };
    settings_decode_info_t local = {0U, 0U, 0U, 0U};
    settings_record_t rec;
    uint16_t offset = 0U;
    settings_rec_status_t st;

    SettingsCodec_Clear(s);
    while ((st = SettingsCodec_NextRecord(MemRead, &src, len, &offset, &rec)) == SETREC_OK)
    {
        if (SettingsCodec_StoreRecord(s, &rec, &local) == 0U)
        {
            return 0U;
        }
    }
    if (st != SETREC_END)
    {
        return 0U;
    }
    if (info != NULL)
    {
        *info = local;
    }
    return 1U;
}

void SettingsCodec_FormatValue(const settings_snapshot_t *s, uint8_t fieldIdx, uint8_t ch,
                               char *out, size_t n)
{
    const settings_field_t *f = &kSettingsFields[fieldIdx];
    const uint8_t *p = ValuePtrC(s, f, (f->scope != SETS_SYSTEM) ? ch : 0U);

    switch (f->kind)
    {
    case SETK_U8:
        snprintf(out, n, "%u", (unsigned int)*p);
        break;
    case SETK_U32:
    {
        uint32_t v;
        memcpy(&v, p, sizeof(v));
        snprintf(out, n, "%lu", (unsigned long)v);
        break;
    }
    default:
    {
        float v;
        memcpy(&v, p, sizeof(v));
        Scpi_FormatFloat(v, out, n);
        break;
    }
    }
}

void SettingsCodec_FormatName(uint8_t fieldIdx, uint8_t ch, char *out, size_t n)
{
    const settings_field_t *f = &kSettingsFields[fieldIdx];

    if (f->scope == SETS_PLANT)
    {
        snprintf(out, n, "%s.%s", kPlantName[(ch < SET_PLANT_COUNT) ? ch : 0U], f->name);
    }
    else if (f->scope == SETS_XLOOP)
    {
        snprintf(out, n, "loop%u.%s", (unsigned)(ch + 2U), f->name);
    }
    else if (f->scope == SETS_XLOOP_PLANT)
    {
        snprintf(out, n, "loop%u.%s.%s", (unsigned)(ch / SET_PLANT_COUNT + 2U), kPlantName[ch % SET_PLANT_COUNT], f->name);
    }
    else
    {
        snprintf(out, n, "%s", f->name);
    }
}

uint8_t SettingsCodec_ValueEqual(const settings_snapshot_t *a, const settings_snapshot_t *b,
                                 uint8_t fieldIdx, uint8_t ch)
{
    const settings_field_t *f = &kSettingsFields[fieldIdx];
    uint8_t c = (f->scope != SETS_SYSTEM) ? ch : 0U;

    return (uint8_t)(memcmp(ValuePtrC(a, f, c), ValuePtrC(b, f, c), ValueLen(f)) == 0);
}

uint32_t SettingsCodec_Crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    uint32_t c = ~crc;

    while (len-- != 0U)
    {
        c ^= *data++;
        for (uint32_t k = 0U; k < 8U; k++)
        {
            c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1UL)));
        }
    }
    return ~c;
}
