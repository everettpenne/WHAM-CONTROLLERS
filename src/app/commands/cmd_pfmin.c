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

/* Reply buffer for PFMIN:DATA? / PFMIN:RAW?: at most PFMIN_PAGE_MAX values per
   reply, each up to " 4294967295" (11 chars) plus the header and CRLF. static,
   not a stack local: this project's linker script reserves only _Min_Stack_Size
   (1 KB) for the stack. */
#define PFMIN_PAGE_MAX  (400U)
static char g_pfminDataBuf[PFMIN_PAGE_MAX * 11U + 96U];

/* Parses the optional <start> [<count>] page arguments against `total` values.
   A reply carries at most PFMIN_PAGE_MAX values (a longer request is cut to that:
   read the rest with a later start). The ethernet serial bridge loses characters
   in replies of about 1.7 kB or more, so the host reads ~100 at a time. Sends the
   error and returns 0 on a bad argument. */
static uint8_t ParsePage(uart_instance_t *inst, const char *name, const char *tokStart,
                         const char *tokCount, uint16_t total, uint16_t *first, uint16_t *last)
{
    long s = 0L;
    long n;
    char msg[64];

    if (tokStart != NULL)
    {
        s = atol(tokStart);
        if ((s < 0L) || (s > (long)total))
        {
            snprintf(msg, sizeof(msg), "%s start must be 0..count", name);
            SendErr(inst, ERR_INVALID_ARGS, msg);
            return 0U;
        }
    }
    n = (long)total - s;
    if (tokCount != NULL)
    {
        long req = atol(tokCount);
        if (req < 1L)
        {
            snprintf(msg, sizeof(msg), "%s count must be at least 1", name);
            SendErr(inst, ERR_INVALID_ARGS, msg);
            return 0U;
        }
        if (req < n)
        {
            n = req;
        }
    }
    if (n > (long)PFMIN_PAGE_MAX)
    {
        n = (long)PFMIN_PAGE_MAX;
    }
    *first = (uint16_t)s;
    *last  = (uint16_t)(s + n);
    return 1U;
}

/* PFMIN:DATA? <ch> [<start> [<count>]] -- OK <periods> OVERCAP=<k> SUM=<s> <p1> <p2> ...
   <periods> is the total; the numbers are the rising-edge-to-rising-edge periods of
   the page asked for (all of them, up to PFMIN_PAGE_MAX, without start/count); SUM is
   their sum mod 2^32, a transport check. */
void cmd_pfmin_data(uart_instance_t *inst, char *args)
{
    char    *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char    *tokStart = (tok != NULL) ? strtok(NULL, " \r\n") : NULL;
    char    *tokCount = (tokStart != NULL) ? strtok(NULL, " \r\n") : NULL;
    long     chArg;
    uint8_t  ch;
    uint16_t count;
    uint16_t first;
    uint16_t last;
    const uint16_t *stamps;
    uint32_t sum = 0U;
    int      len = 0;

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

    count  = PfmInput_GetCount(ch);
    stamps = PfmInput_GetStamps(ch);
    if (ParsePage(inst, "PFMIN:DATA?", tokStart, tokCount, count, &first, &last) == 0U)
    {
        return;
    }
    if (stamps == NULL)
    {
        first = 0U;
        last  = 0U;   /* an inactive channel has nothing: count is 0 */
    }

    for (uint16_t i = first; i < last; i++)
    {
        sum += (uint32_t)(uint16_t)(stamps[i + 1U] - stamps[i]);
    }
    /* OVERCAP=<n>: see PfmInput_GetOvercaptureCount(). */
    len += snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len,
                     "OK %u OVERCAP=%u SUM=%lu", (unsigned int)count,
                     (unsigned int)PfmInput_GetOvercaptureCount(ch), (unsigned long)sum);
    for (uint16_t i = first; i < last; i++)
    {
        len += snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len,
                         " %u", (unsigned int)(uint16_t)(stamps[i + 1U] - stamps[i]));
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
 * <n> edges; t0 is the first edge's raw 16-bit timer value and each following number
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
    uint8_t ch;
    uint16_t count;
    uint16_t first;
    uint16_t last;
    const uint16_t *stamps;
    uint32_t sum = 0U;
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
    stamps = PfmInput_GetStamps(ch);
    if ((stamps == NULL) || (PfmInput_CapturedBothEdges(ch) == 0U))
    {
        SendErr(inst, ERR_INVALID_STATE, "channel is not in BOTH-edge mode (PFMIN:EDGes)");
        return;
    }
    count = PfmInput_GetCount(ch);
    if (ParsePage(inst, "PFMIN:RAW?", tokStart, tokCount, count, &first, &last) == 0U)
    {
        return;
    }

    /* SUM= is the sum of the numbers that follow (mod 2^32), so the host can tell a
       reply that lost characters on the way (seen on the ethernet serial bridge) from
       a good one and read again. The first value of the stream is the first edge's raw
       16-bit timer value, every other is the difference from the previous edge. */
    for (uint16_t i = first; i < last; i++)
    {
        sum += (i == 0U) ? stamps[0] : (uint32_t)(uint16_t)(stamps[i] - stamps[i - 1U]);
    }
    len += snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len,
                    "OK %u OVERCAP=%u LEVEL=%u SUM=%lu", (unsigned int)count,
                    (unsigned int)PfmInput_GetOvercaptureCount(ch), (unsigned int)PfmInput_GetStartLevel(ch),
                    (unsigned long)sum);
    for (uint16_t i = first; i < last; i++)
    {
        uint32_t v = (i == 0U) ? stamps[0] : (uint32_t)(uint16_t)(stamps[i] - stamps[i - 1U]);
        len += snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len, " %lu", (unsigned long)v);
    }
    snprintf(&g_pfminDataBuf[len], sizeof(g_pfminDataBuf) - (size_t)len, "\r\n");
    uart_send(inst, g_pfminDataBuf);
}
