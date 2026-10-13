/*
 * cmd_control.c -- serial command handlers for the closed loop, mirroring the XREX pattern
 * (docs/command_reference.md in WHAM-XREX-PFMG474) where HBRIDGE has the same thing:
 *
 *   PID:LOOPMODE <ch> <0|1> / ?      0 open loop (the SHOT:* profile), 1 closed loop (the TARGet:* profile)
 *   PID:GAINS <ch> <kp> <ki> <kd> / ?   gains of the SELECTED SUPPLY MODE's loop (Hz/V for HVPS, %/V for Limiter)
 *   SOURce:RUN / STOP                 closed-loop hold of SOURce:SETpoint, until stopped
 *   SOURce:SETpoint <ch> <volts> / ?  the hold target
 *   SOURce:STATus? <ch>               OK <running> <setpointV> <measuredV> <output Hz|%>
 *   TARGet:BEGin / STEP <t_us> <volts> [<volts> <volts>] / END / CLEar / ? / POINt? <i>   the target-voltage time series
 *   LOG:ARM <ch> <max> <decim> / LOG:DATA? <ch> [<start> [<count>]]
 *   PID:FAULT? / PID:DIAG?            which loop stopped the last run and why / the control tick's timing
 *   CONFig:PIDRate, :SLEWRate, :MAXVolts, :FEEDback:*, :ADC:*, :CLOSEDloop:*, SIM:PLANT:*
 *
 * `<ch>` is the loop: 1, 2, 3 = phase U, V, W (XREX's channel argument, kept so tooling written for XREX
 * carries over); 0 where XREX allows it means "all loops". Where a command takes the loop OPTIONALLY
 * (CONFig:FEEDback:CHANnel/SCALE/OFFSet/RANGE) leaving it out means loop 1, as before there were three.
 * FIRE plays the shot open- or closed-loop according to each phase's PID:LOOPMODE (cmd_table.c).
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "control_runtime.h"
#include "target_profile.h"
#include "hvps_curve.h"
#include "shot_profile.h"
#include "state_machine.h"
#include "supply_mode.h"
#include "adc.h"
#include "pfm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- small helpers ---- */

static uint8_t ParseSignedVolts(const char *tok, float *out)
{
    int32_t mv = 0;

    if (Target_ParseMilliVolts(tok, &mv) == 0U)
    {
        return 0U;
    }
    *out = (float)mv / 1000.0f;
    return 1U;
}

/* ctl_set_result_t is what scpi_util.h's SCPI_SET_* mean */
typedef char ctl_set_ok_matches[(CTL_SET_OK == SCPI_SET_OK) ? 1 : -1];
typedef char ctl_set_range_matches[(CTL_SET_RANGE == SCPI_SET_RANGE) ? 1 : -1];
typedef char ctl_set_busy_matches[(CTL_SET_BUSY == SCPI_SET_BUSY) ? 1 : -1];

#define CTL_BUSY_MSG  "Not while a closed-loop run is in progress"

static void ReplySet(uart_instance_t *inst, ctl_set_result_t r, const char *rangeMsg)
{
    Scpi_ReplySet(inst, (int)r, rangeMsg, CTL_BUSY_MSG);
}

/* The loop argument: 1..3 (phase U, V, W), or 0 for "all" where allowZero. *loop gets the index 0..2, or
   CTL_ALL_LOOPS for 0. Sends ERR 11 and returns 0 otherwise. */
static uint8_t ParseLoop(uart_instance_t *inst, const char *tok, uint8_t allowZero, uint8_t *loop)
{
    uint32_t v = 0U;

    if ((tok == NULL) || (Scpi_ParseScaled(tok, 0U, &v) == 0U) || (v > CTL_LOOPS) || ((v == 0U) && (allowZero == 0U)))
    {
        Scpi_SendErr(inst, ERR_INVALID_CHANNEL, "Invalid loop (1-3 = phase U, V, W; 0 = all where accepted)");
        return 0U;
    }
    *loop = (v == 0U) ? CTL_ALL_LOOPS : (uint8_t)(v - 1U);
    return 1U;
}

static uint8_t RefuseWhileFiring(uart_instance_t *inst)
{
    if (SM_GetState() == SM_STATE_FIRING)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "Can't change while FIRING");
        return 1U;
    }
    return 0U;
}

/* Reports why a closed-loop start failed; shared with FIRE (cmd_table.c). Returns 1 if `r` was an error. */
uint8_t Cmd_ReportCtlResult(uart_instance_t *inst, ctl_result_t r)
{
    switch (r)
    {
        case CTL_OK:
            return 0U;
        case CTL_NO_MODE:
            Scpi_SendErr(inst, ERR_INVALID_STATE, "No supply mode selected -- send CONFig:MODE LIMiter|HVPS first");
            break;
        case CTL_NO_TARGET:
            Scpi_SendErr(inst, ERR_TABLE_EMPTY, "No target profile -- upload one first (TARGet:BEGin/STEP/END)");
            break;
        case CTL_BAD_CONFIG:
            Scpi_SendErr(inst, ERR_INVALID_ARGS, "Closed-loop configuration unusable (the HVPS resonance leaves no band inside the mode's range, the CURVe table is invalid, or an HVPS target is below 0 V)");
            break;
        case CTL_NO_FEEDBACK:
            Scpi_SendErr(inst, ERR_INVALID_STATE, "The ADC sampler is not running");
            break;
        case CTL_REF_BAD:
            Scpi_SendErr(inst, ERR_INVALID_STATE, "VREF+ is not at ANALOG_VREF_MV (check ADC:VREF?)");
            break;
        case CTL_NO_SHOT:
            Scpi_SendErr(inst, ERR_TABLE_EMPTY, "An open-loop phase has no shot: upload one (SHOT:*), close its loop (PID:LOOPMODE) or disable it (PHASe:ENAble)");
            break;
        case CTL_NEED_CLOSED:
            Scpi_SendErr(inst, ERR_INVALID_STATE, "SOURce:RUN needs every enabled phase closed (PID:LOOPMODE 0 1) or the open ones disabled (PHASe:ENAble)");
            break;
        case CTL_NO_METER:
            Scpi_SendErr(inst, ERR_INVALID_STATE, "SOURce LOOP needs Phase U enabled (PHASe:ENAble) and PFM_Input_01 free (no PFMIN capture running)");
            break;
        case CTL_BUSY:
            Scpi_SendErr(inst, ERR_INVALID_STATE, "A closed-loop run is already in progress");
            break;
        default:
            Scpi_SendErr(inst, ERR_INVALID_ARGS, "Closed-loop configuration unusable (nothing to close)");
            break;
    }
    return 1U;
}

/* ---- PID:* ---- */

static void ReplyBits(uart_instance_t *inst, const uint8_t *vals, uint8_t n)
{
    char buf[32] = "OK";
    size_t used = 2U;

    for (uint8_t i = 0U; (i < n) && (used + 3U < sizeof(buf)); i++)
    {
        buf[used++] = ' ';
        buf[used++] = (char)('0' + vals[i]);
        buf[used] = '\0';
    }
    snprintf(&buf[used], sizeof(buf) - used, "\r\n");
    uart_send(inst, buf);
}

void cmd_pid_loopmode(uart_instance_t *inst, char *args)
{
    char *tokCh = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokV  = (tokCh != NULL) ? strtok(NULL, " \r\n") : NULL;
    uint8_t loop = 0U;

    if (ParseLoop(inst, tokCh, 1U, &loop) == 0U)
    {
        return;
    }
    if ((tokV == NULL) || ((strcmp(tokV, "0") != 0) && (strcmp(tokV, "1") != 0)))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "PID:LOOPMODE needs <ch> <0|1>");
        return;
    }
    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    Control_SetLoopMode(loop, (uint8_t)(tokV[0] - '0'));
    Scpi_ReplyOk(inst);
}

/* PID:LOOPMODE? <ch>: OK <0|1>; with 0 every loop: OK <U> <V> <W> */
void cmd_pid_loopmode_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint8_t loop = 0U;
    uint8_t modes[CTL_LOOPS];

    if (ParseLoop(inst, tok, 1U, &loop) == 0U)
    {
        return;
    }
    if (loop == CTL_ALL_LOOPS)
    {
        for (uint8_t i = 0U; i < CTL_LOOPS; i++) { modes[i] = Control_GetLoopMode(i); }
        ReplyBits(inst, modes, CTL_LOOPS);
        return;
    }
    modes[0] = Control_GetLoopMode(loop);
    ReplyBits(inst, modes, 1U);
}

void cmd_pid_gains(uart_instance_t *inst, char *args)
{
    char *t[4] = { NULL, NULL, NULL, NULL };
    float kp, ki, kd;
    uint8_t loop = 0U;

    if (args != NULL)
    {
        t[0] = strtok(args, " \r\n");
        for (int i = 1; (i < 4) && (t[i - 1] != NULL); i++) { t[i] = strtok(NULL, " \r\n"); }
    }
    if (ParseLoop(inst, t[0], 1U, &loop) == 0U)
    {
        return;
    }
    if ((t[3] == NULL) || (strtok(NULL, " \r\n") != NULL) || (Scpi_ParseUnsigned(t[1], 3U, &kp) == 0U) ||
        (Scpi_ParseUnsigned(t[2], 3U, &ki) == 0U) || (Scpi_ParseUnsigned(t[3], 3U, &kd) == 0U))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "PID:GAINS needs <ch> <kp> <ki> <kd>, each non-negative with up to 3 decimals (max about 4 million)");
        return;
    }
    if (SupplyMode_Get() == SUPPLY_MODE_NONE)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "No supply mode selected: the gains belong to the selected supply's loop");
        return;
    }
    ReplySet(inst, Control_SetGains(loop, kp, ki, kd), "gains must be non-negative");
}

void cmd_pid_gains_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    float kp, ki, kd;
    char a[24], b[24], c[24], buf[96];
    uint8_t loop = 0U;

    if (ParseLoop(inst, tok, 0U, &loop) == 0U)
    {
        return;
    }
    Control_GetGains(loop, &kp, &ki, &kd);
    Scpi_FormatFixed(a, sizeof(a), kp, 3U);
    Scpi_FormatFixed(b, sizeof(b), ki, 3U);
    Scpi_FormatFixed(c, sizeof(c), kd, 3U);
    snprintf(buf, sizeof(buf), "OK %s %s %s\r\n", a, b, c);
    uart_send(inst, buf);
}

/* PID:FAULT? -> OK <loop 1-3, 0 = none> <NONE|OVERVOLTAGE|CLIPPED|CLIPPED_LOW>: which loop stopped the last run, and why.
   (FAULT:SOURces? says that the closed loop did; this says which loop.) */
void cmd_pid_fault_query(uart_instance_t *inst, char *args)
{
    static const char *const kKind[4] = { "NONE", "OVERVOLTAGE", "CLIPPED", "CLIPPED_LOW" };
    char buf[48];
    uint8_t loop, kind;
    (void)args;

    Control_GetFault(&loop, &kind);
    snprintf(buf, sizeof(buf), "OK %u %s\r\n", (unsigned)loop, kKind[kind & 3U]);
    uart_send(inst, buf);
}

/* PID:DIAG? -> OK <ticks> <longest tick, cycles> <mean tick, cycles> <cycles in one tick period>: the control tick's
   cost over the closed-loop runs since boot (CPU cycles at 170 MHz). A longest tick near the period means the next
   ADC block is late. */
void cmd_pid_diag_query(uart_instance_t *inst, char *args)
{
    char buf[80];
    uint32_t ticks, maxC, sumC, budget;
    (void)args;

    Control_GetDiag(&ticks, &maxC, &sumC, &budget);
    snprintf(buf, sizeof(buf), "OK %lu %lu %lu %lu\r\n", (unsigned long)ticks, (unsigned long)maxC,
             (unsigned long)((ticks != 0U) ? (sumC / ticks) : 0U), (unsigned long)budget);
    uart_send(inst, buf);
}

/* ---- SOURce:* ---- */

void cmd_source_run(uart_instance_t *inst, char *args)
{
    (void)args;

    if (AnyFaultLatched() != 0U)
    {
        Scpi_SendErr(inst, ERR_FAULT_LATCHED, "Fault latched -- send FAULT:CLEAR first");
        return;
    }
    if (SM_GetState() != SM_STATE_ARMED)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "Not ARMED -- send ARM first");
        return;
    }
    if (Control_ClosedRunWanted() == 0U)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "No enabled phase is closed loop (PID:LOOPMODE): SOURce:RUN is a closed-loop hold");
        return;
    }
    if (Cmd_ReportCtlResult(inst, Control_Prepare(0U)) != 0U)
    {
        return;
    }
    if (SM_Fire() == 0U)
    {
        Control_Stop();
        Scpi_SendErr(inst, ERR_INVALID_STATE, "SOURce:RUN refused -- state changed");
        return;
    }
    Control_Go();
    Scpi_ReplyOk(inst);
}

void cmd_source_stop(uart_instance_t *inst, char *args)
{
    (void)args;
    Control_Stop();
    Scpi_ReplyOk(inst);
}

void cmd_source_setpoint(uart_instance_t *inst, char *args)
{
    char *tokCh = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokV  = (tokCh != NULL) ? strtok(NULL, " \r\n") : NULL;
    float v;
    uint8_t loop = 0U;

    if (ParseLoop(inst, tokCh, 1U, &loop) == 0U)
    {
        return;
    }
    if ((tokV == NULL) || (ParseSignedVolts(tokV, &v) == 0U))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "SOURce:SETpoint needs <ch> <volts> (signed, up to 3 decimals)");
        return;
    }
    if ((v > Control_GetMaxVolts()) || (v < -Control_GetMaxVolts()))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "Setpoint is beyond CONFig:MAXVolts");
        return;
    }
    Control_SetHoldVolts(loop, v);
    Scpi_ReplyOk(inst);
}

void cmd_source_setpoint_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint8_t loop = 0U;

    if (ParseLoop(inst, tok, 0U, &loop) == 0U)
    {
        return;
    }
    Scpi_ReplyFixed(inst, Control_GetHoldVolts(loop), 3U);
}

void cmd_source_status(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    float sp, pv, out;
    char a[24], b[24], c[24], buf[96];
    uint8_t loop = 0U;

    if (ParseLoop(inst, tok, 0U, &loop) == 0U)
    {
        return;
    }
    Control_GetStatus(loop, &sp, &pv, &out);
    Scpi_FormatFixed(a, sizeof(a), sp, 3U);
    Scpi_FormatFixed(b, sizeof(b), pv, 3U);
    Scpi_FormatFixed(c, sizeof(c), out, 2U);
    snprintf(buf, sizeof(buf), "OK %u %s %s %s\r\n", (unsigned)Control_IsRunning(), a, b, c);
    uart_send(inst, buf);
}

/* ---- TARGet:* : the target voltage as a time series ---- */

static uint8_t g_targetUploadActive = 0U;

void CmdTarget_Discard(void)
{
    Target_Clear();
    g_targetUploadActive = 0U;
}

void cmd_target_begin(uart_instance_t *inst, char *args)
{
    (void)args;
    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    Target_Clear();
    g_targetUploadActive = 1U;
    Scpi_ReplyOk(inst);
}

void cmd_target_step(uart_instance_t *inst, char *args)
{
    char *tok[1U + HRTIM_NUM_CHANNELS + 1U];      /* t, the voltages, and one more to catch an extra */
    uint8_t nTok = 0U;
    uint32_t tUs = 0U;
    int32_t mv[HRTIM_NUM_CHANNELS];
    int32_t limit = (int32_t)(Control_GetMaxVolts() * 1000.0f);

    if (g_targetUploadActive == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_NOT_UPLOADING, "Not currently uploading -- send TARGet:BEGin first");
        return;
    }
    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    if (args != NULL)
    {
        char *p = strtok(args, " \r\n");

        while ((p != NULL) && (nTok < (uint8_t)(sizeof(tok) / sizeof(tok[0]))))
        {
            tok[nTok++] = p;
            p = strtok(NULL, " \r\n");
        }
        if (p != NULL)
        {
            nTok = 0xFFU;
        }
    }
    /* t and ONE voltage for every phase, or t and one voltage per phase */
    if ((nTok != 2U) && (nTok != (uint8_t)(1U + HRTIM_NUM_CHANNELS)))
    {
        Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "TARGet:STEP needs t_us volts (every phase) or t_us volts_U volts_V volts_W");
        return;
    }
    if (Scpi_ParseScaled(tok[0], 0U, &tUs) == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "t_us must be a non-negative integer, volts a signed decimal (3 places)");
        return;
    }
    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        if (Target_ParseMilliVolts(tok[(nTok == 2U) ? 1U : (1U + ch)], &mv[ch]) == 0U)
        {
            Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "t_us must be a non-negative integer, volts a signed decimal (3 places)");
            return;
        }
        if ((mv[ch] < 0) && (SupplyMode_Get() == SUPPLY_MODE_HVPS))
        {
            Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "HVPS targets are power: not below 0 V");
            return;
        }
    }
    switch (Target_Append(tUs, mv, limit))
    {
        case TARGET_APPEND_OK:
            Scpi_ReplyOk(inst);
            break;
        case TARGET_APPEND_FULL:
            Scpi_SendErr(inst, ERR_TABLE_FULL, "Target profile full (TARGET_SETPOINT_MAX setpoints)");
            break;
        case TARGET_APPEND_FIRST_NOT_ZERO:
            Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "The first setpoint must be at t=0");
            break;
        case TARGET_APPEND_NOT_INCREASING:
            Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "Time must be greater than the previous setpoint's");
            break;
        case TARGET_APPEND_TOO_LONG:
            Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "Time is beyond the longest shot (SHOT_MAX_DURATION_US)");
            break;
        default:
        {
            static const char kPhase[HRTIM_NUM_CHANNELS] = { 'U', 'V', 'W' };
            char msg[64];

            snprintf(msg, sizeof(msg), "Target voltage of phase %c is beyond CONFig:MAXVolts", kPhase[Target_BadChannel() % HRTIM_NUM_CHANNELS]);
            Scpi_SendErr(inst, ERR_OUT_OF_MODE_LIMITS, msg);
            break;
        }
    }
}

void cmd_target_end(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;

    if (g_targetUploadActive == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_NOT_UPLOADING, "Not currently uploading -- send TARGet:BEGin first");
        return;
    }
    if (Target_IsComplete() == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "A target profile needs at least two setpoints: t=0 and the end of the shot");
        return;
    }
    g_targetUploadActive = 0U;
    snprintf(buf, sizeof(buf), "OK %u %lu\r\n", (unsigned)Target_Count(), (unsigned long)Target_DurationUs());
    uart_send(inst, buf);
}

void cmd_target_clear(uart_instance_t *inst, char *args)
{
    (void)args;
    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    CmdTarget_Discard();
    Scpi_ReplyOk(inst);
}

void cmd_target_query(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u %lu\r\n", (unsigned)Target_Count(), (unsigned long)Target_DurationUs());
    uart_send(inst, buf);
}

/* TARGet:POINt? <i> -> OK <t_us> <volts>, or OK <t_us> <volts_U> <volts_V> <volts_W> when the phases differ */
void cmd_target_point_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint32_t i = 0U;
    target_setpoint_t sp;
    char v[24], buf[96];
    int len;
    uint8_t same = 1U;

    if ((tok == NULL) || (Scpi_ParseScaled(tok, 0U, &i) == 0U) || (i > 0xFFFFU))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "TARGet:POINt? needs one argument: the setpoint index (from 0)");
        return;
    }
    if (Target_Get((uint16_t)i, &sp) == 0U)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "No such setpoint");
        return;
    }
    len = snprintf(buf, sizeof(buf), "OK %lu", (unsigned long)sp.tUs);
    for (uint8_t ch = 1U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        if (sp.milliVolts[ch] != sp.milliVolts[0]) { same = 0U; }
    }
    for (uint8_t ch = 0U; ch < ((same != 0U) ? 1U : HRTIM_NUM_CHANNELS); ch++)
    {
        Scpi_FormatFixed(v, sizeof(v), (float)sp.milliVolts[ch] / 1000.0f, 3U);
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " %s", v);
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");
    uart_send(inst, buf);
}

/* ---- LOG:* ---- */

void cmd_log_arm(uart_instance_t *inst, char *args)
{
    char *t[3] = { NULL, NULL, NULL };
    uint32_t max = 0U, decim = 0U;
    uint8_t loop = 0U;

    if (args != NULL)
    {
        t[0] = strtok(args, " \r\n");
        t[1] = (t[0] != NULL) ? strtok(NULL, " \r\n") : NULL;
        t[2] = (t[1] != NULL) ? strtok(NULL, " \r\n") : NULL;
    }
    if (ParseLoop(inst, t[0], 1U, &loop) == 0U)
    {
        return;
    }
    if ((t[2] == NULL) || (Scpi_ParseScaled(t[1], 0U, &max) == 0U) || (Scpi_ParseScaled(t[2], 0U, &decim) == 0U) ||
        (max < 1U) || (max > Control_LogMaxSamples(loop)) || (decim < 1U) || (decim > 10000U))
    {
        char msg[160];

        snprintf(msg, sizeof(msg), "LOG:ARM needs <ch> <maxSamples 1-%u> <decim 1-10000> (the cap is a third as big for ch 0 = every loop)",
                 (unsigned)Control_LogMaxSamples(loop));
        Scpi_SendErr(inst, ERR_INVALID_ARGS, msg);
        return;
    }
    Control_LogArm(loop, (uint16_t)max, (uint16_t)decim);
    Scpi_ReplyOk(inst);
}

/* LOG:DATA? <ch> [<start> [<count>]] -- OK <count> <rateHz> SUM=<s> sp1 pv1 out1 sp2 ... (millivolts, millivolts,
   milli-Hz or milli-percent) of loop <ch> (1-3; the loop that LOG:ARM <ch> armed, or any when it armed 0). A page of
   at most 40 samples per reply: the ethernet serial bridge loses characters in long replies, and SUM (the sum of
   the numbers mod 2^32) lets the host tell. */
void cmd_log_data(uart_instance_t *inst, char *args)
{
    char *t[3] = { NULL, NULL, NULL };
    uint32_t start = 0U, want = 40U;
    uint16_t count = Control_LogCount();
    static char buf[40U * 3U * 12U + 64U];
    uint32_t sum = 0U;
    uint16_t last;
    int len;
    uint8_t loop = 0U;

    if (args != NULL)
    {
        t[0] = strtok(args, " \r\n");
        t[1] = (t[0] != NULL) ? strtok(NULL, " \r\n") : NULL;
        t[2] = (t[1] != NULL) ? strtok(NULL, " \r\n") : NULL;
    }
    if (ParseLoop(inst, t[0], 0U, &loop) == 0U)
    {
        return;
    }
    if (((t[1] != NULL) && (Scpi_ParseScaled(t[1], 0U, &start) == 0U)) ||
        ((t[2] != NULL) && ((Scpi_ParseScaled(t[2], 0U, &want) == 0U) || (want < 1U))) || (start > count))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "LOG:DATA? <ch> [<start> [<count>]]");
        return;
    }
    if (want > 40U) { want = 40U; }
    last = (uint16_t)(((start + want) > count) ? count : (start + want));
    {
        int32_t a, b, c;

        if ((count != 0U) && (Control_LogGet(loop, 0U, &a, &b, &c) == 0U))
        {
            Scpi_SendErr(inst, ERR_INVALID_ARGS, "That loop was not logged (LOG:ARM <ch> armed another loop)");
            return;
        }
    }
    for (uint16_t i = (uint16_t)start; i < last; i++)
    {
        int32_t a, b, c;
        (void)Control_LogGet(loop, i, &a, &b, &c);
        sum += (uint32_t)a + (uint32_t)b + (uint32_t)c;
    }
    len = snprintf(buf, sizeof(buf), "OK %u %lu SUM=%lu", (unsigned)count, (unsigned long)Control_LogRateHz(), (unsigned long)sum);
    for (uint16_t i = (uint16_t)start; i < last; i++)
    {
        int32_t a, b, c;
        (void)Control_LogGet(loop, i, &a, &b, &c);
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " %ld %ld %ld", (long)a, (long)b, (long)c);
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");
    uart_send(inst, buf);
}

/* ---- settings: one generated pair (set, query) per float setting ---- */

#define FLOAT_SETTING(FN, SETTER, GETTER, DEC, USAGE, RANGEMSG) \
    SCPI_FLOAT_SETTING(FN, SETTER, GETTER, DEC, USAGE, RANGEMSG, CTL_BUSY_MSG)

/* A float setting that belongs to a loop: "<value>" sets loop 1 (what it did before there were three loops),
   "<loop> <value>" sets that loop, loop 0 every loop; the query takes the loop the same way ("?" = loop 1). */
#define LOOP_FLOAT_SETTING(FN, SETTER, GETTER, DEC, USAGE, RANGEMSG) \
void FN(uart_instance_t *inst, char *args) \
{ \
    char *a_ = (args != NULL) ? strtok(args, " \r\n") : NULL; \
    char *b_ = (a_ != NULL) ? strtok(NULL, " \r\n") : NULL; \
    uint8_t loop_ = 0U; \
    float v_; \
    if ((a_ == NULL) || (strtok(NULL, " \r\n") != NULL)) \
    { \
        Scpi_SendErr(inst, ERR_INVALID_ARGS, USAGE); \
        return; \
    } \
    if ((b_ != NULL) && (ParseLoop(inst, a_, 1U, &loop_) == 0U)) \
    { \
        return; \
    } \
    if (Scpi_ParseUnsigned((b_ != NULL) ? b_ : a_, (DEC), &v_) == 0U) \
    { \
        Scpi_SendErr(inst, ERR_INVALID_ARGS, USAGE); \
        return; \
    } \
    ReplySet(inst, SETTER(loop_, v_), RANGEMSG); \
} \
void FN##_query(uart_instance_t *inst, char *args) \
{ \
    char *a_ = (args != NULL) ? strtok(args, " \r\n") : NULL; \
    uint8_t loop_ = 0U; \
    if ((a_ != NULL) && (ParseLoop(inst, a_, 0U, &loop_) == 0U)) \
    { \
        return; \
    } \
    Scpi_ReplyFixed(inst, GETTER(loop_), (DEC)); \
}

static ctl_set_result_t SetRate(float v)  { return Control_SetRateHz((uint32_t)(v + 0.5f)); }
static float            GetRate(void)     { return (float)Control_GetRateHz(); }
static ctl_set_result_t SetScans(float v) { return Control_SetScansPerTick((uint16_t)(v + 0.5f)); }
static float            GetScans(void)    { return (float)Control_GetScansPerTick(); }
static ctl_set_result_t SetSmp(float v)   { return Control_SetSampleTimeX10((uint16_t)(v * 10.0f + 0.5f)); }
static float            GetSmp(void)      { return (float)Control_GetSampleTimeX10() / 10.0f; }

FLOAT_SETTING(cmd_config_pid_rate,   SetRate,  GetRate,  0U, "CONFig:PIDRate <hz>",
              "rate must be 1000-20000 Hz and the ADC scan must fit in 1/(rate x scans per tick)")
FLOAT_SETTING(cmd_config_adc_average, SetScans, GetScans, 0U, "CONFig:ADC:AVERage <scans per tick>",
              "scans per tick must be 1-64 and the ADC scan must fit in 1/(rate x scans)")
FLOAT_SETTING(cmd_config_adc_sample, SetSmp,   GetSmp,   1U, "CONFig:ADC:SAMPle <clocks>: 2.5 6.5 12.5 24.5 47.5 92.5 247.5 640.5",
              "sampling time must be 2.5 6.5 12.5 24.5 47.5 92.5 247.5 or 640.5, and the scan must still fit")
FLOAT_SETTING(cmd_config_slew_rate,  Control_SetSlewPerTick, Control_GetSlewPerTick, 3U, "CONFig:SLEWRate <Hz or % per tick, 0 = off>",
              "slew must be >= 0")
FLOAT_SETTING(cmd_config_max_volts,  Control_SetMaxVolts, Control_GetMaxVolts, 3U, "CONFig:MAXVolts <volts>",
              "max volts must be > 0 and <= 10000")
LOOP_FLOAT_SETTING(cmd_config_fb_scale,   Control_SetFeedbackScale, Control_GetFeedbackScale, 4U, "CONFig:FEEDback:SCALE [<loop 0-3>] <process volts per ADC volt>",
              "scale must be > 0")
LOOP_FLOAT_SETTING(cmd_config_fb_offset,  Control_SetFeedbackOffset, Control_GetFeedbackOffset, 4U, "CONFig:FEEDback:OFFSet [<loop 0-3>] <ADC volts that read as 0>",
              "offset must be 0 to the ADC full scale")
LOOP_FLOAT_SETTING(cmd_config_fb_range,   Control_SetFeedbackRange, Control_GetFeedbackRange, 3U, "CONFig:FEEDback:RANGE [<loop 0-3>] <Vmax in process volts>",
              "Vmax must be > 0 and <= 100000")
FLOAT_SETTING(cmd_config_fb_dacratio, Control_SetDacRatio, Control_GetDacRatio, 4U, "CONFig:FEEDback:DACRatio <ADC volts per DAC volt>",
              "DAC ratio must be 0.05 to 2")
FLOAT_SETTING(cmd_config_fb_filter,  Control_SetFeedbackFilterHz, Control_GetFeedbackFilterHz, 1U, "CONFig:FEEDback:FILTer <cutoff Hz, 0 = off>",
              "cutoff must be 0 or below half the tick rate")
FLOAT_SETTING(cmd_config_fb_clip,    Control_SetClipMs, Control_GetClipMs, 1U, "CONFig:FEEDback:CLIPTime <ms, 0 = off>",
              "clip time must be 0-1000 ms")

FLOAT_SETTING(cmd_config_cl_duty,  Control_SetHvpsDuty, Control_GetHvpsDuty,  2U, "CONFig:CLOSEDloop:DUTY <percent>",
              "HVPS duty must be inside the HVPS mode's duty range")
FLOAT_SETTING(cmd_config_cl_freq,  Control_SetLimiterFreq, Control_GetLimiterFreq,  0U, "CONFig:CLOSEDloop:FREQuency <Hz>",
              "Limiter carrier must be inside the Limiter mode's frequency range")
FLOAT_SETTING(cmd_config_cl_res,   Control_SetHvpsResonance, Control_GetHvpsResonance,   0U, "CONFig:CLOSEDloop:RESonance <Hz>",
              "resonance must be inside the HVPS mode's frequency range")
FLOAT_SETTING(cmd_config_cl_vpeak, Control_SetHvpsVpeak, Control_GetHvpsVpeak, 3U, "CONFig:CLOSEDloop:VPEAK <volts at resonance>",
              "Vpeak must be > 0")
FLOAT_SETTING(cmd_config_cl_vscale, Control_SetLimiterVscale, Control_GetLimiterVscale, 3U, "CONFig:CLOSEDloop:VSCALE <volts at |2D-1| = 1>",
              "Vscale must be > 0")

void cmd_config_cl_branch(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;

    if (tok == NULL)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "CONFig:CLOSEDloop:BRANCH ABOVE|BELOW");
        return;
    }
    if (Control_IsRunning() != 0U)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "Not while a closed-loop run is in progress");
        return;
    }
    if (strcasecmp(tok, "ABOVE") == 0)       { (void)Control_SetHvpsAbove(1U); }
    else if (strcasecmp(tok, "BELOW") == 0)  { (void)Control_SetHvpsAbove(0U); }
    else
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "branch must be ABOVE or BELOW resonance");
        return;
    }
    Scpi_ReplyOk(inst);
}

void cmd_config_cl_branch_query(uart_instance_t *inst, char *args)
{
    (void)args;
    uart_send(inst, (Control_Loop()->cfg.hvpsAbove != 0U) ? "OK ABOVE\r\n" : "OK BELOW\r\n");
}

/* CONFig:FEEDback:CHANnel [<loop 0-3>] <1-6 | PA0 PA1 PC0 PB0 PC1 PB1>
   One argument sets loop 1 (as it did before there were three loops); with a loop first it sets that loop, 0 every loop.
   The defaults are phase U on PA0, V on PA1 and W on PB0. */
static const char *const kPinNames[ADC_NUM_CHANNELS] = { "PA0", "PA1", "PC0", "PB0", "PC1", "PB1" };

void cmd_config_fb_channel(uart_instance_t *inst, char *args)
{
    char *a = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *b = (a != NULL) ? strtok(NULL, " \r\n") : NULL;
    const char *tok;
    uint32_t n = 0U;
    int idx = -1;
    uint8_t loop = 0U;

    if ((a == NULL) || (strtok(NULL, " \r\n") != NULL))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "CONFig:FEEDback:CHANnel [<loop 0-3>] <1-6 or PA0 PA1 PC0 PB0 PC1 PB1>");
        return;
    }
    if ((b != NULL) && (ParseLoop(inst, a, 1U, &loop) == 0U))
    {
        return;
    }
    tok = (b != NULL) ? b : a;
    if ((Scpi_ParseScaled(tok, 0U, &n) != 0U) && (n >= 1U) && (n <= ADC_NUM_CHANNELS))
    {
        idx = (int)n - 1;
    }
    else
    {
        for (int i = 0; i < (int)ADC_NUM_CHANNELS; i++)
        {
            if (strcasecmp(tok, kPinNames[i]) == 0) { idx = i; }
        }
    }
    if (idx < 0)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "channel must be 1-6 or one of PA0 PA1 PC0 PB0 PC1 PB1");
        return;
    }
    ReplySet(inst, Control_SetFeedbackChannel(loop, (uint8_t)idx), "bad channel");
}

void cmd_config_fb_channel_query(uart_instance_t *inst, char *args)
{
    char buf[32];
    char *a = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint8_t loop = 0U;
    uint8_t c;

    if ((a != NULL) && (ParseLoop(inst, a, 0U, &loop) == 0U))
    {
        return;
    }
    c = Control_GetFeedbackChannel(loop);
    snprintf(buf, sizeof(buf), "OK %u %s\r\n", (unsigned)(c + 1U), kPinNames[c]);
    uart_send(inst, buf);
}

/* CONFig:FEEDback:SOURce ADC|SIM */
void cmd_config_fb_source(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint8_t sim;

    if (tok == NULL)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "CONFig:FEEDback:SOURce ADC|SIM|DAC|LOOP");
        return;
    }
    if (strcasecmp(tok, "ADC") == 0)        { sim = 0U; }
    else if (strcasecmp(tok, "SIM") == 0)   { sim = 1U; }
    else if (strcasecmp(tok, "DAC") == 0)   { sim = 2U; }
    else if (strcasecmp(tok, "LOOP") == 0)  { sim = 3U; }
    else
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "source must be ADC, SIM, DAC or LOOP");
        return;
    }
    ReplySet(inst, Control_SetFeedbackSim(sim), "bad source");
}

void cmd_config_fb_source_query(uart_instance_t *inst, char *args)
{
    (void)args;
    switch (Control_GetFeedbackSim())
    {
        case 1U:  uart_send(inst, "OK SIM\r\n"); break;
        case 2U:  uart_send(inst, "OK DAC\r\n"); break;
        case 3U:  uart_send(inst, "OK LOOP\r\n"); break;
        default:  uart_send(inst, "OK ADC\r\n"); break;
    }
}

/* ---- CONFig:CLOSEDloop:CURVe:* : the HVPS frequency -> volts lookup table (hvps_curve.h) ---- */

static hvps_curve_t g_curveStage;
static uint8_t      g_curveUploadActive = 0U;

static uint8_t RefuseCurveWhileRunning(uart_instance_t *inst)
{
    if (Control_IsRunning() != 0U)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "Not while a closed-loop run is in progress");
        return 1U;
    }
    return 0U;
}

void cmd_curve_begin(uart_instance_t *inst, char *args)
{
    (void)args;
    if (RefuseCurveWhileRunning(inst) != 0U)
    {
        return;
    }
    HvpsCurve_Clear(&g_curveStage);
    g_curveUploadActive = 1U;
    Scpi_ReplyOk(inst);
}

void cmd_curve_step(uart_instance_t *inst, char *args)
{
    char *tokF = NULL, *tokV = NULL, *extra = NULL;
    float hz = 0.0f, volts = 0.0f;

    if (g_curveUploadActive == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_NOT_UPLOADING, "Not currently uploading -- send CONFig:CLOSEDloop:CURVe:BEGin first");
        return;
    }
    if (args != NULL)
    {
        tokF = strtok(args, " \r\n");
        tokV = (tokF != NULL) ? strtok(NULL, " \r\n") : NULL;
        extra = (tokV != NULL) ? strtok(NULL, " \r\n") : NULL;
    }
    if ((tokV == NULL) || (extra != NULL) || (Scpi_ParseUnsigned(tokF, 1U, &hz) == 0U) || (Scpi_ParseUnsigned(tokV, 3U, &volts) == 0U))
    {
        Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "CURVe:STEP needs exactly two values: <Hz> <volts>, both non-negative (power is never negative)");
        return;
    }
    switch (HvpsCurve_Append(&g_curveStage, hz, volts))
    {
        case HVPS_CURVE_OK:
            Scpi_ReplyOk(inst);
            break;
        case HVPS_CURVE_FULL:
            Scpi_SendErr(inst, ERR_TABLE_FULL, "Curve full (HVPS_CURVE_MAX_POINTS points)");
            break;
        default:
            Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "Frequency must be above the previous point's");
            break;
    }
}

void cmd_curve_end(uart_instance_t *inst, char *args)
{
    char pk[24], pv[24], buf[80];
    (void)args;

    if (g_curveUploadActive == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_NOT_UPLOADING, "Not currently uploading -- send CONFig:CLOSEDloop:CURVe:BEGin first");
        return;
    }
    if (RefuseCurveWhileRunning(inst) != 0U)
    {
        return;
    }
    if (HvpsCurve_IsValid(&g_curveStage) == 0U)
    {
        Scpi_SendErr(inst, ERR_TABLE_STEP_INVALID, "A curve needs at least two points and a single peak (rising then falling)");
        return;
    }
    g_curveUploadActive = 0U;
    Control_Loop()->cfg.curve = g_curveStage;
    Control_Loop()->cfg.curveCustom = 1U;
    Scpi_FormatFixed(pk, sizeof(pk), HvpsCurve_PeakHz(&g_curveStage), 1U);
    Scpi_FormatFixed(pv, sizeof(pv), HvpsCurve_PeakVolts(&g_curveStage), 3U);
    snprintf(buf, sizeof(buf), "OK %u %s %s\r\n", (unsigned)g_curveStage.n, pk, pv);
    uart_send(inst, buf);
}

void cmd_curve_clear(uart_instance_t *inst, char *args)
{
    (void)args;
    if (RefuseCurveWhileRunning(inst) != 0U)
    {
        return;
    }
    g_curveUploadActive = 0U;
    Control_Loop()->cfg.curveCustom = 0U;       /* back to the linear default */
    Scpi_ReplyOk(inst);
}

void cmd_curve_query(uart_instance_t *inst, char *args)
{
    char pk[24], pv[24], buf[80];
    (void)args;

    if (Control_Loop()->cfg.curveCustom == 0U)
    {
        uart_send(inst, "OK LINEAR\r\n");
        return;
    }
    Scpi_FormatFixed(pk, sizeof(pk), HvpsCurve_PeakHz(&Control_Loop()->cfg.curve), 1U);
    Scpi_FormatFixed(pv, sizeof(pv), HvpsCurve_PeakVolts(&Control_Loop()->cfg.curve), 3U);
    snprintf(buf, sizeof(buf), "OK TABLE %u %s %s\r\n", (unsigned)Control_Loop()->cfg.curve.n, pk, pv);
    uart_send(inst, buf);
}

void cmd_curve_point_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint32_t i = 0U;
    char f[24], v[24], buf[64];
    const hvps_curve_t *cv = &Control_Loop()->cfg.curve;

    if ((tok == NULL) || (Scpi_ParseScaled(tok, 0U, &i) == 0U))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "CURVe:POINt? needs one argument: the point index (from 0)");
        return;
    }
    if ((Control_Loop()->cfg.curveCustom == 0U) || (i >= cv->n))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "No such point (no table loaded, or index out of range)");
        return;
    }
    Scpi_FormatFixed(f, sizeof(f), cv->hz[i], 1U);
    Scpi_FormatFixed(v, sizeof(v), cv->volts[i], 3U);
    snprintf(buf, sizeof(buf), "OK %s %s\r\n", f, v);
    uart_send(inst, buf);
}

/* ---- SIM:PLANT:* : the simulated supply's parameters ---- */

static ctl_set_result_t SimBusy(void) { return (Control_IsRunning() != 0U) ? CTL_SET_BUSY : CTL_SET_OK; }
#define SIM_ALL(field, value) do { for (uint8_t i_ = 0U; i_ < CTL_LOOPS; i_++) { Control_SimPlantN(i_)->field = (value); } } while (0)
static ctl_set_result_t SetSimTau(float v)   { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v < 0.0001f) || (v > 10.0f)) { return CTL_SET_RANGE; } SIM_ALL(tau, v); return CTL_SET_OK; }
static float            GetSimTau(void)      { return Control_SimPlant()->tau; }
static ctl_set_result_t SetSimGainFor(uint8_t loop, float v)   /* live: a disturbance; loop 0 (CTL_ALL_LOOPS) = every plant */
{
    if (v > 10.0f) { return CTL_SET_RANGE; }
    if (loop == CTL_ALL_LOOPS) { SIM_ALL(gain, v); } else { Control_SimPlantN(loop)->gain = v; }
    return CTL_SET_OK;
}
static float            GetSimGainFor(uint8_t loop) { return Control_SimPlantN(loop)->gain; }
static ctl_set_result_t SetSimRes(float v)   { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v < 1000.0f) || (v > 100000.0f)) { return CTL_SET_RANGE; } SIM_ALL(resonance, v); return CTL_SET_OK; }
static float            GetSimRes(void)      { return Control_SimPlant()->resonance; }
static ctl_set_result_t SetSimWidth(float v) { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v < 10.0f) || (v > 100000.0f)) { return CTL_SET_RANGE; } SIM_ALL(width, v); return CTL_SET_OK; }
static float            GetSimWidth(void)    { return Control_SimPlant()->width; }
static ctl_set_result_t SetSimVpeak(float v) { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v <= 0.0f) || (v > 1000.0f)) { return CTL_SET_RANGE; } SIM_ALL(vpeak, v); return CTL_SET_OK; }
static float            GetSimVpeak(void)    { return Control_SimPlant()->vpeak; }
static ctl_set_result_t SetSimVscale(float v) { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v <= 0.0f) || (v > 1000.0f)) { return CTL_SET_RANGE; } SIM_ALL(vscale, v); return CTL_SET_OK; }
static float            GetSimVscale(void)   { return Control_SimPlant()->vscale; }

void cmd_sim_plant_shape(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;

    if (tok == NULL)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "SIM:PLANT:SHAPE LINEAR|LORENTZ");
        return;
    }
    if (SimBusy() != CTL_SET_OK)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "Not while a closed-loop run is in progress");
        return;
    }
    if (strcasecmp(tok, "LINEAR") == 0)        { SIM_ALL(shape, SIM_SHAPE_LINEAR); }
    else if (strcasecmp(tok, "LORENTZ") == 0)  { SIM_ALL(shape, SIM_SHAPE_LORENTZ); }
    else
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "shape must be LINEAR or LORENTZ");
        return;
    }
    Scpi_ReplyOk(inst);
}

void cmd_sim_plant_shape_query(uart_instance_t *inst, char *args)
{
    (void)args;
    uart_send(inst, (Control_SimPlant()->shape == SIM_SHAPE_LORENTZ) ? "OK LORENTZ\r\n" : "OK LINEAR\r\n");
}

FLOAT_SETTING(cmd_sim_plant_tau,    SetSimTau,    GetSimTau,    4U, "SIM:PLANT:TAU <seconds>", "tau must be 0.0001-10 s")
LOOP_FLOAT_SETTING(cmd_sim_plant_gain, SetSimGainFor, GetSimGainFor, 3U, "SIM:PLANT:GAIN [<loop 0-3>] <factor>   (one factor = loop 1; loop 0 = every plant)", "gain must be 0-10")
FLOAT_SETTING(cmd_sim_plant_res,    SetSimRes,    GetSimRes,    0U, "SIM:PLANT:RESonance <Hz>", "resonance must be 1000-100000 Hz")
FLOAT_SETTING(cmd_sim_plant_width,  SetSimWidth,  GetSimWidth,  0U, "SIM:PLANT:WIDTH <Hz>", "width must be 10-100000 Hz")
FLOAT_SETTING(cmd_sim_plant_vpeak,  SetSimVpeak,  GetSimVpeak,  3U, "SIM:PLANT:VPEAK <volts>", "Vpeak must be > 0")
FLOAT_SETTING(cmd_sim_plant_vscale, SetSimVscale, GetSimVscale, 3U, "SIM:PLANT:VSCALE <volts>", "Vscale must be > 0")
