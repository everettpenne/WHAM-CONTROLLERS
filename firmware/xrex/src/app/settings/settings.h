/*
 * settings.h -- reads the controller's live operator-configurable values
 * into a snapshot, and applies a snapshot back through the same setters
 * the serial commands use (so every limit and cross-check still applies).
 * The snapshot model and its encoding are settings_codec.h; where the
 * encoded snapshots are kept is profile_store.h; the *SAV / *RCL /
 * CONFig:PROFile commands tying them together are cmd_profile.c.
 *
 * Callers apply only in IDLE (cmd_profile.c enforces that); several of
 * the setters would refuse or misbehave mid-shot.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include "settings_codec.h"

/* Fills *s from the live settings. Timing fields are marked present only
   if a shot timing has ever been set. */
void Settings_Capture(settings_snapshot_t *s);

typedef struct
{
    uint16_t applied;
    uint16_t failed;
    char     failedNames[96];   /* comma-separated, truncated if long */
} settings_apply_result_t;

/* Applies every value marked present in *s. A value a setter rejects is
   counted and named in the result; the others are still applied. */
void Settings_Apply(const settings_snapshot_t *s, settings_apply_result_t *r);

#endif /* SETTINGS_H */
