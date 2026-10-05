/*
 * settings_codec.c -- see settings_codec.h.
 */
#include "settings_codec.h"
#include "scpi_util.h"
#include <stdio.h>
#include <string.h>

#define SYS_OFF(member)  ((uint16_t)offsetof(settings_snapshot_t, member))
#define CH_OFF(member)   ((uint16_t)offsetof(settings_channel_t, member))

/* Order here is only the display/encode order; ids are what identify a field. */
const settings_field_t kSettingsFields[] =
{
    { SETF_PID_RATE_HZ,      "PIDRate",          SETK_U32, 0U, SYS_OFF(pidRateHz)     },
    { SETF_TURNON_HZ,        "TURNONHz",         SETK_U32, 0U, SYS_OFF(turnonHz)      },
    { SETF_MAX_FREQ_HZ,      "MAXFREQHz",        SETK_U32, 0U, SYS_OFF(maxFreqHz)     },
    { SETF_SLEW_HZ_PER_TICK, "SLEWRate",         SETK_F32, 0U, SYS_OFF(slewHzPerTick) },
    { SETF_FAULT_RAMP_S,     "FaultRampTime",    SETK_F32, 0U, SYS_OFF(faultRampS)    },
    { SETF_POL_WATER,        "Polarity.WATER",   SETK_U8,  0U, SYS_OFF(polWater)      },
    { SETF_POL_TEMP,         "Polarity.TEMP",    SETK_U8,  0U, SYS_OFF(polTemp)       },
    { SETF_POL_ENERPRO,      "Polarity.ENERPRO", SETK_U8,  0U, SYS_OFF(polEnerpro)    },
    { SETF_POL_OCP,          "Polarity.OCP",     SETK_U8,  0U, SYS_OFF(polOcp)        },
    { SETF_GPOUT_ENABLE,     "GPOut",            SETK_U8,  0U, SYS_OFF(gpoutEnable)   },
    { SETF_PWMALT_ENABLE,    "PWMAlt",           SETK_U8,  0U, SYS_OFF(pwmAltEnable)  },
    { SETF_RAMP_UP_MS,       "RampUpMs",         SETK_U32, 0U, SYS_OFF(rampUpMs)      },
    { SETF_FLAT_TOP_MS,      "FlatTopMs",        SETK_U32, 0U, SYS_OFF(flatTopMs)     },
    { SETF_RAMP_DOWN_MS,     "RampDownMs",       SETK_U32, 0U, SYS_OFF(rampDownMs)    },

    { SETF_KP,               "kp",               SETK_F32, 1U, CH_OFF(kp)             },
    { SETF_KI,               "ki",               SETK_F32, 1U, CH_OFF(ki)             },
    { SETF_KD,               "kd",               SETK_F32, 1U, CH_OFF(kd)             },
    { SETF_CLOSED_LOOP,      "ClosedLoop",       SETK_U8,  1U, CH_OFF(closedLoop)     },
    { SETF_MAX_CURRENT_A,    "MaxCurrent",       SETK_F32, 1U, CH_OFF(maxCurrentA)    },
    { SETF_DEMAND_CURRENT_A, "DemandCurrent",    SETK_F32, 1U, CH_OFF(demandCurrentA) },
    { SETF_NICKNAME,         "Nickname",         SETK_STR, 1U, CH_OFF(nickname)       }
};
const uint8_t kSettingsFieldCount = (uint8_t)(sizeof(kSettingsFields) / sizeof(kSettingsFields[0]));

/* The presence bitmap must be big enough for every (field, channel). */
typedef char settings_present_bits_check[(sizeof(kSettingsFields) / sizeof(kSettingsFields[0])
                                          * HRTIM_NUM_CHANNELS <= SET_PRESENT_BITS) ? 1 : -1];

static uint8_t *ValuePtr(settings_snapshot_t *s, const settings_field_t *f, uint8_t ch)
{
    uint8_t *base = (uint8_t *)s;

    if (f->perChannel != 0U)
    {
        base = (uint8_t *)&s->ch[ch];
    }
    return base + f->offset;
}

static const uint8_t *ValuePtrC(const settings_snapshot_t *s, const settings_field_t *f, uint8_t ch)
{
    return ValuePtr((settings_snapshot_t *)(uintptr_t)s, f, ch);
}

static uint16_t PresentBit(uint8_t fieldIdx, uint8_t ch)
{
    return (uint16_t)((uint16_t)fieldIdx * HRTIM_NUM_CHANNELS + ch);
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

static uint8_t ValueLen(const settings_snapshot_t *s, const settings_field_t *f, uint8_t ch)
{
    switch (f->kind)
    {
    case SETK_U8:  return 1U;
    case SETK_U32: return 4U;
    case SETK_F32: return 4U;
    default:       return (uint8_t)strlen((const char *)ValuePtrC(s, f, ch));
    }
}

uint16_t SettingsCodec_Encode(const settings_snapshot_t *s, uint8_t *buf, uint16_t cap)
{
    uint16_t pos = 0U;

    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        const settings_field_t *f = &kSettingsFields[fi];
        uint8_t chCount = (f->perChannel != 0U) ? (uint8_t)HRTIM_NUM_CHANNELS : 1U;

        for (uint8_t c = 0U; c < chCount; c++)
        {
            uint8_t len;

            if (SettingsCodec_IsPresent(s, fi, c) == 0U)
            {
                continue;
            }
            len = ValueLen(s, f, c);
            if ((uint32_t)pos + 3U + len > cap)
            {
                return 0U;
            }
            buf[pos++] = f->id;
            buf[pos++] = (f->perChannel != 0U) ? c : SET_CHANNEL_SYSTEM;
            buf[pos++] = len;
            memcpy(&buf[pos], ValuePtrC(s, f, c), len);
            pos = (uint16_t)(pos + len);
        }
    }
    return pos;
}

uint8_t SettingsCodec_Decode(const uint8_t *buf, uint16_t len, settings_snapshot_t *s,
                             settings_decode_info_t *info)
{
    uint16_t pos = 0U;
    settings_decode_info_t local = {0U, 0U, 0U};

    SettingsCodec_Clear(s);

    while (pos < len)
    {
        uint8_t id;
        uint8_t ch;
        uint8_t vlen;
        uint8_t fi;
        const settings_field_t *f = NULL;

        if ((uint32_t)pos + 3U > len)
        {
            return 0U;
        }
        id   = buf[pos];
        ch   = buf[pos + 1U];
        vlen = buf[pos + 2U];
        pos  = (uint16_t)(pos + 3U);
        if ((uint32_t)pos + vlen > len)
        {
            return 0U;
        }

        for (fi = 0U; fi < kSettingsFieldCount; fi++)
        {
            if (kSettingsFields[fi].id == id)
            {
                f = &kSettingsFields[fi];
                break;
            }
        }
        if (f == NULL)
        {
            local.unknown++;
            pos = (uint16_t)(pos + vlen);
            continue;
        }

        /* Known field: its shape must match exactly. */
        if ((f->perChannel != 0U) == (ch == SET_CHANNEL_SYSTEM))
        {
            return 0U;
        }
        if (f->kind == SETK_STR ? (vlen > SET_NICKNAME_MAX) : (vlen != ((f->kind == SETK_U8) ? 1U : 4U)))
        {
            return 0U;
        }
        if (f->perChannel != 0U && ch >= HRTIM_NUM_CHANNELS)
        {
            local.outOfRange++;
            pos = (uint16_t)(pos + vlen);
            continue;
        }

        {
            uint8_t chIdx = (f->perChannel != 0U) ? ch : 0U;
            uint8_t *dst = ValuePtr(s, f, chIdx);

            memset(dst, 0, (f->kind == SETK_STR) ? (SET_NICKNAME_MAX + 1U) : vlen);
            memcpy(dst, &buf[pos], vlen);
            SettingsCodec_SetPresent(s, fi, chIdx);
        }
        local.records++;
        pos = (uint16_t)(pos + vlen);
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
    const uint8_t *p = ValuePtrC(s, f, (f->perChannel != 0U) ? ch : 0U);

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
    case SETK_F32:
    {
        float v;
        memcpy(&v, p, sizeof(v));
        Scpi_FormatFloat(v, out, n);
        break;
    }
    default:
        snprintf(out, n, "%s", (p[0] == 0U) ? "-" : (const char *)p);
        break;
    }
}

void SettingsCodec_FormatName(uint8_t fieldIdx, uint8_t ch, char *out, size_t n)
{
    const settings_field_t *f = &kSettingsFields[fieldIdx];

    if (f->perChannel != 0U)
    {
        snprintf(out, n, "ch%u.%s", (unsigned int)(ch + 1U), f->name);
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
    uint8_t c = (f->perChannel != 0U) ? ch : 0U;
    const uint8_t *pa = ValuePtrC(a, f, c);
    const uint8_t *pb = ValuePtrC(b, f, c);

    if (f->kind == SETK_STR)
    {
        return (uint8_t)(strncmp((const char *)pa, (const char *)pb, SET_NICKNAME_MAX + 1U) == 0);
    }
    return (uint8_t)(memcmp(pa, pb, (f->kind == SETK_U8) ? 1U : 4U) == 0);
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
