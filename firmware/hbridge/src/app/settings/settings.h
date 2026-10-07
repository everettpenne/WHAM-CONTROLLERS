/*
 * settings.h -- ties the controller's live operator-configurable values to saved profiles:
 *   - captures the live scalar settings into a snapshot (settings_codec.h) and applies a snapshot back
 *     through the same setters the serial commands use, so every limit and cross-check still applies;
 *   - saves and recalls a WHOLE profile -- the scalars plus the shot, target and HVPS-curve tables -- to and
 *     from a flash slot (profile_store.h), STREAMING the tables: they are about 10 KB, more than free RAM,
 *     so they are encoded straight from the live table into flash and decoded straight from flash into it,
 *     never copied.
 * The snapshot model and encoding are settings_codec.h; the *SAV / *RCL / CONFig:PROFile commands tying them
 * together are cmd_profile.c.
 *
 * Callers apply only in IDLE (cmd_profile.c enforces that); several of the setters would refuse or misbehave
 * mid-shot. A recall is applied in a fixed order -- supply mode, then the ADC and loop timing, feedback,
 * limits and plant parameters, gains, loop mode, then the tables (which are checked against the mode and the
 * limits just applied) -- and a value or table a setter rejects is counted and named in the result while the
 * rest are still applied. A table that fails part way is left EMPTY rather than half-loaded (a shot or target
 * with a missing tail must never be runnable); a failed curve leaves the live one untouched.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include "settings_codec.h"
#include "profile_store.h"

/* Fills *s from the live scalar settings. */
void Settings_Capture(settings_snapshot_t *s);

typedef struct
{
    uint16_t applied;
    uint16_t failed;
    char     failedNames[96];   /* comma-separated, truncated if long */
} settings_apply_result_t;

/* Applies every scalar marked present in *s. *modeChanged is set if the supply mode was changed. */
void Settings_Apply(const settings_snapshot_t *s, settings_apply_result_t *r, uint8_t *modeChanged);

/* A table's identity for previews and comparisons: how many points, and a CRC over their packed bytes. */
typedef struct
{
    uint16_t count;
    uint32_t crc;
} settings_table_sum_t;

typedef struct
{
    settings_table_sum_t shot;
    settings_table_sum_t target;
    settings_table_sum_t curve;     /* count 0 = the linear default */
} settings_tables_t;

/* The live tables. */
void Settings_LiveTables(settings_tables_t *t);

/* Saves the live settings and tables to a slot. */
profile_status_t Settings_Save(uint8_t slot, const char *label, const char *fw);

typedef struct
{
    settings_apply_result_t apply;      /* scalars and tables together */
    uint8_t  modeChanged;
    uint16_t ignored;                   /* records skipped: unknown ids and out-of-range plants */
} settings_recall_result_t;

/* Applies a slot: scalars, then tables. */
profile_status_t Settings_Recall(uint8_t slot, settings_recall_result_t *r);

/* Reads a slot WITHOUT applying anything: its scalars, the tables' identities, and the header. */
profile_status_t Settings_Inspect(uint8_t slot, settings_snapshot_t *s, settings_tables_t *t,
                                  settings_decode_info_t *dinfo, profile_info_t *info);

#endif /* SETTINGS_H */
