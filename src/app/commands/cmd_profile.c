/*
 * cmd_profile.c -- saved settings profiles: *SAV, *RCL and the
 * CONFig:PROFile:* commands (catalog, preview, compare, clear).
 *
 * Added 2026-09-29. Eight numbered slots in the external flash keep a
 * snapshot of the operator-configurable settings (which ones:
 * settings_codec.h). Loading is always explicit -- nothing is applied at
 * boot -- and *SAV, *RCL and CLEar work only in IDLE (they erase/write
 * flash, or change live settings). The read-only commands (CATalog?,
 * PROFile?, DIFF?) work in any state.
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
#define PAYLOAD_BUF_BYTES  (512U)
static uint8_t             g_payload[PAYLOAD_BUF_BYTES];
static settings_snapshot_t g_live;
static settings_snapshot_t g_saved;

/* Parses the slot argument into 0-based *slot; sends ERR 12 and returns 0
   on a missing/invalid one. */
static uint8_t ParseSlot(uart_instance_t *inst, const char *tok, uint8_t *slot)
{
    long v;
    char *end = NULL;

    if (tok == NULL)
    {
        SendErr(inst, ERR_INVALID_ARGS, "needs a slot number");
        return 0U;
    }
    v = strtol(tok, &end, 10);
    if (end == tok || *end != '\0' || v < 1L || v > (long)PROFILE_SLOT_COUNT)
    {
        SendErr(inst, ERR_INVALID_ARGS, "slot must be 1-8");
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
    SendErr(inst, ERR_INVALID_STATE, msg);
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
    SendErr(inst, ERR_PROFILE, msg);
}

/* Loads slot's payload and decodes it into g_saved. Sends the error itself
   and returns 0 on failure. */
static uint8_t LoadSaved(uart_instance_t *inst, uint8_t slot, profile_info_t *info,
                         settings_decode_info_t *dinfo)
{
    uint16_t len = 0U;
    profile_status_t rc = ProfileStore_Read(slot, g_payload, sizeof(g_payload), &len, info);

    if (rc != PROFILE_OK)
    {
        SendProfileErr(inst, slot, rc);
        return 0U;
    }
    if (SettingsCodec_Decode(g_payload, len, &g_saved, dinfo) == 0U)
    {
        SendErr(inst, ERR_PROFILE, "saved data could not be decoded");
        return 0U;
    }
    return 1U;
}

/* *SAV <slot> [label] -- save the current settings. */
void cmd_sav(uart_instance_t *inst, char *args)
{
    char *tok;
    char *label;
    uint8_t slot;
    uint16_t n;
    profile_status_t rc;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (ParseSlot(inst, tok, &slot) == 0U)
    {
        return;
    }
    label = strtok(NULL, " \r\n");
    if (label != NULL && (strlen(label) > PROFILE_LABEL_MAX || strcmp(label, "-") == 0))
    {
        SendErr(inst, ERR_INVALID_ARGS, "label must be 1-15 characters, no spaces (\"-\" is reserved)");
        return;
    }
    if (RequireIdle(inst, "*SAV") == 0U)
    {
        return;
    }

    Settings_Capture(&g_live);
    n = SettingsCodec_Encode(&g_live, g_payload, sizeof(g_payload));
    if (n == 0U)
    {
        SendErr(inst, ERR_PROFILE, "settings do not fit the working buffer");
        return;
    }
    rc = ProfileStore_Write(slot, label, FW_VERSION_STRING, g_payload, n);
    if (rc != PROFILE_OK)
    {
        SendProfileErr(inst, slot, rc);
        return;
    }
    uart_send(inst, "OK\r\n");
}

/* *RCL <slot> -- apply a saved profile. Reply: OK APPLIED=<n> FAILED=<k>
   [<names>] [IGNORED=<m>]. */
void cmd_rcl(uart_instance_t *inst, char *args)
{
    char *tok;
    uint8_t slot;
    profile_info_t info;
    settings_decode_info_t dinfo;
    settings_apply_result_t res;
    char buf[192];
    int n;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (ParseSlot(inst, tok, &slot) == 0U || RequireIdle(inst, "*RCL") == 0U)
    {
        return;
    }
    if (LoadSaved(inst, slot, &info, &dinfo) == 0U)
    {
        return;
    }
    Settings_Apply(&g_saved, &res);

    n = snprintf(buf, sizeof(buf), "OK APPLIED=%u FAILED=%u", (unsigned int)res.applied, (unsigned int)res.failed);
    if (res.failed != 0U && n > 0 && (size_t)n < sizeof(buf))
    {
        n += snprintf(&buf[n], sizeof(buf) - (size_t)n, " %s", res.failedNames);
    }
    if ((dinfo.unknown + dinfo.outOfRange) != 0U && n > 0 && (size_t)n < sizeof(buf))
    {
        n += snprintf(&buf[n], sizeof(buf) - (size_t)n, " IGNORED=%u", (unsigned int)(dinfo.unknown + dinfo.outOfRange));
    }
    if (n > 0 && (size_t)n < sizeof(buf) - 3U)
    {
        strcat(buf, "\r\n");
        uart_send(inst, buf);
    }
    else
    {
        uart_send(inst, "OK\r\n");
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

/* CONFig:PROFile? <slot> -- OK label=<l> fw=<v> <name>=<value> ...; nothing is applied. */
void cmd_profile_query(uart_instance_t *inst, char *args)
{
    char *tok;
    uint8_t slot;
    profile_info_t info;
    char name[24];
    char val[24];
    char buf[64];

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (ParseSlot(inst, tok, &slot) == 0U || LoadSaved(inst, slot, &info, NULL) == 0U)
    {
        return;
    }
    snprintf(buf, sizeof(buf), "OK label=%s fw=%s", (info.label[0] != '\0') ? info.label : "-",
             (info.fw[0] != '\0') ? info.fw : "-");
    uart_send(inst, buf);
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        uint8_t chCount = (kSettingsFields[fi].perChannel != 0U) ? (uint8_t)HRTIM_NUM_CHANNELS : 1U;

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
    uart_send(inst, "\r\n");
}

/* CONFig:PROFile:DIFF? <slot> -- OK SAME, or OK <name>=<live>><saved> ... for
   every saved value that differs from the live one. Nothing is applied. */
void cmd_profile_diff(uart_instance_t *inst, char *args)
{
    char *tok;
    uint8_t slot;
    profile_info_t info;
    char name[24];
    char live[24];
    char saved[24];
    char buf[80];
    uint16_t differing = 0U;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (ParseSlot(inst, tok, &slot) == 0U || LoadSaved(inst, slot, &info, NULL) == 0U)
    {
        return;
    }
    Settings_Capture(&g_live);

    uart_send(inst, "OK");
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        uint8_t chCount = (kSettingsFields[fi].perChannel != 0U) ? (uint8_t)HRTIM_NUM_CHANNELS : 1U;

        for (uint8_t c = 0U; c < chCount; c++)
        {
            if (SettingsCodec_IsPresent(&g_saved, fi, c) == 0U ||
                SettingsCodec_ValueEqual(&g_live, &g_saved, fi, c) != 0U)
            {
                continue;
            }
            SettingsCodec_FormatName(fi, c, name, sizeof(name));
            SettingsCodec_FormatValue(&g_live, fi, c, live, sizeof(live));
            SettingsCodec_FormatValue(&g_saved, fi, c, saved, sizeof(saved));
            snprintf(buf, sizeof(buf), " %s=%s>%s", name, live, saved);
            uart_send(inst, buf);
            differing++;
        }
    }
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
    uart_send(inst, "OK\r\n");
}
