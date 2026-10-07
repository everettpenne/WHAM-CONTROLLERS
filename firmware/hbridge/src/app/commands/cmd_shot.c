/*
 * cmd_shot.c -- serial command handlers: the time-series shot, SHOT:*.
 * See shot_profile.h for what a shot is and the rules it follows.
 *
 *   SHOT:BEGin                         start an upload (discards the previous shot)
 *   SHOT:STEP <t_us> <freq_hz> <duty_pct>
 *                                      one setpoint; t_us is an integer number of
 *                                      microseconds, frequency in Hz (up to 3
 *                                      decimals), duty in percent (up to 2).
 *                                      The first must be t = 0; times increase.
 *   SHOT:END                           finish; at least two setpoints
 *   SHOT:CLEar                         discard the shot
 *   SHOT?                              OK <setpoints> <duration_us> <PROFILE|TABLE>
 *   SHOT:POINt? <i>                    OK <t_us> <freq_hz> <duty_pct> (i from 0)
 *
 * Every setpoint is checked against the selected supply mode (CONFig:MODE)
 * as it is uploaded. Error codes are shared with TABle:* (cmd_common.h):
 * 2 no upload open, 3 full, 4 bad argument or ordering, 10 outside the mode's
 * limits, 13 no mode selected / not allowed while FIRING.
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "pfm.h"
#include "shot_profile.h"
#include "state_machine.h"
#include "supply_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_shotUploadActive = 0U;

static uint8_t RefuseWhileFiring(uart_instance_t *inst)
{
    if (SM_GetState() == SM_STATE_FIRING)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "Shot can't change while FIRING");
        return 1U;
    }
    return 0U;
}

/* Drops the shot and any upload in progress. Called on a supply-mode change
   (cmd_config.c): a shot checked against one mode's limits must not survive
   into another. */
void CmdShot_Discard(void)
{
    ShotProfile_Clear();
    g_shotUploadActive = 0U;
}

void cmd_shot_begin(uart_instance_t *inst, char *args)
{
    (void)args;

    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    if (SupplyMode_Get() == SUPPLY_MODE_NONE)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "No supply mode selected -- send CONFig:MODE LIMiter|HVPS first");
        return;
    }
    ShotProfile_Clear();
    g_shotUploadActive = 1U;
    Scpi_ReplyOk(inst);
}

static void SendLimitErr(uart_instance_t *inst, shot_append_result_t res)
{
    supply_limits_t lim;
    char msg[96];
    supply_mode_t mode = SupplyMode_Get();

    (void)SupplyMode_GetLimits(mode, &lim);
    if ((res == SHOT_APPEND_FREQ_LOW) || (res == SHOT_APPEND_FREQ_HIGH))
    {
        snprintf(msg, sizeof(msg), "frequency is %s the %s range (%lu-%lu Hz)",
                 (res == SHOT_APPEND_FREQ_LOW) ? "below" : "above", SupplyMode_Name(mode),
                 (unsigned long)lim.freqMinHz, (unsigned long)lim.freqMaxHz);
    }
    else
    {
        snprintf(msg, sizeof(msg), "duty is %s the %s range (%u-%u%%)",
                 (res == SHOT_APPEND_DUTY_LOW) ? "below" : "above", SupplyMode_Name(mode),
                 (unsigned)lim.dutyMinPct, (unsigned)lim.dutyMaxPct);
    }
    Scpi_SendErr(inst, ERR_OUT_OF_MODE_LIMITS, msg);
}

void cmd_shot_step(uart_instance_t *inst, char *args)
{
    char *tokT = NULL;
    char *tokF = NULL;
    char *tokD = NULL;
    char *extra = NULL;
    uint32_t tUs = 0U;
    uint32_t fMilli = 0U;
    uint32_t dHund = 0U;
    shot_append_result_t res;

    if (g_shotUploadActive == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_NOT_UPLOADING, "Not currently uploading -- send SHOT:BEGin first");
        return;
    }
    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    if (args != NULL)
    {
        tokT  = strtok(args, " \r\n");
        tokF  = (tokT != NULL) ? strtok(NULL, " \r\n") : NULL;
        tokD  = (tokF != NULL) ? strtok(NULL, " \r\n") : NULL;
        extra = (tokD != NULL) ? strtok(NULL, " \r\n") : NULL;
    }
    if ((tokD == NULL) || (extra != NULL))
    {
        Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "SHOT:STEP needs exactly three values: t_us freq_hz duty_pct");
        return;
    }
    if ((Scpi_ParseScaled(tokT, 0U, &tUs) == 0U) ||
        (Scpi_ParseScaled(tokF, 3U, &fMilli) == 0U) ||
        (Scpi_ParseScaled(tokD, 2U, &dHund) == 0U) || (dHund > 65535U))
    {
        Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID,
                "values must be non-negative decimals: t_us integer, freq_hz up to 3 decimals, duty_pct up to 2");
        return;
    }

    res = ShotProfile_Append(tUs, fMilli, (uint16_t)dHund);
    switch (res)
    {
        case SHOT_APPEND_OK:
            Scpi_ReplyOk(inst);
            break;
        case SHOT_APPEND_NO_MODE:
            Scpi_SendErr(inst, ERR_INVALID_STATE, "No supply mode selected -- send CONFig:MODE LIMiter|HVPS first");
            break;
        case SHOT_APPEND_FULL:
            Scpi_SendErr(inst, ERR_TABLE_FULL, "Shot full (SHOT_SETPOINT_MAX setpoints)");
            break;
        case SHOT_APPEND_FIRST_NOT_ZERO:
            Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "The first setpoint must be at t=0");
            break;
        case SHOT_APPEND_NOT_INCREASING:
            Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "Time must be greater than the previous setpoint's");
            break;
        case SHOT_APPEND_TOO_LONG:
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "Time is beyond the longest shot (%lu us)", (unsigned long)SHOT_MAX_DURATION_US);
            Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, msg);
            break;
        }
        default:
            SendLimitErr(inst, res);
            break;
    }
}

void cmd_shot_end(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;

    if (g_shotUploadActive == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_NOT_UPLOADING, "Not currently uploading -- send SHOT:BEGin first");
        return;
    }
    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    if (ShotProfile_IsComplete() == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "A shot needs at least two setpoints: t=0 and the end of the shot");
        return;
    }
    g_shotUploadActive = 0U;
    PFM_SelectSource(PFM_SOURCE_PROFILE);
    snprintf(buf, sizeof(buf), "OK %u %lu\r\n", (unsigned int)ShotProfile_Count(),
             (unsigned long)ShotProfile_DurationUs());
    uart_send(inst, buf);
}

void cmd_shot_clear(uart_instance_t *inst, char *args)
{
    (void)args;

    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    CmdShot_Discard();
    Scpi_ReplyOk(inst);
}

void cmd_shot_query(uart_instance_t *inst, char *args)
{
    char buf[64];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u %lu %s\r\n", (unsigned int)ShotProfile_Count(),
             (unsigned long)ShotProfile_DurationUs(),
             (PFM_GetSource() == PFM_SOURCE_PROFILE) ? "PROFILE" : "TABLE");
    uart_send(inst, buf);
}

void cmd_shot_point_query(uart_instance_t *inst, char *args)
{
    char buf[80];
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint32_t index = 0U;
    shot_setpoint_t sp;

    if ((tok == NULL) || (Scpi_ParseScaled(tok, 0U, &index) == 0U))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "SHOT:POINt? needs one argument: the setpoint index (from 0)");
        return;
    }
    if ((index > 0xFFFFU) || (ShotProfile_Get((uint16_t)index, &sp) == 0U))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "No such setpoint");
        return;
    }
    snprintf(buf, sizeof(buf), "OK %lu %lu.%03lu %u.%02u\r\n", (unsigned long)sp.tUs,
             (unsigned long)(sp.freqMilliHz / 1000U), (unsigned long)(sp.freqMilliHz % 1000U),
             (unsigned int)(sp.dutyHundredths / 100U), (unsigned int)(sp.dutyHundredths % 100U));
    uart_send(inst, buf);
}
