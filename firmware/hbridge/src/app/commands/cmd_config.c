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
#include "hrtim.h"
#include "state_machine.h"
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
 * CONFig:DEADtime <ns> / CONFig:DEADtime?      (added 2026-11-16)
 *
 * The dead time between a phase's output and its complement, one value for every timing unit (every phase). Default 100 ns
 * at every boot (HRTIM_DEADTIME_COUNTS, hrtim.h); RAM only, not saved in a profile. One HRTIM count is 5.88 ns, so the
 * value is rounded: CONFig:DEADtime? replies the dead time in force in ns (100 -> 100, 120 -> 118). Range
 * HRTIM_DEADTIME_MIN_NS..HRTIM_DEADTIME_MAX_NS (50-3000 ns). Not while FIRING (the register is rewritten in place): ERR 13.
 * Too small a value risks shoot-through on a real bridge: the hub announces it as a limits change.
 *
 *   CONFig:DEADtime 150     OK            ERR 12 not a whole number or outside 50-3000, ERR 13 while FIRING
 *   CONFig:DEADtime?        OK 150
 * -------------------------------------------------------------------------- */
void cmd_config_deadtime(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    long ns;

    if ((tok == NULL) || (strtok(NULL, " \r\n") != NULL))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "CONFig:DEADtime needs one argument: nanoseconds (50-3000)");
        return;
    }
    SCPI_NEED_LONG(inst, tok, ns);
    if ((ns < (long)HRTIM_DEADTIME_MIN_NS) || (ns > (long)HRTIM_DEADTIME_MAX_NS))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "dead time must be 50-3000 ns");
        return;
    }
    if (SM_GetState() == SM_STATE_FIRING)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "CONFig:DEADtime not while FIRING");
        return;
    }
    (void)HRTIM1_SetDeadTimeNs((uint32_t)ns);
    Scpi_ReplyOk(inst);
}

void cmd_config_deadtime_query(uart_instance_t *inst, char *args)
{
    char buf[32];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %lu\r\n", (unsigned long)HRTIM1_GetDeadTimeNs());
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
