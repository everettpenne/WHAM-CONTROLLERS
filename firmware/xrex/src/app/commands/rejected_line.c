/*
 * rejected_line.c -- see rejected_line.h.
 */
#include "rejected_line.h"
#include <stdio.h>
#include <string.h>

static uint8_t  g_raw[REJECTED_LINE_KEEP];
static size_t   g_len;     /* true length of the last refused line */
static size_t   g_kept;    /* bytes actually kept */
static uint32_t g_count;

void RejectedLine_Record(const uint8_t *raw, size_t len)
{
    g_kept = (len < REJECTED_LINE_KEEP) ? len : REJECTED_LINE_KEEP;
    memcpy(g_raw, raw, g_kept);
    g_len = len;
    g_count++;
}

uint32_t RejectedLine_Count(void)
{
    return g_count;
}

size_t RejectedLine_Format(char *buf, size_t cap)
{
    size_t n = 0U;

    if (cap == 0U)
    {
        return 0U;
    }
    n += (size_t)snprintf(buf, cap, "COUNT=%lu LEN=%lu HEX=", (unsigned long)g_count, (unsigned long)g_len);
    for (size_t i = 0U; (i < g_kept) && (n + 3U < cap); i++)
    {
        n += (size_t)snprintf(&buf[n], cap - n, "%02X", (unsigned)g_raw[i]);
    }
    if (n + 7U < cap)
    {
        n += (size_t)snprintf(&buf[n], cap - n, " TEXT=");
    }
    for (size_t i = 0U; i < g_kept; i++)
    {
        const uint8_t c = g_raw[i];
        const char   *fmt;
        char          one[5];

        if (c == (uint8_t)'\\')                 { fmt = "\\\\"; }
        else if ((c >= 0x20U) && (c < 0x7FU))   { one[0] = (char)c; one[1] = '\0'; fmt = one; }
        else                                    { (void)snprintf(one, sizeof(one), "\\x%02X", (unsigned)c); fmt = one; }
        if (n + strlen(fmt) + 1U > cap)
        {
            break;
        }
        n += (size_t)snprintf(&buf[n], cap - n, "%s", fmt);
    }
    return n;
}
