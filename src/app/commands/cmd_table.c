/*
 * cmd_table.c -- serial command handlers: legacy TABle:*, FIRE, PFM:DIAG?/GAPLOG?.
 * Split out of the old single commands.c (2026-09-29); the handlers are
 * unchanged. See commands.h for the file map.
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "pfm.h"
#include "hrtim.h"
#include "state_machine.h"
#include "supply_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * PFM table upload
 *
 * TABle:BEGin -> zero or more TABle:STEP <per> <cmp0> ... <cmp(N-1)>
 * (N = HRTIM_NUM_CHANNELS, ctrlr_config.h) -> TABle:END. Construction
 * (what the actual per/cmp values for a given shot profile should be)
 * happens entirely off-controller, in python/pfm_table_upload.py --
 * this layer just accepts whatever PFM_Step_t values it's given and
 * writes them in, one at a time. See pfm.h's "ADDED" header note for
 * why that split was a deliberate project decision, not a placeholder
 * for a builder that's coming later.
 *
 * g_tableUploadActive is deliberately local to this file, not pfm.c --
 * "is an upload session open" is protocol-layer state, not table
 * data, matching how the rest of this codebase keeps that kind of
 * bookkeeping in commands.c (see cmd_boot()'s own comments for the
 * same principle applied elsewhere).
 * -------------------------------------------------------------------------- */
static uint8_t g_tableUploadActive = 0U;

/* The table is read by the playback interrupt while FIRING, so it cannot be
   replaced mid-shot (ERR 13); upload between shots. */
static uint8_t RefuseWhileFiring(uart_instance_t *inst)
{
    if (SM_GetState() == SM_STATE_FIRING)
    {
        SendErr(inst, ERR_INVALID_STATE, "Table can't change while FIRING");
        return 1U;
    }
    return 0U;
}

/* Discards the uploaded table and any upload in progress. Called when the
   supply mode changes (cmd_config.c): a table checked against one mode's
   limits must not survive into another. */
void CmdTable_Discard(void)
{
    PFM_TableReset();
    g_tableUploadActive = 0U;
}

void cmd_table_begin(uart_instance_t *inst, char *args)
{
    (void)args;

    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }
    if (SupplyMode_Get() == SUPPLY_MODE_NONE)
    {
        SendErr(inst, ERR_INVALID_STATE, "No supply mode selected -- send CONFig:MODE LIMiter|HVPS first");
        return;
    }
    PFM_TableReset();
    g_tableUploadActive = 1U;

    uart_send(inst, "OK\r\n");
}

/* per + one compare value per channel -- see ctrlr_config.h's
   HRTIM_NUM_CHANNELS. */
#define TABLE_STEP_TOKEN_COUNT  (1U + HRTIM_NUM_CHANNELS)

/* Builds the "wrong token count" error text at runtime -- the count
   itself (1 + HRTIM_NUM_CHANNELS) is only known as a preprocessor
   expression, not a plain literal, so the standard #x/two-level
   stringification trick doesn't work here: it would stringify the
   unevaluated text "(1U + (4U))" rather than the computed value "5".
   snprintf() with %u sidesteps that entirely. */
static void SendTableStepCountErr(uart_instance_t *inst)
{
    char msg[64];
    snprintf(msg, sizeof(msg),
             "TABLE:STEP needs exactly %u values: per cmp0 .. cmp(N-1)",
             (unsigned int)TABLE_STEP_TOKEN_COUNT);
    SendErr(inst, ERR_TABLE_STEP_INVALID, msg);
}

void cmd_table_step(uart_instance_t *inst, char *args)
{
    char *tok;
    long  vals[TABLE_STEP_TOKEN_COUNT];
    int   n = 0;
    uint16_t cmp[HRTIM_NUM_CHANNELS];

    if (g_tableUploadActive == 0U)
    {
        SendErr(inst, ERR_TABLE_NOT_UPLOADING, "Not currently uploading -- send TABLE:BEGIN first");
        return;
    }
    if (RefuseWhileFiring(inst) != 0U)
    {
        return;
    }

    if (args == NULL)
    {
        SendTableStepCountErr(inst);
        return;
    }

    for (tok = strtok(args, " \r\n"); tok != NULL; tok = strtok(NULL, " \r\n"))
    {
        long v;

        if (n >= (int)TABLE_STEP_TOKEN_COUNT)
        {
            /* One extra token showed up -- too many values, not a
               valid step. */
            n++;
            break;
        }

        v = atol(tok);
        if (v < 0L || v > 65535L)
        {
            SendErr(inst, ERR_TABLE_STEP_INVALID, "Value out of uint16 range (0-65535)");
            return;
        }

        vals[n] = v;
        n++;
    }

    if (n != (int)TABLE_STEP_TOKEN_COUNT)
    {
        SendTableStepCountErr(inst);
        return;
    }

    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        cmp[ch] = (uint16_t)vals[1 + ch];
    }

    /* The selected supply mode's carrier and duty limits (supply_mode.h). */
    {
        supply_limits_t lim;
        char msg[96];
        supply_step_result_t res = SupplyMode_CheckStep((uint16_t)vals[0], cmp, HRTIM_NUM_CHANNELS);

        if (res != SUPPLY_STEP_OK)
        {
            if (SupplyMode_GetLimits(SupplyMode_Get(), &lim) == 0U)
            {
                SendErr(inst, ERR_INVALID_STATE, "No supply mode selected -- send CONFig:MODE LIMiter|HVPS first");
                return;
            }
            if ((res == SUPPLY_STEP_FREQ_LOW) || (res == SUPPLY_STEP_FREQ_HIGH))
            {
                snprintf(msg, sizeof(msg), "per %lu is %s the %s range (%lu-%lu Hz)",
                         (unsigned long)vals[0], (res == SUPPLY_STEP_FREQ_LOW) ? "below" : "above",
                         SupplyMode_Name(SupplyMode_Get()),
                         (unsigned long)lim.freqMinHz, (unsigned long)lim.freqMaxHz);
            }
            else
            {
                snprintf(msg, sizeof(msg), "a channel's duty is %s the %s range (%u-%u%%)",
                         (res == SUPPLY_STEP_DUTY_LOW) ? "below" : "above",
                         SupplyMode_Name(SupplyMode_Get()),
                         (unsigned)lim.dutyMinPct, (unsigned)lim.dutyMaxPct);
            }
            SendErr(inst, ERR_OUT_OF_MODE_LIMITS, msg);
            return;
        }
    }

    if (PFM_AppendStep((uint16_t)vals[0], cmp) == 0U)
    {
        SendErr(inst, ERR_TABLE_FULL, "Table full");
        return;
    }

    uart_send(inst, "OK\r\n");
}

void cmd_table_end(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;

    g_tableUploadActive = 0U;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned int)PFM_GetEntryCount());
    uart_send(inst, buf);
}

void cmd_table_query(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned int)PFM_GetEntryCount());
    uart_send(inst, buf);
}

/* --------------------------------------------------------------------------
 * FIRE
 *
 * Starts table playback from step 0 through the state machine
 * (state_machine.h): the controller must be ARMED (ARM first), and each
 * shot needs a fresh ARM -- the state returns to IDLE when the table ends.
 * Gated 2026-09-29 (Phase 3); before that FIRE took effect immediately in
 * any state.
 *
 * Checked in this order, each with its own error: a latched fault (ERR 6
 * -- PC10/HRTIM1_FLT6 or GateDriverStatus; the outputs are already safe in
 * hardware, but firing into a still-tripped fault would be a confusing
 * "OK" that produces no output), not ARMED (ERR 13), and an empty table
 * (ERR 5 -- PFM_CycleBoundaryHandler() would otherwise stop the outputs
 * again at the first interrupt, silently, after a near-instant blip).
 * SM_Fire() re-checks all three as a last line of defense.
 * -------------------------------------------------------------------------- */

void cmd_fire(uart_instance_t *inst, char *args)
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

    if (PFM_GetEntryCount() == 0U)
    {
        SendErr(inst, ERR_TABLE_EMPTY, "Table is empty -- upload one first (TABLE:BEGIN/STEP/END)");
        return;
    }

    if (SM_Fire() == 0U)
    {
        SendErr(inst, ERR_INVALID_STATE, "FIRE refused -- state changed");   /* narrow race only */
        return;
    }

    uart_send(inst, "OK\r\n");
}

/* --------------------------------------------------------------------------
 * PFM:DIAG?
 *
 * Diagnostic, added 2026-09-09 to test the "HRTIM1_Master's own
 * interrupt is being starved by higher-priority TIM2-5 capture ISRs"
 * theory for a real-hardware table-advance stall found this session
 * (a real PFM output holding one table entry for far longer than the
 * table's own dwell length says it should -- see docs/changelog.txt
 * and pfm.c's own comment on g_diagMaxGapCycles for the full
 * writeup). Reports PFM_GetDiagCounters()'s two numbers for the
 * CURRENT/most recent shot (reset at every FIRE/PFM_Restart()):
 * how many times PFM_CycleBoundaryHandler() has run, and the largest
 * real-time gap seen between two consecutive calls, in both raw CPU
 * cycles and microseconds (170 MHz CPU clock, confirmed against
 * main.c's SystemClock_Config(), not assumed). A max gap far larger
 * than one real period's worth of time is direct evidence of ISR
 * starvation, not just "the table happened to hold one entry a
 * while." Not gated on PFM_INPUT_FEATURE_ENABLED -- this is about the
 * PFM output engine itself, unrelated to whether input capture exists.
 * -------------------------------------------------------------------------- */
void cmd_pfm_diag(uart_instance_t *inst, char *args)
{
    char buf[80];
    uint32_t callCount;
    uint32_t maxGapCycles;
    (void)args;

    PFM_GetDiagCounters(&callCount, &maxGapCycles);

    /* 170 MHz CPU clock -- see this function's own doc comment. */
    uint32_t maxGapUs = maxGapCycles / 170U;

    snprintf(buf, sizeof(buf), "OK %lu %lu %lu\r\n",
             (unsigned long)callCount, (unsigned long)maxGapCycles,
             (unsigned long)maxGapUs);
    uart_send(inst, buf);
}

/* TEMPORARY debug command, 2026-09-09 (second round) -- see pfm.h's
   own comment on PFM_GetDiagGapLog(). Sized like PFMIN:DATA?'s own
   g_pfminDataBuf (commands.c) -- up to PFM_INPUT_MAX_PERIODS entries,
   each up to " 4294967295" (11 chars), plus the "OK <count>" prefix
   and CRLF. Remove once the RAMP/HOLD/RAMP real-hardware lag
   investigation (docs/changelog.txt) is resolved and this diagnostic
   is no longer needed. */
static char g_pfmGapLogBuf[2600];

void cmd_pfm_gaplog(uart_instance_t *inst, char *args)
{
    uint16_t count;
    const uint32_t *log = PFM_GetDiagGapLog(&count);
    int len = 0;
    (void)args;

    len += snprintf(&g_pfmGapLogBuf[len], sizeof(g_pfmGapLogBuf) - (size_t)len,
                     "OK %u", (unsigned int)count);
    for (uint16_t i = 0U; i < count; i++)
    {
        len += snprintf(&g_pfmGapLogBuf[len], sizeof(g_pfmGapLogBuf) - (size_t)len,
                         " %lu", (unsigned long)log[i]);
    }
    snprintf(&g_pfmGapLogBuf[len], sizeof(g_pfmGapLogBuf) - (size_t)len, "\r\n");

    uart_send(inst, g_pfmGapLogBuf);
}
