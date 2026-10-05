/*
 * cmd_profile.c -- saved settings profiles: *SAV, *RCL and the CONFig:PROFile:* commands (catalog,
 * preview, compare, clear). Same commands, replies and rules as WHAM-XREX-PFMG474's cmd_profile.c.
 *
 * Eight numbered slots in the external flash keep a snapshot of the operator-configurable settings AND the
 * shot, target and HVPS-curve tables (which ones: settings_codec.h). Loading is always explicit -- nothing is
 * applied at boot -- and *SAV, *RCL and CLEar work only in IDLE (they erase/write flash, or change live
 * settings). The read-only commands (CATalog?, PROFile?, DIFF?) work in any state.
 *
 * A *SAV or *RCL takes a fraction of a second (the sector erases and page programs block the main loop; the
 * serial link is not serviced meanwhile).
 *
 * Slot numbers on the wire are 1..PROFILE_SLOT_COUNT.
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "state_machine.h"
#include "settings.h"
#include "profile_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Working memory for one command at a time, kept out of the stack. */
static settings_snapshot_t g_live;
static settings_snapshot_t g_saved;

/* Parses the slot argument into 0-based *slot; sends ERR 12 and returns 0 on a missing/invalid one. */
static uint8_t ParseSlot(uart_instance_t *inst, const char *tok, uint8_t *slot)
{
    long v = 0L;

    if (tok == NULL)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "needs a slot number");
        return 0U;
    }
    if (Scpi_ParseLong(tok, &v) == 0U || v < 1L || v > (long)PROFILE_SLOT_COUNT)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "slot must be 1-8");
        return 0U;
    }
    *slot = (uint8_t)(v - 1L);
    return 1U;
}

static uint8_t RequireIdle(uart_instance_t *inst, const char *what)
{
    char msg[48];

    if (SM_GetState() == SM_STATE_IDLE)
    {
        return 1U;
    }
    snprintf(msg, sizeof(msg), "%s only in IDLE", what);
    Scpi_SendErr(inst, ERR_INVALID_STATE, msg);
    return 0U;
}

static void SendProfileErr(uart_instance_t *inst, uint8_t slot, profile_status_t rc)
{
    char msg[64];

    if (rc == PROFILE_ERR_STORAGE)
    {
        snprintf(msg, sizeof(msg), "slot %u: flash: %s", (unsigned int)(slot + 1U),
                 NvStore_StatusText(ProfileStore_StorageStatus()));
    }
    else
    {
        snprintf(msg, sizeof(msg), "slot %u: %s", (unsigned int)(slot + 1U), ProfileStore_StatusText(rc));
    }
    Scpi_SendErr(inst, ERR_PROFILE, msg);
}

/* *SAV <slot> [label] -- save the current settings and tables. */
void cmd_sav(uart_instance_t *inst, char *args)
{
    char *tok;
    char *label;
    uint8_t slot;
    profile_status_t rc;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (ParseSlot(inst, tok, &slot) == 0U)
    {
        return;
    }
    label = strtok(NULL, " \r\n");
    if (label != NULL && (strlen(label) > PROFILE_LABEL_MAX || strcmp(label, "-") == 0))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "label must be 1-15 characters, no spaces (\"-\" is reserved)");
        return;
    }
    if (RequireIdle(inst, "*SAV") == 0U)
    {
        return;
    }
    rc = Settings_Save(slot, label, FW_VERSION_STRING);
    if (rc != PROFILE_OK)
    {
        SendProfileErr(inst, slot, rc);
        return;
    }
    Scpi_ReplyOk(inst);
}

/* *RCL <slot> -- apply a saved profile. Reply: OK APPLIED=<n> FAILED=<k> [<names>] [IGNORED=<m>]. */
void cmd_rcl(uart_instance_t *inst, char *args)
{
    char *tok;
    uint8_t slot;
    profile_info_t info;
    settings_recall_result_t res;
    profile_status_t rc;
    char buf[192];
    int n;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (ParseSlot(inst, tok, &slot) == 0U || RequireIdle(inst, "*RCL") == 0U)
    {
        return;
    }
    /* Look before touching anything: a recall of an empty or corrupt slot must leave the live tables alone. */
    rc = ProfileStore_Info(slot, &info);
    if (rc == PROFILE_OK && info.state != PROFILE_SLOT_VALID)
    {
        rc = (info.state == PROFILE_SLOT_EMPTY) ? PROFILE_ERR_EMPTY : PROFILE_ERR_CORRUPT;
    }
    if (rc != PROFILE_OK)
    {
        SendProfileErr(inst, slot, rc);
        return;
    }
    /* The recall replaces the shot and target (and may change the supply mode, which always dropped the table
       and the shot); drop them and any upload in progress first. */
    CmdTable_Discard();
    CmdShot_Discard();
    CmdTarget_Discard();

    rc = Settings_Recall(slot, &res);
    if (rc != PROFILE_OK)
    {
        SendProfileErr(inst, slot, rc);
        return;
    }
    n = snprintf(buf, sizeof(buf), "OK APPLIED=%u FAILED=%u", (unsigned int)res.apply.applied,
                 (unsigned int)res.apply.failed);
    if (res.apply.failed != 0U && n > 0 && (size_t)n < sizeof(buf))
    {
        n += snprintf(&buf[n], sizeof(buf) - (size_t)n, " %s", res.apply.failedNames);
    }
    if (res.ignored != 0U && n > 0 && (size_t)n < sizeof(buf))
    {
        n += snprintf(&buf[n], sizeof(buf) - (size_t)n, " IGNORED=%u", (unsigned int)res.ignored);
    }
    if (n > 0 && (size_t)n < sizeof(buf) - 3U)
    {
        strcat(buf, "\r\n");
        uart_send(inst, buf);
    }
    else
    {
        Scpi_ReplyOk(inst);
    }
}

/* CONFig:PROFile:CATalog? -- OK 1=<label>/<fw> 2=empty 3=corrupt ... */
void cmd_profile_catalog(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;

    uart_send(inst, "OK");
    for (uint8_t s = 0U; s < PROFILE_SLOT_COUNT; s++)
    {
        profile_info_t info;
        profile_status_t rc = ProfileStore_Info(s, &info);

        if (rc != PROFILE_OK)
        {
            uart_send(inst, "\r\n");
            SendProfileErr(inst, s, rc);
            return;
        }
        if (info.state == PROFILE_SLOT_VALID)
        {
            snprintf(buf, sizeof(buf), " %u=%s/%s", (unsigned int)(s + 1U),
                     (info.label[0] != '\0') ? info.label : "-", (info.fw[0] != '\0') ? info.fw : "-");
        }
        else
        {
            snprintf(buf, sizeof(buf), " %u=%s", (unsigned int)(s + 1U),
                     (info.state == PROFILE_SLOT_CORRUPT) ? "corrupt" : "empty");
        }
        uart_send(inst, buf);
    }
    uart_send(inst, "\r\n");
}

/* Reads a slot for a preview; sends the error and returns 0 on failure. */
static uint8_t Inspect(uart_instance_t *inst, uint8_t slot, settings_tables_t *t, profile_info_t *info)
{
    profile_status_t rc = Settings_Inspect(slot, &g_saved, t, NULL, info);

    if (rc != PROFILE_OK)
    {
        if (rc == PROFILE_ERR_CORRUPT)
        {
            Scpi_SendErr(inst, ERR_PROFILE, "saved data could not be decoded");
        }
        else
        {
            SendProfileErr(inst, slot, rc);
        }
        return 0U;
    }
    return 1U;
}

/* CONFig:PROFile? <slot> -- OK label=<l> fw=<v> <name>=<value> ... shot=<n> target=<n> curve=<n|LINEAR>;
   nothing is applied. */
void cmd_profile_query(uart_instance_t *inst, char *args)
{
    char *tok;
    uint8_t slot;
    profile_info_t info;
    settings_tables_t tables;
    char name[40];
    char val[24];
    char buf[96];

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (ParseSlot(inst, tok, &slot) == 0U || Inspect(inst, slot, &tables, &info) == 0U)
    {
        return;
    }
    snprintf(buf, sizeof(buf), "OK label=%s fw=%s", (info.label[0] != '\0') ? info.label : "-",
             (info.fw[0] != '\0') ? info.fw : "-");
    uart_send(inst, buf);
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        uint8_t chCount = (kSettingsFields[fi].perPlant != 0U) ? (uint8_t)SET_PLANT_COUNT : 1U;

        for (uint8_t c = 0U; c < chCount; c++)
        {
            if (SettingsCodec_IsPresent(&g_saved, fi, c) == 0U)
            {
                continue;
            }
            SettingsCodec_FormatName(fi, c, name, sizeof(name));
            SettingsCodec_FormatValue(&g_saved, fi, c, val, sizeof(val));
            snprintf(buf, sizeof(buf), " %s=%s", name, val);
            uart_send(inst, buf);
        }
    }
    snprintf(buf, sizeof(buf), " shot=%u target=%u", (unsigned int)tables.shot.count, (unsigned int)tables.target.count);
    uart_send(inst, buf);
    if (tables.curve.count == 0U)
    {
        uart_send(inst, " curve=LINEAR");
    }
    else
    {
        snprintf(buf, sizeof(buf), " curve=%u", (unsigned int)tables.curve.count);
        uart_send(inst, buf);
    }
    uart_send(inst, "\r\n");
}

/* One table's difference, appended to a DIFF? reply: name=<live>><saved> (counts), with a * if only the contents
   differ. Returns 1 if it differed. */
static uint8_t DiffTable(uart_instance_t *inst, const char *name, const settings_table_sum_t *live,
                         const settings_table_sum_t *saved)
{
    char buf[48];

    if (live->count == saved->count && live->crc == saved->crc)
    {
        return 0U;
    }
    snprintf(buf, sizeof(buf), " %s=%u>%u%s", name, (unsigned int)live->count, (unsigned int)saved->count,
             (live->count == saved->count) ? "*" : "");
    uart_send(inst, buf);
    return 1U;
}

/* CONFig:PROFile:DIFF? <slot> -- OK SAME, or OK <name>=<live>><saved> ... for every saved value that differs
   from the live one (tables by point count, "*" if only the points differ). Nothing is applied. */
void cmd_profile_diff(uart_instance_t *inst, char *args)
{
    char *tok;
    uint8_t slot;
    profile_info_t info;
    settings_tables_t saved;
    settings_tables_t live;
    char name[40];
    char lv[24];
    char sv[24];
    char buf[96];
    uint16_t differing = 0U;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (ParseSlot(inst, tok, &slot) == 0U || Inspect(inst, slot, &saved, &info) == 0U)
    {
        return;
    }
    Settings_Capture(&g_live);
    Settings_LiveTables(&live);

    uart_send(inst, "OK");
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        uint8_t chCount = (kSettingsFields[fi].perPlant != 0U) ? (uint8_t)SET_PLANT_COUNT : 1U;

        for (uint8_t c = 0U; c < chCount; c++)
        {
            if (SettingsCodec_IsPresent(&g_saved, fi, c) == 0U ||
                SettingsCodec_ValueEqual(&g_live, &g_saved, fi, c) != 0U)
            {
                continue;
            }
            SettingsCodec_FormatName(fi, c, name, sizeof(name));
            SettingsCodec_FormatValue(&g_live, fi, c, lv, sizeof(lv));
            SettingsCodec_FormatValue(&g_saved, fi, c, sv, sizeof(sv));
            snprintf(buf, sizeof(buf), " %s=%s>%s", name, lv, sv);
            uart_send(inst, buf);
            differing++;
        }
    }
    differing = (uint16_t)(differing + DiffTable(inst, "shot", &live.shot, &saved.shot));
    differing = (uint16_t)(differing + DiffTable(inst, "target", &live.target, &saved.target));
    differing = (uint16_t)(differing + DiffTable(inst, "curve", &live.curve, &saved.curve));
    uart_send(inst, (differing == 0U) ? " SAME\r\n" : "\r\n");
}

/* CONFig:PROFile:CLEar <slot> -- erase a slot. */
void cmd_profile_clear(uart_instance_t *inst, char *args)
{
    char *tok;
    uint8_t slot;
    profile_status_t rc;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (ParseSlot(inst, tok, &slot) == 0U || RequireIdle(inst, "CONFig:PROFile:CLEar") == 0U)
    {
        return;
    }
    rc = ProfileStore_Clear(slot);
    if (rc != PROFILE_OK)
    {
        SendProfileErr(inst, slot, rc);
        return;
    }
    Scpi_ReplyOk(inst);
}
