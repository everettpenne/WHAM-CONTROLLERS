/*
 * settings_codec.h -- the saved-settings data model: a snapshot of every
 * operator-configurable SCALAR value, the table that describes each field,
 * the tagged-record encoding, and the CRC. No hardware and no other module's
 * state -- host-tested (tests/test_settings_codec.c). Reading the live
 * settings and applying them back is settings.h; storing encoded payloads in
 * flash is profile_store.h.
 *
 * Wire format of a payload: a sequence of records, each
 *     [field id u8][channel u8][length u8][value]
 * little-endian, channel 0xFF for a system-wide field. Records rather than a
 * raw struct so that a firmware version that adds, removes or reorders fields
 * can still read an older save: unknown ids are skipped, fields the save
 * doesn't have are "not present" and left alone on recall. FIELD IDS ARE A
 * FROZEN CONTRACT once shipped -- never renumber or reuse one; retire an id by
 * leaving it unused. (This is the same framing as XREX's codec; the field table
 * and the meaning of the channel byte are the product-specific part.)
 *
 * What differs from XREX:
 *   - the "channel" byte of a per-plant field is the PLANT index (0 = HVPS,
 *     1 = Limiter, closed_loop.h's CL_PLANT_*), not a Transrex channel: the
 *     gains are kept per supply type, whichever mode is selected;
 *   - three TABLES travel in the same payload, after the scalars: the open-loop
 *     shot (id 64/65), the closed-loop target (66/67) and the HVPS curve
 *     (68/69). Each table is a COUNT record (u16) then POINTS records, one per
 *     chunk of up to SET_TABLE_CHUNK_BYTES, the channel byte being the chunk
 *     index (so a table is at most 255 chunks). Tables are too big for RAM
 *     (about 10 KB) so they are streamed to and from flash by settings.c, never
 *     held in a snapshot.
 *
 * Three loops (2026-10-13, one per phase U, V, W): loop 1's values keep the ids and shape they always had (so an
 * older save recalls into loop 1 and a newer save is still readable by older firmware: it just does not see loops
 * 2 and 3). Loops 2 and 3 ("extra loops", index 0 and 1 in the channel byte) have their own ids: mode, feedback
 * input, scale, offset (19-22) and gains (35-37, channel = plant + 2 x extra loop). The shot and target tables
 * have a second, per-channel format (ids 70-73: one duty or voltage per phase in every point); a save uses it only
 * when the phases differ, so an ordinary profile is saved, byte for byte, as it always was.
 *
 * Deliberately NOT saved: the output/run state (ARM, FIRE, SOURce:RUN/SETpoint), which phases
 * are enabled (PHASe:ENAble -- a recall must never change which legs of the bridge are driven),
 * the DAC outputs, and every bench-simulation knob (CONFig:FEEDback:SOURce and
 * DACRatio, SIM:PLANT:*) -- a profile that turned the loop onto a simulated
 * feedback by itself would be a hazard on a real supply.
 */
#ifndef SETTINGS_CODEC_H
#define SETTINGS_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "ctrlr_config.h"

#define SET_CHANNEL_SYSTEM   (0xFFU)
#define SET_PLANT_COUNT      (2U)
#define SET_PLANT_HVPS       (0U)
#define SET_PLANT_LIMITER    (1U)
#define SET_XLOOPS           (HRTIM_NUM_CHANNELS - 1U)               /* the loops after the first */
#define SET_MAX_INSTANCES    (SET_XLOOPS * SET_PLANT_COUNT)          /* the most values one field has */

/* Field ids -- frozen, see above. */
typedef enum
{
    SETF_SUPPLY_MODE      = 1,    /* supply_mode_t: 1 LIMITER, 2 HVPS (CONFig:MODE) */
    SETF_PID_RATE_HZ      = 2,
    SETF_ADC_SCANS        = 3,    /* CONFig:ADC:AVERage */
    SETF_ADC_SAMPLE_X10   = 4,    /* CONFig:ADC:SAMPle, ADC clocks x10 */
    SETF_SLEW_PER_TICK    = 5,
    SETF_MAX_VOLTS        = 6,
    SETF_FB_CHANNEL       = 7,    /* 0-based feedback input */
    SETF_FB_SCALE         = 8,
    SETF_FB_OFFSET_V      = 9,
    SETF_FB_FILTER_HZ     = 10,
    SETF_FB_CLIP_MS       = 11,
    SETF_LOOP_MODE        = 12,   /* PID:LOOPMODE: 0 open loop, 1 closed loop */
    SETF_CL_HVPS_DUTY     = 13,
    SETF_CL_LIMITER_FREQ  = 14,
    SETF_CL_RESONANCE_HZ  = 15,
    SETF_CL_VPEAK         = 16,
    SETF_CL_VSCALE        = 17,
    SETF_CL_BRANCH        = 18,   /* 1 above resonance, 0 below */

    /* the extra loops (2 and 3): channel byte = loop - 2 */
    SETF_X_LOOP_MODE      = 19,
    SETF_X_FB_CHANNEL     = 20,
    SETF_X_FB_SCALE       = 21,
    SETF_X_FB_OFFSET_V    = 22,

    SETF_KP               = 32,   /* per plant (loop 1) */
    SETF_KI               = 33,
    SETF_KD               = 34,
    SETF_X_KP             = 35,   /* the extra loops' gains: channel = plant + SET_PLANT_COUNT x (loop - 2) */
    SETF_X_KI             = 36,
    SETF_X_KD             = 37,

    SETF_SHOT_COUNT       = 64,   /* tables: see above */
    SETF_SHOT_POINTS      = 65,
    SETF_TARGET_COUNT     = 66,
    SETF_TARGET_POINTS    = 67,
    SETF_CURVE_COUNT      = 68,
    SETF_CURVE_POINTS     = 69,
    SETF_SHOT3_COUNT      = 70,   /* the per-channel shot: a duty per phase in every point */
    SETF_SHOT3_POINTS     = 71,
    SETF_TARGET3_COUNT    = 72,   /* the per-channel target: a voltage per phase in every point */
    SETF_TARGET3_POINTS   = 73
} settings_field_id_t;

/* A table's point records: bytes per point and points per chunk (<= 255 bytes per record). */
#define SET_SHOT_POINT_BYTES    (10U)    /* t_us u32, freq_mHz u32, duty_hundredths u16 (every phase) */
#define SET_TARGET_POINT_BYTES  (8U)     /* t_us u32, millivolts i32 (every phase) */
#define SET_CURVE_POINT_BYTES   (8U)     /* hz f32, volts f32 */
#define SET_SHOT3_POINT_BYTES   (8U + 2U * HRTIM_NUM_CHANNELS)    /* t_us u32, freq_mHz u32, a duty_hundredths u16 per phase */
#define SET_TARGET3_POINT_BYTES (4U + 4U * HRTIM_NUM_CHANNELS)    /* t_us u32, a millivolts i32 per phase */
#define SET_SHOT_CHUNK_POINTS   (24U)
#define SET_TARGET_CHUNK_POINTS (30U)
#define SET_CURVE_CHUNK_POINTS  (30U)
#define SET_SHOT3_CHUNK_POINTS  (255U / SET_SHOT3_POINT_BYTES)
#define SET_TARGET3_CHUNK_POINTS (255U / SET_TARGET3_POINT_BYTES)

typedef enum { SETK_U8, SETK_U32, SETK_F32 } settings_kind_t;

/* What a field has one value PER (the channel byte of its records is that index) */
typedef enum
{
    SETS_SYSTEM = 0,         /* one value (channel byte 0xFF) */
    SETS_PLANT,              /* per plant: HVPS, Limiter (loop 1's gains) */
    SETS_XLOOP,              /* per extra loop (2, 3) */
    SETS_XLOOP_PLANT         /* per extra loop and plant: channel = plant + SET_PLANT_COUNT x extra loop */
} settings_scope_t;

typedef struct
{
    uint8_t         id;
    const char     *name;        /* wire/preview name; per-plant fields print as "<plant>.<name>", extra loops' as "loopN.<name>" */
    settings_kind_t kind;
    uint8_t         scope;       /* settings_scope_t */
    uint16_t        offset;      /* into settings_snapshot_t (system), settings_plant_t or settings_xloop_t */
} settings_field_t;

extern const settings_field_t kSettingsFields[];
extern const uint8_t          kSettingsFieldCount;

#define SET_PRESENT_BITS   (128U)   /* >= fieldCount * SET_MAX_INSTANCES */

typedef struct
{
    float kp;
    float ki;
    float kd;
} settings_plant_t;

/* One of the loops after the first (loop 2, loop 3) */
typedef struct
{
    uint8_t loopMode;
    uint8_t fbChannel;
    float   fbScale;
    float   fbOffsetV;
} settings_xloop_t;

typedef struct
{
    uint8_t  supplyMode;
    uint32_t pidRateHz;
    uint32_t adcScans;
    uint32_t adcSampleX10;
    float    slewPerTick;
    float    maxVolts;
    uint8_t  fbChannel;
    float    fbScale;
    float    fbOffsetV;
    float    fbFilterHz;
    float    fbClipMs;
    uint8_t  loopMode;
    float    clHvpsDuty;
    float    clLimiterFreq;
    float    clResonanceHz;
    float    clVpeak;
    float    clVscale;
    uint8_t  clBranch;
    settings_plant_t plant[SET_PLANT_COUNT];
    settings_xloop_t xloop[SET_XLOOPS];
    settings_plant_t xplant[SET_MAX_INSTANCES];     /* [extra loop * SET_PLANT_COUNT + plant] */
    /* Which (field, instance) values hold real data. Absent values are neither encoded, compared nor applied. */
    uint8_t  present[(SET_PRESENT_BITS + 7U) / 8U];
} settings_snapshot_t;

/* How many values a field has: 1, SET_PLANT_COUNT, SET_XLOOPS or SET_XLOOPS x SET_PLANT_COUNT (the channel index runs 0..n-1). */
uint8_t SettingsCodec_Instances(uint8_t fieldIdx);

void    SettingsCodec_Clear(settings_snapshot_t *s);
uint8_t SettingsCodec_IsPresent(const settings_snapshot_t *s, uint8_t fieldIdx, uint8_t ch);
void    SettingsCodec_SetPresent(settings_snapshot_t *s, uint8_t fieldIdx, uint8_t ch);

/* ---- encoding ---- */

/* Where encoded bytes go: return 1 to keep going, 0 to abort (the encoder then returns 0). */
typedef uint8_t (*settings_sink_fn)(void *ctx, const uint8_t *data, uint16_t n);

/* One record. */
uint8_t SettingsCodec_EmitRecord(settings_sink_fn sink, void *ctx, uint8_t id, uint8_t ch,
                                 const void *value, uint8_t len);

/* Every present scalar value, in the table's order. */
uint8_t SettingsCodec_EncodeScalars(const settings_snapshot_t *s, settings_sink_fn sink, void *ctx);

/* Convenience for tests and small payloads: the scalars into a buffer (capacity cap); the byte count, or 0
   if it does not fit. */
uint16_t SettingsCodec_Encode(const settings_snapshot_t *s, uint8_t *buf, uint16_t cap);

/* ---- decoding ---- */

typedef struct
{
    uint16_t records;     /* scalar records understood and stored */
    uint16_t tables;      /* table records seen (applied by settings.c, not stored in a snapshot) */
    uint16_t unknown;     /* unknown field ids skipped */
    uint16_t outOfRange;  /* plant index beyond SET_PLANT_COUNT, skipped */
} settings_decode_info_t;

/* One record as read from a payload. */
typedef struct
{
    uint8_t id;
    uint8_t ch;
    uint8_t len;
    uint8_t value[255];
} settings_record_t;

/* Reads n bytes at `offset` of a payload; returns 1 on success. */
typedef uint8_t (*settings_read_fn)(void *ctx, uint16_t offset, void *buf, uint16_t n);

typedef enum { SETREC_OK = 0, SETREC_END, SETREC_BAD } settings_rec_status_t;

/* Reads the record at *offset (of a payload `total` bytes long) and advances *offset past it. SETREC_END
   at the clean end of the payload, SETREC_BAD for a truncated record or a failed read. */
settings_rec_status_t SettingsCodec_NextRecord(settings_read_fn rd, void *ctx, uint16_t total,
                                               uint16_t *offset, settings_record_t *rec);

/* 1 if the id is one of the table ids (64..73). */
uint8_t SettingsCodec_IsTableId(uint8_t id);

/* Stores a scalar record into the snapshot. Returns 1 if stored or deliberately skipped (unknown id, plant
   out of range, table id: counted in `info`), 0 if the record is malformed for a KNOWN field (wrong length
   or channel shape). */
uint8_t SettingsCodec_StoreRecord(settings_snapshot_t *s, const settings_record_t *rec, settings_decode_info_t *info);

/* Decodes a whole buffered payload's scalars into *s (cleared first). 1 on success, 0 if malformed. */
uint8_t SettingsCodec_Decode(const uint8_t *buf, uint16_t len, settings_snapshot_t *s,
                             settings_decode_info_t *info);

/* ---- text and comparison, for CONFig:PROFile:* ---- */

/* Field value as text (floats as Scpi_FormatFloat (scpi_util.h), U8/U32 decimal). */
void SettingsCodec_FormatValue(const settings_snapshot_t *s, uint8_t fieldIdx, uint8_t ch,
                               char *out, size_t n);

/* Display name: "PIDRate" or "hvps.kp". */
void SettingsCodec_FormatName(uint8_t fieldIdx, uint8_t ch, char *out, size_t n);

/* 1 if the value of (fieldIdx, ch) is the same in both snapshots. */
uint8_t SettingsCodec_ValueEqual(const settings_snapshot_t *a, const settings_snapshot_t *b,
                                 uint8_t fieldIdx, uint8_t ch);

/* CRC-32 (zlib polynomial), continued: pass 0 for the first block. */
uint32_t SettingsCodec_Crc32(uint32_t crc, const uint8_t *data, size_t len);

/* Little-endian helpers for the table points. */
void     SettingsCodec_Put16(uint8_t *p, uint16_t v);
void     SettingsCodec_Put32(uint8_t *p, uint32_t v);
uint16_t SettingsCodec_Get16(const uint8_t *p);
uint32_t SettingsCodec_Get32(const uint8_t *p);

#endif /* SETTINGS_CODEC_H */
