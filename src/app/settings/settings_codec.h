/*
 * settings_codec.h -- the saved-settings data model: a snapshot of every
 * operator-configurable value, the table that describes each field, and
 * the pure functions that encode/decode/compare snapshots. No hardware
 * and no other module's state -- host-tested (tests/test_settings_codec.c).
 * Reading the live settings and applying a snapshot to them is
 * settings.h; storing encoded snapshots in flash is profile_store.h.
 *
 * Wire format of an encoded snapshot (the "payload" a slot stores): a
 * sequence of records, each  [field id u8][channel u8][length u8][value],
 * little-endian, channel 0xFF for a system-wide field and 0..N-1 for a
 * per-channel one. Records rather than a raw struct so that a firmware
 * version that adds, removes or reorders fields can still read an older
 * save: unknown ids are skipped, fields the save doesn't have are simply
 * "not present" and left alone on recall. FIELD IDS ARE A FROZEN
 * CONTRACT once shipped -- never renumber or reuse one; retire an id by
 * leaving it unused.
 *
 * Deliberately NOT saved (see the design agreed 2026-09-29): setpoints,
 * channel enables, the EXTernal:ENAble interlock, DEBUG:FAULT:BYPASS, and
 * every run/arm/fault action. CONFig:MaxCarrierHz is not saved either: it
 * only affects the legacy TABle:/FIRE path.
 */
#ifndef SETTINGS_CODEC_H
#define SETTINGS_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "ctrlr_config.h"

#define SET_NICKNAME_MAX   (15U)   /* == PID_CHANNEL_NICKNAME_MAX_LEN */
#define SET_CHANNEL_SYSTEM (0xFFU)

typedef struct
{
    float   kp;
    float   ki;
    float   kd;
    uint8_t closedLoop;
    float   maxCurrentA;
    float   demandCurrentA;
    char    nickname[SET_NICKNAME_MAX + 1U];
} settings_channel_t;

/* Field ids -- frozen, see above. */
typedef enum
{
    SETF_PID_RATE_HZ      = 1,
    SETF_TURNON_HZ        = 2,
    SETF_MAX_FREQ_HZ      = 3,
    SETF_SLEW_HZ_PER_TICK = 4,
    SETF_FAULT_RAMP_S     = 5,
    SETF_POL_WATER        = 6,
    SETF_POL_TEMP         = 7,
    SETF_POL_ENERPRO      = 8,
    SETF_POL_OCP          = 9,
    /* 10: reserved (CONFig:MaxCarrierHz, deliberately not saved) */
    SETF_GPOUT_ENABLE     = 11,
    SETF_PWMALT_ENABLE    = 12,
    SETF_RAMP_UP_MS       = 13,
    SETF_FLAT_TOP_MS      = 14,
    SETF_RAMP_DOWN_MS     = 15,

    SETF_KP               = 32,
    SETF_KI               = 33,
    SETF_KD               = 34,
    SETF_CLOSED_LOOP      = 35,
    SETF_MAX_CURRENT_A    = 36,
    SETF_DEMAND_CURRENT_A = 37,
    SETF_NICKNAME         = 38
} settings_field_id_t;

typedef enum { SETK_U8, SETK_U32, SETK_F32, SETK_STR } settings_kind_t;

typedef struct
{
    uint8_t         id;
    const char     *name;        /* wire/preview name; channel fields print as "ch<n>.<name>" */
    settings_kind_t kind;
    uint8_t         perChannel;  /* 1: one value per channel */
    uint16_t        offset;      /* into settings_snapshot_t (system) or settings_channel_t */
} settings_field_t;

extern const settings_field_t kSettingsFields[];
extern const uint8_t          kSettingsFieldCount;

#define SET_PRESENT_BITS   (64U * HRTIM_NUM_CHANNELS)   /* >= fieldCount * channels */

typedef struct
{
    uint32_t pidRateHz;
    uint32_t turnonHz;
    uint32_t maxFreqHz;
    float    slewHzPerTick;
    float    faultRampS;
    uint8_t  polWater;
    uint8_t  polTemp;
    uint8_t  polEnerpro;
    uint8_t  polOcp;
    uint8_t  gpoutEnable;
    uint8_t  pwmAltEnable;
    uint32_t rampUpMs;
    uint32_t flatTopMs;
    uint32_t rampDownMs;
    settings_channel_t ch[HRTIM_NUM_CHANNELS];
    /* Which (field, channel) values hold real data. Absent values are
       neither encoded, compared nor applied. */
    uint8_t  present[(SET_PRESENT_BITS + 7U) / 8U];
} settings_snapshot_t;

void    SettingsCodec_Clear(settings_snapshot_t *s);
uint8_t SettingsCodec_IsPresent(const settings_snapshot_t *s, uint8_t fieldIdx, uint8_t ch);
void    SettingsCodec_SetPresent(settings_snapshot_t *s, uint8_t fieldIdx, uint8_t ch);

/* Encodes every present value into buf (capacity cap). Returns the byte
   count, or 0 if it does not fit. */
uint16_t SettingsCodec_Encode(const settings_snapshot_t *s, uint8_t *buf, uint16_t cap);

typedef struct
{
    uint16_t records;     /* records understood and stored */
    uint16_t unknown;     /* unknown field ids skipped */
    uint16_t outOfRange;  /* channel beyond HRTIM_NUM_CHANNELS, skipped */
} settings_decode_info_t;

/* Decodes a payload into *s (cleared first). Returns 1 on success, 0 if
   the payload is malformed (truncated record, wrong length for a known
   field). info may be NULL. */
uint8_t SettingsCodec_Decode(const uint8_t *buf, uint16_t len, settings_snapshot_t *s,
                             settings_decode_info_t *info);

/* Field value as text (floats as Scpi_FormatFloat (scpi_util.h), U8/U32 decimal, empty nickname "-"). */
void SettingsCodec_FormatValue(const settings_snapshot_t *s, uint8_t fieldIdx, uint8_t ch,
                               char *out, size_t n);

/* Display name: "PIDRate" or "ch2.kp". */
void SettingsCodec_FormatName(uint8_t fieldIdx, uint8_t ch, char *out, size_t n);

/* 1 if the value of (fieldIdx, ch) is the same in both snapshots. */
uint8_t SettingsCodec_ValueEqual(const settings_snapshot_t *a, const settings_snapshot_t *b,
                                 uint8_t fieldIdx, uint8_t ch);

/* CRC-32 (zlib polynomial), continued: pass 0 for the first block. */
uint32_t SettingsCodec_Crc32(uint32_t crc, const uint8_t *data, size_t len);

#endif /* SETTINGS_CODEC_H */
