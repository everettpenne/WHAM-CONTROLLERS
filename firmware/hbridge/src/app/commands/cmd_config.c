/*
 * cmd_config.c -- serial command handlers: CONFig:CHANnels?, CONFig:MODE/MODE?/LIMits?.
 * Split out of the old single commands.c (2026-09-29); the handlers are
 * unchanged. See commands.h for the file map.
 */
#include "commands.h"
#include "cmd_common.h"
#include "state_machine.h"
#include "supply_mode.h"
#include "ctrlr_config.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* --------------------------------------------------------------------------
 * CONFig:CHANnels?
 *
 * Reports HRTIM_NUM_CHANNELS (ctrlr_config.h) -- added alongside the
 * 2026-09-08 channel-count generalization specifically so
 * python/pfm_table_upload.py (or any other host tool) can confirm what
 * a given board was actually built for, rather than assuming a value
 * that could silently drift from reality after a rebuild with a
 * different channel count -- a TABLE:STEP wire-format mismatch that
 * would otherwise only surface as a confusing ERR 4 partway through an
 * upload.
 * -------------------------------------------------------------------------- */
void cmd_config_channels(uart_instance_t *inst, char *args)
{
    char buf[32];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned int)HRTIM_NUM_CHANNELS);
    uart_send(inst, buf);
}

/* --------------------------------------------------------------------------
 * CONFig:MODE / CONFig:MODE? / CONFig:LIMits?
 *
 * Selects which H-bridge supply this controller is driving (supply_mode.h):
 * LIMiter (PWM, 75-125 kHz, 5-95% duty) or HVPS (PFM, 10-30 kHz, 40-60%
 * duty). TABle:STEP is checked against the selected mode; no table can be
 * uploaded until one is selected. Boot state: NONE.
 *
 * Only accepted in IDLE (ERR 13), and a mode CHANGE discards the uploaded
 * table (TABle? -> 0) so it can never outlive the limits it was checked
 * against; re-selecting the current mode changes nothing. Arguments are
 * matched case-insensitively: LIMITER or LIM, HVPS.
 * -------------------------------------------------------------------------- */
void cmd_config_mode(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    supply_mode_t mode;

    if (tok == NULL)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "CONFig:MODE needs one argument: LIMiter or HVPS");
        return;
    }
    if ((strcasecmp(tok, "LIM") == 0) || (strcasecmp(tok, "LIMITER") == 0))
    {
        mode = SUPPLY_MODE_LIMITER;
    }
    else if (strcasecmp(tok, "HVPS") == 0)
    {
        mode = SUPPLY_MODE_HVPS;
    }
    else
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "mode must be LIMiter or HVPS");
        return;
    }
    if (SM_GetState() != SM_STATE_IDLE)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "CONFig:MODE only in IDLE");
        return;
    }
    if (SupplyMode_Set(mode) != 0U)
    {
        CmdTable_Discard();
        CmdShot_Discard();
    }
    Scpi_ReplyOk(inst);
}

void cmd_config_mode_query(uart_instance_t *inst, char *args)
{
    char buf[24];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %s\r\n", SupplyMode_Name(SupplyMode_Get()));
    uart_send(inst, buf);
}

/* OK <mode> FREQ <min> <max> DUTY <min> <max> (Hz, percent), or "OK NONE". */
void cmd_config_limits_query(uart_instance_t *inst, char *args)
{
    char buf[80];
    supply_limits_t lim;
    (void)args;

    if (SupplyMode_GetLimits(SupplyMode_Get(), &lim) == 0U)
    {
        uart_send(inst, "OK NONE\r\n");
        return;
    }
    snprintf(buf, sizeof(buf), "OK %s FREQ %lu %lu DUTY %u %u\r\n", SupplyMode_Name(SupplyMode_Get()),
             (unsigned long)lim.freqMinHz, (unsigned long)lim.freqMaxHz,
             (unsigned)lim.dutyMinPct, (unsigned)lim.dutyMaxPct);
    uart_send(inst, buf);
}
