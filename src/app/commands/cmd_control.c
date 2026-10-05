/*
 * cmd_control.c -- serial command handlers for the closed loop, mirroring the XREX pattern
 * (docs/command_reference.md in WHAM-XREX-PFMG474) where HBRIDGE has the same thing:
 *
 *   PID:LOOPMODE <ch> <0|1> / ?      0 open loop (the SHOT:* profile), 1 closed loop (the TARGet:* profile)
 *   PID:GAINS <ch> <kp> <ki> <kd> / ?   gains of the SELECTED SUPPLY MODE's loop (Hz/V for HVPS, %/V for Limiter)
 *   SOURce:RUN / STOP                 closed-loop hold of SOURce:SETpoint, until stopped
 *   SOURce:SETpoint <ch> <volts> / ?  the hold target
 *   SOURce:STATus? <ch>               OK <running> <setpointV> <measuredV> <output Hz|%>
 *   TARGet:BEGin / STEP <t_us> <volts> / END / CLEar / ? / POINt? <i>   the target-voltage time series
 *   LOG:ARM <ch> <max> <decim> / LOG:DATA? <ch> [<start> [<count>]]
 *   CONFig:PIDRate, :SLEWRate, :MAXVolts, :FEEDback:*, :ADC:*, :CLOSEDloop:*, SIM:PLANT:*
 *
 * `<ch>` is the loop, 1 today (XREX's channel argument, kept so tooling written for XREX carries over
 * and a second loop later does not change the protocol); 0 where XREX allows it means "all loops".
 * FIRE plays the shot open- or closed-loop according to PID:LOOPMODE (cmd_table.c).
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "control_runtime.h"
#include "target_profile.h"
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

static uint8_t ParseUnsigned(const char *tok, uint8_t decimals, float *out)
{
    uint32_t v = 0U;
    float div = 1.0f;

    if (ShotProfile_ParseScaled(tok, decimals, &v) == 0U)
    {
        return 0U;
    }
    for (uint8_t i = 0U; i < decimals; i++) { div *= 10.0f; }
    *out = (float)v / div;
    return 1U;
}

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

/* v with `decimals` decimals into buf, signed: "-1.250" */
static void Fixed(char *buf, size_t n, float v, uint8_t decimals)
{
    uint32_t scale = 1U;
    int32_t scaled;
    uint32_t mag;
    char sign = ' ';

    for (uint8_t i = 0U; i < decimals; i++) { scale *= 10U; }
    scaled = (int32_t)(v * (float)scale + ((v < 0.0f) ? -0.5f : 0.5f));
    if (scaled < 0) { sign = '-'; mag = (uint32_t)(-scaled); } else { mag = (uint32_t)scaled; }
    if (decimals == 0U)
    {
        snprintf(buf, n, "%s%lu", (sign == '-') ? "-" : "", (unsigned long)mag);
    }
    else
    {
        snprintf(buf, n, "%s%lu.%0*lu", (sign == '-') ? "-" : "", (unsigned long)(mag / scale), (int)decimals,
                 (unsigned long)(mag % scale));
    }
}

static void ReplyOk(uart_instance_t *inst)
{
    uart_send(inst, "OK\r\n");
}

static void ReplyFixed(uart_instance_t *inst, float v, uint8_t decimals)
{
    char num[24];
    char buf[40];

    Fixed(num, sizeof(num), v, decimals);
    snprintf(buf, sizeof(buf), "OK %s\r\n", num);
    uart_send(inst, buf);
}

static void ReplySet(uart_instance_t *inst, ctl_set_result_t r, const char *rangeMsg)
{
    if (r == CTL_SET_OK)
    {
        ReplyOk(inst);
    }
    else if (r == CTL_SET_BUSY)
    {
        SendErr(inst, ERR_INVALID_STATE, "Not while a closed-loop run is in progress");
    }
    else
    {
        SendErr(inst, ERR_INVALID_ARGS, rangeMsg);
    }
}

/* The loop argument: 1 (the only loop), or 0 for "all" where allowZero. Sends ERR 11 and returns 0 otherwise. */
static uint8_t ParseLoop(uart_instance_t *inst, const char *tok, uint8_t allowZero)
{
    uint32_t v = 0U;

    if ((tok == NULL) || (ShotProfile_ParseScaled(tok, 0U, &v) == 0U) || (v > 1U) || ((v == 0U) && (allowZero == 0U)))
    {
        SendErr(inst, ERR_INVALID_CHANNEL, "Invalid loop (1 is the only loop; 0 = all where accepted)");
        return 0U;
    }
    return 1U;
}

static uint8_t RefuseWhileFiring(uart_instance_t *inst)
{
    if (SM_GetState() == SM_STATE_FIRING)
    {
        SendErr(inst, ERR_INVALID_STATE, "Can't change while FIRING");
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
            SendErr(inst, ERR_INVALID_STATE, "No supply mode selected -- send CONFig:MODE LIMiter|HVPS first");
            break;
        case CTL_NO_TARGET:
            SendErr(inst, ERR_TABLE_EMPTY, "No target profile -- upload one first (TARGet:BEGin/STEP/END)");
            break;
        case CTL_BAD_CONFIG:
            SendErr(inst, ERR_INVALID_ARGS, "Closed-loop configuration unusable (the HVPS resonance leaves no band inside the mode's range)");
            break;
        case CTL_NO_FEEDBACK:
            SendErr(inst, ERR_INVALID_STATE, "The ADC sampler is not running");
            break;
        case CTL_REF_BAD:
            SendErr(inst, ERR_INVALID_STATE, "VREF+ is not at ANALOG_VREF_MV (check ADC:VREF?)");
            break;
        case CTL_NO_METER:
            SendErr(inst, ERR_INVALID_STATE, "SOURce LOOP needs PFM_Input_01 free (no PFMIN capture running)");
            break;
        default:
            SendErr(inst, ERR_INVALID_STATE, "A closed-loop run is already in progress");
            break;
    }
    return 1U;
}

/* ---- PID:* ---- */

void cmd_pid_loopmode(uart_instance_t *inst, char *args)
{
    char *tokCh = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokV  = (tokCh != NULL) ? strtok(NULL, " \r\n") : NULL;

    if (ParseLoop(inst, tokCh, 1U) == 0U)
    {
        return;
    }
    if ((tokV == NULL) || ((strcmp(tokV, "0") != 0) && (strcmp(tokV, "1") != 0)))
    {
        SendErr(inst, ERR_INVALID_ARGS, "PID:LOOPMODE needs <ch> <0|1>");
        return;
    }
    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    Control_SetLoopMode((uint8_t)(tokV[0] - '0'));
    ReplyOk(inst);
}

void cmd_pid_loopmode_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;

    if (ParseLoop(inst, tok, 0U) == 0U)
    {
        return;
    }
    uart_send(inst, (Control_GetLoopMode() != 0U) ? "OK 1\r\n" : "OK 0\r\n");
}

void cmd_pid_gains(uart_instance_t *inst, char *args)
{
    char *t[4] = { NULL, NULL, NULL, NULL };
    float kp, ki, kd;

    if (args != NULL)
    {
        t[0] = strtok(args, " \r\n");
        for (int i = 1; (i < 4) && (t[i - 1] != NULL); i++) { t[i] = strtok(NULL, " \r\n"); }
    }
    if (ParseLoop(inst, t[0], 1U) == 0U)
    {
        return;
    }
    if ((t[3] == NULL) || (strtok(NULL, " \r\n") != NULL) || (ParseUnsigned(t[1], 3U, &kp) == 0U) ||
        (ParseUnsigned(t[2], 3U, &ki) == 0U) || (ParseUnsigned(t[3], 3U, &kd) == 0U))
    {
        SendErr(inst, ERR_INVALID_ARGS, "PID:GAINS needs <ch> <kp> <ki> <kd>, each non-negative with up to 3 decimals (max about 4 million)");
        return;
    }
    if (SupplyMode_Get() == SUPPLY_MODE_NONE)
    {
        SendErr(inst, ERR_INVALID_STATE, "No supply mode selected: the gains belong to the selected supply's loop");
        return;
    }
    ReplySet(inst, Control_SetGains(kp, ki, kd), "gains must be non-negative");
}

void cmd_pid_gains_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    float kp, ki, kd;
    char a[24], b[24], c[24], buf[96];

    if (ParseLoop(inst, tok, 0U) == 0U)
    {
        return;
    }
    Control_GetGains(&kp, &ki, &kd);
    Fixed(a, sizeof(a), kp, 3U);
    Fixed(b, sizeof(b), ki, 3U);
    Fixed(c, sizeof(c), kd, 3U);
    snprintf(buf, sizeof(buf), "OK %s %s %s\r\n", a, b, c);
    uart_send(inst, buf);
}

/* ---- SOURce:* ---- */

void cmd_source_run(uart_instance_t *inst, char *args)
{
    (void)args;

    if (AnyFaultLatched() != 0U)
    {
        SendErr(inst, ERR_FAULT_LATCHED, "Fault latched -- send FAULT:CLEAR first");
        return;
    }
    if (SM_GetState() != SM_STATE_ARMED)
    {
        SendErr(inst, ERR_INVALID_STATE, "Not ARMED -- send ARM first");
        return;
    }
    if (Control_GetLoopMode() == 0U)
    {
        SendErr(inst, ERR_INVALID_STATE, "PID:LOOPMODE is 0 (open loop): SOURce:RUN is a closed-loop hold");
        return;
    }
    if (Cmd_ReportCtlResult(inst, Control_Prepare(0U)) != 0U)
    {
        return;
    }
    if (SM_Fire() == 0U)
    {
        Control_Stop();
        SendErr(inst, ERR_INVALID_STATE, "SOURce:RUN refused -- state changed");
        return;
    }
    Control_Go();
    ReplyOk(inst);
}

void cmd_source_stop(uart_instance_t *inst, char *args)
{
    (void)args;
    Control_Stop();
    ReplyOk(inst);
}

void cmd_source_setpoint(uart_instance_t *inst, char *args)
{
    char *tokCh = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokV  = (tokCh != NULL) ? strtok(NULL, " \r\n") : NULL;
    float v;

    if (ParseLoop(inst, tokCh, 1U) == 0U)
    {
        return;
    }
    if ((tokV == NULL) || (ParseSignedVolts(tokV, &v) == 0U))
    {
        SendErr(inst, ERR_INVALID_ARGS, "SOURce:SETpoint needs <ch> <volts> (signed, up to 3 decimals)");
        return;
    }
    if ((v > Control_GetMaxVolts()) || (v < -Control_GetMaxVolts()))
    {
        SendErr(inst, ERR_INVALID_ARGS, "Setpoint is beyond CONFig:MAXVolts");
        return;
    }
    Control_SetHoldVolts(v);
    ReplyOk(inst);
}

void cmd_source_setpoint_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;

    if (ParseLoop(inst, tok, 0U) == 0U)
    {
        return;
    }
    ReplyFixed(inst, Control_GetHoldVolts(), 3U);
}

void cmd_source_status(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    float sp, pv, out;
    char a[24], b[24], c[24], buf[96];

    if (ParseLoop(inst, tok, 0U) == 0U)
    {
        return;
    }
    Control_GetStatus(&sp, &pv, &out);
    Fixed(a, sizeof(a), sp, 3U);
    Fixed(b, sizeof(b), pv, 3U);
    Fixed(c, sizeof(c), out, 2U);
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
    ReplyOk(inst);
}

void cmd_target_step(uart_instance_t *inst, char *args)
{
    char *tokT = NULL, *tokV = NULL, *extra = NULL;
    uint32_t tUs = 0U;
    int32_t mv = 0;
    int32_t limit = (int32_t)(Control_GetMaxVolts() * 1000.0f);

    if (g_targetUploadActive == 0U)
    {
        SendErr(inst, ERR_TABLE_NOT_UPLOADING, "Not currently uploading -- send TARGet:BEGin first");
        return;
    }
    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    if (args != NULL)
    {
        tokT = strtok(args, " \r\n");
        tokV = (tokT != NULL) ? strtok(NULL, " \r\n") : NULL;
        extra = (tokV != NULL) ? strtok(NULL, " \r\n") : NULL;
    }
    if ((tokV == NULL) || (extra != NULL))
    {
        SendErr(inst, ERR_TABLE_STEP_INVALID, "TARGet:STEP needs exactly two values: t_us volts");
        return;
    }
    if ((ShotProfile_ParseScaled(tokT, 0U, &tUs) == 0U) || (Target_ParseMilliVolts(tokV, &mv) == 0U))
    {
        SendErr(inst, ERR_TABLE_STEP_INVALID, "t_us must be a non-negative integer, volts a signed decimal (3 places)");
        return;
    }
    switch (Target_Append(tUs, mv, limit))
    {
        case TARGET_APPEND_OK:
            ReplyOk(inst);
            break;
        case TARGET_APPEND_FULL:
            SendErr(inst, ERR_TABLE_FULL, "Target profile full (TARGET_SETPOINT_MAX setpoints)");
            break;
        case TARGET_APPEND_FIRST_NOT_ZERO:
            SendErr(inst, ERR_TABLE_STEP_INVALID, "The first setpoint must be at t=0");
            break;
        case TARGET_APPEND_NOT_INCREASING:
            SendErr(inst, ERR_TABLE_STEP_INVALID, "Time must be greater than the previous setpoint's");
            break;
        case TARGET_APPEND_TOO_LONG:
            SendErr(inst, ERR_TABLE_STEP_INVALID, "Time is beyond the longest shot (SHOT_MAX_DURATION_US)");
            break;
        default:
            SendErr(inst, ERR_OUT_OF_MODE_LIMITS, "Target voltage is beyond CONFig:MAXVolts");
            break;
    }
}

void cmd_target_end(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;

    if (g_targetUploadActive == 0U)
    {
        SendErr(inst, ERR_TABLE_NOT_UPLOADING, "Not currently uploading -- send TARGet:BEGin first");
        return;
    }
    if (Target_IsComplete() == 0U)
    {
        SendErr(inst, ERR_TABLE_STEP_INVALID, "A target profile needs at least two setpoints: t=0 and the end of the shot");
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
    ReplyOk(inst);
}

void cmd_target_query(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u %lu\r\n", (unsigned)Target_Count(), (unsigned long)Target_DurationUs());
    uart_send(inst, buf);
}

void cmd_target_point_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint32_t i = 0U;
    target_setpoint_t sp;
    char v[24], buf[64];

    if ((tok == NULL) || (ShotProfile_ParseScaled(tok, 0U, &i) == 0U) || (i > 0xFFFFU))
    {
        SendErr(inst, ERR_INVALID_ARGS, "TARGet:POINt? needs one argument: the setpoint index (from 0)");
        return;
    }
    if (Target_Get((uint16_t)i, &sp) == 0U)
    {
        SendErr(inst, ERR_INVALID_ARGS, "No such setpoint");
        return;
    }
    Fixed(v, sizeof(v), (float)sp.milliVolts / 1000.0f, 3U);
    snprintf(buf, sizeof(buf), "OK %lu %s\r\n", (unsigned long)sp.tUs, v);
    uart_send(inst, buf);
}

/* ---- LOG:* ---- */

void cmd_log_arm(uart_instance_t *inst, char *args)
{
    char *t[3] = { NULL, NULL, NULL };
    uint32_t max = 0U, decim = 0U;

    if (args != NULL)
    {
        t[0] = strtok(args, " \r\n");
        t[1] = (t[0] != NULL) ? strtok(NULL, " \r\n") : NULL;
        t[2] = (t[1] != NULL) ? strtok(NULL, " \r\n") : NULL;
    }
    if (ParseLoop(inst, t[0], 1U) == 0U)
    {
        return;
    }
    if ((t[2] == NULL) || (ShotProfile_ParseScaled(t[1], 0U, &max) == 0U) || (ShotProfile_ParseScaled(t[2], 0U, &decim) == 0U) ||
        (max < 1U) || (max > 1000U) || (decim < 1U) || (decim > 10000U))
    {
        SendErr(inst, ERR_INVALID_ARGS, "LOG:ARM needs <ch> <maxSamples 1-1000> <decim 1-10000>");
        return;
    }
    Control_LogArm((uint16_t)max, (uint16_t)decim);
    ReplyOk(inst);
}

/* LOG:DATA? <ch> [<start> [<count>]] -- OK <count> <rateHz> SUM=<s> sp1 pv1 out1 sp2 ... (millivolts, millivolts,
   milli-Hz or milli-percent). A page of at most 40 samples per reply: the ethernet serial bridge loses characters
   in long replies, and SUM (the sum of the numbers mod 2^32) lets the host tell. */
void cmd_log_data(uart_instance_t *inst, char *args)
{
    char *t[3] = { NULL, NULL, NULL };
    uint32_t start = 0U, want = 40U;
    uint16_t count = Control_LogCount();
    static char buf[40U * 3U * 12U + 64U];
    uint32_t sum = 0U;
    uint16_t last;
    int len;

    if (args != NULL)
    {
        t[0] = strtok(args, " \r\n");
        t[1] = (t[0] != NULL) ? strtok(NULL, " \r\n") : NULL;
        t[2] = (t[1] != NULL) ? strtok(NULL, " \r\n") : NULL;
    }
    if (ParseLoop(inst, t[0], 0U) == 0U)
    {
        return;
    }
    if (((t[1] != NULL) && (ShotProfile_ParseScaled(t[1], 0U, &start) == 0U)) ||
        ((t[2] != NULL) && ((ShotProfile_ParseScaled(t[2], 0U, &want) == 0U) || (want < 1U))) || (start > count))
    {
        SendErr(inst, ERR_INVALID_ARGS, "LOG:DATA? <ch> [<start> [<count>]]");
        return;
    }
    if (want > 40U) { want = 40U; }
    last = (uint16_t)(((start + want) > count) ? count : (start + want));
    for (uint16_t i = (uint16_t)start; i < last; i++)
    {
        int32_t a, b, c;
        (void)Control_LogGet(i, &a, &b, &c);
        sum += (uint32_t)a + (uint32_t)b + (uint32_t)c;
    }
    len = snprintf(buf, sizeof(buf), "OK %u %lu SUM=%lu", (unsigned)count, (unsigned long)Control_LogRateHz(), (unsigned long)sum);
    for (uint16_t i = (uint16_t)start; i < last; i++)
    {
        int32_t a, b, c;
        (void)Control_LogGet(i, &a, &b, &c);
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " %ld %ld %ld", (long)a, (long)b, (long)c);
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");
    uart_send(inst, buf);
}

/* ---- settings: one generated pair (set, query) per float setting ---- */

#define FLOAT_SETTING(FN, SETTER, GETTER, DEC, USAGE, RANGEMSG) \
void FN(uart_instance_t *inst, char *args) \
{ \
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL; \
    float v; \
    if ((tok == NULL) || (strtok(NULL, " \r\n") != NULL) || (ParseUnsigned(tok, (DEC), &v) == 0U)) \
    { \
        SendErr(inst, ERR_INVALID_ARGS, USAGE); \
        return; \
    } \
    ReplySet(inst, SETTER(v), RANGEMSG); \
} \
void FN##_query(uart_instance_t *inst, char *args) \
{ \
    (void)args; \
    ReplyFixed(inst, GETTER(), (DEC)); \
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
FLOAT_SETTING(cmd_config_fb_scale,   Control_SetFeedbackScale, Control_GetFeedbackScale, 4U, "CONFig:FEEDback:SCALE <process volts per ADC volt>",
              "scale must be > 0")
FLOAT_SETTING(cmd_config_fb_offset,  Control_SetFeedbackOffset, Control_GetFeedbackOffset, 4U, "CONFig:FEEDback:OFFSet <ADC volts that read as 0>",
              "offset must be 0 to the ADC full scale")
FLOAT_SETTING(cmd_config_fb_range,   Control_SetFeedbackRange, Control_GetFeedbackRange, 3U, "CONFig:FEEDback:RANGE <Vmax in process volts>",
              "Vmax must be > 0 and <= 100000")
FLOAT_SETTING(cmd_config_fb_dacratio, Control_SetDacRatio, Control_GetDacRatio, 4U, "CONFig:FEEDback:DACRatio <ADC volts per DAC volt>",
              "DAC ratio must be 0.05 to 2")
FLOAT_SETTING(cmd_config_fb_filter,  Control_SetFeedbackFilterHz, Control_GetFeedbackFilterHz, 1U, "CONFig:FEEDback:FILTer <cutoff Hz, 0 = off>",
              "cutoff must be 0 or below half the tick rate")
FLOAT_SETTING(cmd_config_fb_clip,    Control_SetClipMs, Control_GetClipMs, 1U, "CONFig:FEEDback:CLIPTime <ms, 0 = off>",
              "clip time must be 0-1000 ms")

/* closed-loop plant settings, validated against the supply modes' own limits */
static ctl_set_result_t CheckBusy(void) { return (Control_IsRunning() != 0U) ? CTL_SET_BUSY : CTL_SET_OK; }

static ctl_set_result_t SetClDuty(float v)
{
    supply_limits_t lim;
    ctl_set_result_t b = CheckBusy();
    (void)SupplyMode_GetLimits(SUPPLY_MODE_HVPS, &lim);
    if (b != CTL_SET_OK) { return b; }
    if ((v < (float)lim.dutyMinPct) || (v > (float)lim.dutyMaxPct)) { return CTL_SET_RANGE; }
    Control_Loop()->cfg.hvpsDutyPct = v;
    return CTL_SET_OK;
}
static float GetClDuty(void) { return Control_Loop()->cfg.hvpsDutyPct; }

static ctl_set_result_t SetClFreq(float v)
{
    supply_limits_t lim;
    ctl_set_result_t b = CheckBusy();
    (void)SupplyMode_GetLimits(SUPPLY_MODE_LIMITER, &lim);
    if (b != CTL_SET_OK) { return b; }
    if ((v < (float)lim.freqMinHz) || (v > (float)lim.freqMaxHz)) { return CTL_SET_RANGE; }
    Control_Loop()->cfg.limiterFreqHz = v;
    return CTL_SET_OK;
}
static float GetClFreq(void) { return Control_Loop()->cfg.limiterFreqHz; }

static ctl_set_result_t SetClRes(float v)
{
    supply_limits_t lim;
    ctl_set_result_t b = CheckBusy();
    (void)SupplyMode_GetLimits(SUPPLY_MODE_HVPS, &lim);
    if (b != CTL_SET_OK) { return b; }
    if ((v < (float)lim.freqMinHz) || (v > (float)lim.freqMaxHz)) { return CTL_SET_RANGE; }
    Control_Loop()->cfg.hvpsResonanceHz = v;
    return CTL_SET_OK;
}
static float GetClRes(void) { return Control_Loop()->cfg.hvpsResonanceHz; }

static ctl_set_result_t SetClVpeak(float v)
{
    if (CheckBusy() != CTL_SET_OK) { return CTL_SET_BUSY; }
    if (v <= 0.0f) { return CTL_SET_RANGE; }
    Control_Loop()->cfg.hvpsVpeak = v;
    return CTL_SET_OK;
}
static float GetClVpeak(void) { return Control_Loop()->cfg.hvpsVpeak; }

static ctl_set_result_t SetClVscale(float v)
{
    if (CheckBusy() != CTL_SET_OK) { return CTL_SET_BUSY; }
    if (v <= 0.0f) { return CTL_SET_RANGE; }
    Control_Loop()->cfg.limiterVscale = v;
    return CTL_SET_OK;
}
static float GetClVscale(void) { return Control_Loop()->cfg.limiterVscale; }

FLOAT_SETTING(cmd_config_cl_duty,  SetClDuty,  GetClDuty,  2U, "CONFig:CLOSEDloop:DUTY <percent>",
              "HVPS duty must be inside the HVPS mode's duty range")
FLOAT_SETTING(cmd_config_cl_freq,  SetClFreq,  GetClFreq,  0U, "CONFig:CLOSEDloop:FREQuency <Hz>",
              "Limiter carrier must be inside the Limiter mode's frequency range")
FLOAT_SETTING(cmd_config_cl_res,   SetClRes,   GetClRes,   0U, "CONFig:CLOSEDloop:RESonance <Hz>",
              "resonance must be inside the HVPS mode's frequency range")
FLOAT_SETTING(cmd_config_cl_vpeak, SetClVpeak, GetClVpeak, 3U, "CONFig:CLOSEDloop:VPEAK <volts at resonance>",
              "Vpeak must be > 0")
FLOAT_SETTING(cmd_config_cl_vscale, SetClVscale, GetClVscale, 3U, "CONFig:CLOSEDloop:VSCALE <volts at |2D-1| = 1>",
              "Vscale must be > 0")

void cmd_config_cl_branch(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;

    if (tok == NULL)
    {
        SendErr(inst, ERR_INVALID_ARGS, "CONFig:CLOSEDloop:BRANCH ABOVE|BELOW");
        return;
    }
    if (Control_IsRunning() != 0U)
    {
        SendErr(inst, ERR_INVALID_STATE, "Not while a closed-loop run is in progress");
        return;
    }
    if (strcasecmp(tok, "ABOVE") == 0)       { Control_Loop()->cfg.hvpsAbove = 1U; }
    else if (strcasecmp(tok, "BELOW") == 0)  { Control_Loop()->cfg.hvpsAbove = 0U; }
    else
    {
        SendErr(inst, ERR_INVALID_ARGS, "branch must be ABOVE or BELOW resonance");
        return;
    }
    ReplyOk(inst);
}

void cmd_config_cl_branch_query(uart_instance_t *inst, char *args)
{
    (void)args;
    uart_send(inst, (Control_Loop()->cfg.hvpsAbove != 0U) ? "OK ABOVE\r\n" : "OK BELOW\r\n");
}

/* CONFig:FEEDback:CHANnel <1-6 | PA0 PA1 PC0 PB0 PC1 PB1> */
static const char *const kPinNames[ADC_NUM_CHANNELS] = { "PA0", "PA1", "PC0", "PB0", "PC1", "PB1" };

void cmd_config_fb_channel(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint32_t n = 0U;
    int idx = -1;

    if (tok == NULL)
    {
        SendErr(inst, ERR_INVALID_ARGS, "CONFig:FEEDback:CHANnel <1-6 or PA0 PA1 PC0 PB0 PC1 PB1>");
        return;
    }
    if ((ShotProfile_ParseScaled(tok, 0U, &n) != 0U) && (n >= 1U) && (n <= ADC_NUM_CHANNELS))
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
        SendErr(inst, ERR_INVALID_ARGS, "channel must be 1-6 or one of PA0 PA1 PC0 PB0 PC1 PB1");
        return;
    }
    ReplySet(inst, Control_SetFeedbackChannel((uint8_t)idx), "bad channel");
}

void cmd_config_fb_channel_query(uart_instance_t *inst, char *args)
{
    char buf[32];
    uint8_t c = Control_GetFeedbackChannel();
    (void)args;

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
        SendErr(inst, ERR_INVALID_ARGS, "CONFig:FEEDback:SOURce ADC|SIM|DAC|LOOP");
        return;
    }
    if (strcasecmp(tok, "ADC") == 0)        { sim = 0U; }
    else if (strcasecmp(tok, "SIM") == 0)   { sim = 1U; }
    else if (strcasecmp(tok, "DAC") == 0)   { sim = 2U; }
    else if (strcasecmp(tok, "LOOP") == 0)  { sim = 3U; }
    else
    {
        SendErr(inst, ERR_INVALID_ARGS, "source must be ADC, SIM, DAC or LOOP");
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

/* ---- SIM:PLANT:* : the simulated supply's parameters ---- */

static ctl_set_result_t SimBusy(void) { return (Control_IsRunning() != 0U) ? CTL_SET_BUSY : CTL_SET_OK; }
static ctl_set_result_t SetSimTau(float v)   { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v < 0.0001f) || (v > 10.0f)) { return CTL_SET_RANGE; } Control_SimPlant()->tau = v; return CTL_SET_OK; }
static float            GetSimTau(void)      { return Control_SimPlant()->tau; }
static ctl_set_result_t SetSimGain(float v)  { if (v > 10.0f) { return CTL_SET_RANGE; } Control_SimPlant()->gain = v; return CTL_SET_OK; }   /* live: a disturbance */
static float            GetSimGain(void)     { return Control_SimPlant()->gain; }
static ctl_set_result_t SetSimRes(float v)   { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v < 1000.0f) || (v > 100000.0f)) { return CTL_SET_RANGE; } Control_SimPlant()->resonance = v; return CTL_SET_OK; }
static float            GetSimRes(void)      { return Control_SimPlant()->resonance; }
static ctl_set_result_t SetSimWidth(float v) { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v < 10.0f) || (v > 100000.0f)) { return CTL_SET_RANGE; } Control_SimPlant()->width = v; return CTL_SET_OK; }
static float            GetSimWidth(void)    { return Control_SimPlant()->width; }
static ctl_set_result_t SetSimVpeak(float v) { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v <= 0.0f) || (v > 1000.0f)) { return CTL_SET_RANGE; } Control_SimPlant()->vpeak = v; return CTL_SET_OK; }
static float            GetSimVpeak(void)    { return Control_SimPlant()->vpeak; }
static ctl_set_result_t SetSimVscale(float v) { if (SimBusy() != CTL_SET_OK) { return CTL_SET_BUSY; } if ((v <= 0.0f) || (v > 1000.0f)) { return CTL_SET_RANGE; } Control_SimPlant()->vscale = v; return CTL_SET_OK; }
static float            GetSimVscale(void)   { return Control_SimPlant()->vscale; }

FLOAT_SETTING(cmd_sim_plant_tau,    SetSimTau,    GetSimTau,    4U, "SIM:PLANT:TAU <seconds>", "tau must be 0.0001-10 s")
FLOAT_SETTING(cmd_sim_plant_gain,   SetSimGain,   GetSimGain,   3U, "SIM:PLANT:GAIN <factor>", "gain must be 0-10")
FLOAT_SETTING(cmd_sim_plant_res,    SetSimRes,    GetSimRes,    0U, "SIM:PLANT:RESonance <Hz>", "resonance must be 1000-100000 Hz")
FLOAT_SETTING(cmd_sim_plant_width,  SetSimWidth,  GetSimWidth,  0U, "SIM:PLANT:WIDTH <Hz>", "width must be 10-100000 Hz")
FLOAT_SETTING(cmd_sim_plant_vpeak,  SetSimVpeak,  GetSimVpeak,  3U, "SIM:PLANT:VPEAK <volts>", "Vpeak must be > 0")
FLOAT_SETTING(cmd_sim_plant_vscale, SetSimVscale, GetSimVscale, 3U, "SIM:PLANT:VSCALE <volts>", "Vscale must be > 0")
