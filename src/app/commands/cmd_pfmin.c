/*
 * cmd_pfmin.c -- serial command handlers: PFMIN:*.
 * Split out of the old single commands.c (2026-09-29); the handlers are
 * unchanged. See commands.h for the file map.
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "pfm_input.h"
#include "state_machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if (PFM_INPUT_FEATURE_ENABLED != 0)
/* --------------------------------------------------------------------------
 * PFMIN:CAPTURE, PFMIN:STATus?, PFMIN:DATA?
 *
 * See pfm_input.h and commands.h's own header comment above these
 * declarations for the shot-synchronized design -- PFMIN:CAPTURE only
 * arms (PfmInput_Arm()); the actual capture starts inside
 * PFM_Restart() (pfm.c), at the next FIRE, not here.
 * -------------------------------------------------------------------------- */
void cmd_pfmin_capture(uart_instance_t *inst, char *args)
{
    char *tok;
    long  m;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (tok == NULL)
    {
        SendErr(inst, ERR_PFMIN_BAD_COUNT, "PFMIN:CAPTURE needs one argument: M (1-PFM_INPUT_MAX_PERIODS)");
        return;
    }

    m = atol(tok);
    if ((m < 1L) || (m > (long)PFM_INPUT_MAX_PERIODS))
    {
        SendErr(inst, ERR_PFMIN_BAD_COUNT, "M out of range (1-PFM_INPUT_MAX_PERIODS)");
        return;
    }

    PfmInput_Arm((uint16_t)m);

    uart_send(inst, "OK\r\n");
}

void cmd_pfmin_status(uart_instance_t *inst, char *args)
{
    char buf[64];
    int  len = 0;
    (void)args;

    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, "OK");
    for (uint8_t ch = 0U; ch < PFM_INPUT_NUM_CHANNELS; ch++)
    {
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len,
                         " %u", (unsigned int)PfmInput_GetCount(ch));
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");

    uart_send(inst, buf);
}

/* TEMPORARY debug command, 2026-09-09 -- see pfm_input.h's own comment
   on PfmInput_GetDmaStartStatus(). Remove once DMA capture is
   confirmed reliable. */
void cmd_pfmin_dmastat(uart_instance_t *inst, char *args)
{
    char buf[64];
    int  len = 0;
    (void)args;

    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, "OK");
    for (uint8_t ch = 0U; ch < PFM_INPUT_NUM_CHANNELS; ch++)
    {
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len,
                         " %u", (unsigned int)PfmInput_GetDmaStartStatus(ch));
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");

    uart_send(inst, buf);
}

/* Sized for the worst case: PFM_INPUT_MAX_PERIODS entries, each up to
   " 4294967295" (11 chars), plus the "OK <count> OVERCAP=<n>" prefix
   and CRLF -- comfortably over 200*11+32 with room to spare. static,
   not a stack local: this project's linker script reserves only
   _Min_Stack_Size (1 KB) for the stack (STM32G474QETX_FLASH.ld) --
   putting several KB on the stack here would be a real, needless risk
   even though the actual runtime stack has far more room in practice
   (see AGENTS.md's own flagged-but-deferred note on stack sizing). */
static char g_pfminDataBuf[4700];   /* raw mode: up to 402 edges x 11 chars */

void cmd_pfmin_data(uart_instance_t *inst, char *args)
{
    char    *tok;
    long     chArg;
    uint8_t  ch;
    uint16_t count;
    const uint32_t *periods;
    int      len = 0;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (tok == NULL)
    {
        SendErr(inst, ERR_PFMIN_BAD_CHANNEL, "PFMIN:DATA? needs one argument: channel (1-6)");
        return;
    }

    chArg = atol(tok);
    if ((chArg < 1L) || (chArg > (long)PFM_INPUT_NUM_CHANNELS))
    {
        SendErr(inst, ERR_PFMIN_BAD_CHANNEL, "Invalid PFM_Input channel (1-6)");
        return;
    }
    ch = (uint8_t)(chArg - 1L);   /* 1-6 on the wire -> 0-5 internally */

    if (PfmInput_GetEdgeMode(ch) != 0U)
    {
        SendErr(inst, ERR_INVALID_STATE, "channel is in BOTH-edge mode -- use PFMIN:RAW?");
        return;
    }

    count   = PfmInput_GetCount(ch);
    periods = PfmInput_GetPeriods(ch);

    /* OVERCAP=<n>: a genuine data-quality indicator (see
       PfmInput_GetOvercaptureCount()'s own doc comment in
       pfm_input.c) -- nonzero means a second rising edge arrived
       before this module's ISR could read the previous one, so some
       entries below may not be trustworthy. Expected to read 0 in
       normal operation; not yet documented in
       docs/command_reference.md. */
    len += snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len,
                     "OK %u OVERCAP=%u", (unsigned int)count,
                     (unsigned int)PfmInput_GetOvercaptureCount(ch));

    for (uint16_t i = 0U; i < count; i++)
    {
        len += snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len,
                         " %lu", (unsigned long)periods[i]);
    }
    snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len, "\r\n");

    uart_send(inst, g_pfminDataBuf);
}
#endif /* PFM_INPUT_FEATURE_ENABLED */

/* --------------------------------------------------------------------------
 * PFMIN:EDGes <ch> RISING|BOTH  /  PFMIN:EDGes?  /  PFMIN:RAW? <ch> [<start> [<count>]]
 *
 * Raw edge mode (pfm_input.h): a channel set to BOTH captures every edge and
 * keeps the raw timer values, for measuring duty (time high) and for looking
 * at what the real edge stream is. The mode is applied at the next
 * PFMIN:CAPTURE, and can only be changed in IDLE.
 *   PFMIN:EDGes 1 BOTH     -> OK
 *   PFMIN:EDGes?           -> OK R B R R R R      (R rising only, B both)
 *   PFMIN:CAPTURE 50 ... FIRE ...
 *   PFMIN:RAW? 1           -> OK <n> OVERCAP=<k> LEVEL=<0|1> SUM=<s> <t0> <d1> <d2> ...
 * <n> edges; t0 is the first edge's raw timer value and each following number
 * is the difference from the previous edge (so the reply stays short); SUM is the
 * sum of those numbers mod 2^32, a transport check; LEVEL
 * is the pin level just before capture started, the first edge is the
 * opposite transition. PFMIN:DATA? refuses a BOTH channel (ERR 13).
 * <n> is always the total; <start> and <count> select a page of the values (SUM is
 * over the page). Without them the whole stream is returned.
 * -------------------------------------------------------------------------- */
void cmd_pfmin_edges(uart_instance_t *inst, char *args)
{
    char *tokCh   = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokMode = (tokCh != NULL) ? strtok(NULL, " \r\n") : NULL;
    long chArg;
    uint8_t both;

    if ((tokCh == NULL) || (tokMode == NULL))
    {
        SendErr(inst, ERR_INVALID_ARGS, "PFMIN:EDGes needs two arguments: channel (1-6) RISING|BOTH");
        return;
    }
    chArg = atol(tokCh);
    if ((chArg < 1L) || (chArg > (long)PFM_INPUT_NUM_CHANNELS))
    {
        SendErr(inst, ERR_PFMIN_BAD_CHANNEL, "Invalid PFM_Input channel (1-6)");
        return;
    }
    if (strcasecmp(tokMode, "BOTH") == 0)
    {
        both = 1U;
    }
    else if ((strcasecmp(tokMode, "RISING") == 0) || (strcasecmp(tokMode, "RIS") == 0))
    {
        both = 0U;
    }
    else
    {
        SendErr(inst, ERR_INVALID_ARGS, "mode must be RISING or BOTH");
        return;
    }
    if (SM_GetState() != SM_STATE_IDLE)
    {
        SendErr(inst, ERR_INVALID_STATE, "PFMIN:EDGes only in IDLE");
        return;
    }
    (void)PfmInput_SetEdgeMode((uint8_t)(chArg - 1L), both);
    uart_send(inst, "OK\r\n");
}

void cmd_pfmin_edges_query(uart_instance_t *inst, char *args)
{
    char buf[48];
    int  len = 0;
    (void)args;

    len += snprintf(buf, sizeof(buf), "OK");
    for (uint8_t ch = 0U; ch < PFM_INPUT_NUM_CHANNELS; ch++)
    {
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " %c", (PfmInput_GetEdgeMode(ch) != 0U) ? 'B' : 'R');
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");
    uart_send(inst, buf);
}

void cmd_pfmin_raw(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokStart = (tok != NULL) ? strtok(NULL, " \r\n") : NULL;
    char *tokCount = (tokStart != NULL) ? strtok(NULL, " \r\n") : NULL;
    long chArg;
    uint16_t first = 0U;
    uint16_t last;
    uint8_t ch;
    uint16_t count;
    const uint32_t *edges;
    uint32_t mask;
    int len = 0;

    if (tok == NULL)
    {
        SendErr(inst, ERR_PFMIN_BAD_CHANNEL, "PFMIN:RAW? needs one argument: channel (1-6)");
        return;
    }
    chArg = atol(tok);
    if ((chArg < 1L) || (chArg > (long)PFM_INPUT_NUM_CHANNELS))
    {
        SendErr(inst, ERR_PFMIN_BAD_CHANNEL, "Invalid PFM_Input channel (1-6)");
        return;
    }
    ch = (uint8_t)(chArg - 1L);
    edges = PfmInput_GetRawEdges(ch);
    if (edges == NULL)
    {
        SendErr(inst, ERR_INVALID_STATE, "channel is not in BOTH-edge mode (PFMIN:EDGes)");
        return;
    }
    count = PfmInput_GetCount(ch);
    mask  = (ch >= 4U) ? 0xFFFFU : 0xFFFFFFFFU;   /* PFM_Input_5/6 are the 16-bit timers TIM3/TIM4 */

    /* Optional page: start index and value count. The ethernet serial bridge
       loses a character at a fixed place in replies of about 1.7 kB or more, so
       the host reads in pages of ~100 values, each with its own SUM. */
    if (tokStart != NULL)
    {
        long s = atol(tokStart);
        if ((s < 0L) || (s > (long)count))
        {
            SendErr(inst, ERR_INVALID_ARGS, "PFMIN:RAW? start must be 0..count");
            return;
        }
        first = (uint16_t)s;
    }
    last = count;
    if (tokCount != NULL)
    {
        long n = atol(tokCount);
        if ((n < 1L) || (n > (long)(count - first)))
        {
            if (n < 1L)
            {
                SendErr(inst, ERR_INVALID_ARGS, "PFMIN:RAW? count must be at least 1");
                return;
            }
            n = (long)(count - first);
        }
        last = (uint16_t)(first + (uint16_t)n);
    }

    /* SUM= is the sum of the numbers that follow (mod 2^32), so the host can tell a
       reply that lost characters on the way (seen on the ethernet serial bridge) from
       a good one and read again. */
    uint32_t sum = 0U;
    for (uint16_t i = first; i < last; i++)
    {
        sum += (i == 0U) ? edges[0] : ((edges[i] - edges[i - 1U]) & mask);
    }
    len += snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len,
                    "OK %u OVERCAP=%u LEVEL=%u SUM=%lu", (unsigned int)count,
                    (unsigned int)PfmInput_GetOvercaptureCount(ch), (unsigned int)PfmInput_GetStartLevel(ch),
                    (unsigned long)sum);
    for (uint16_t i = first; i < last; i++)
    {
        uint32_t v = (i == 0U) ? edges[0] : ((edges[i] - edges[i - 1U]) & mask);
        len += snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len, " %lu", (unsigned long)v);
    }
    snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len, "\r\n");
    uart_send(inst, g_pfminDataBuf);
}
