/*
 * scpi_util.c -- see scpi_util.h.
 */
#include "scpi_util.h"
#include <stdio.h>
#include <string.h>

/* ---- replies ---- */

void Scpi_SendErr(uart_instance_t *inst, int code, const char *msg)
{
    char buf[128];

    if (snprintf(buf, sizeof(buf), "ERR %d %s\r\n", code, msg) >= (int)sizeof(buf))
    {
        /* a message too long for one reply is cut and marked, but the line must still END: without its CR LF it would run
           into the next reply (found 2026-10-13, when the longer closed-loop messages did exactly that) */
        memcpy(&buf[sizeof(buf) - 6U], "...\r\n", 6U);
    }
    uart_send(inst, buf);
}

void Scpi_ReplyOk(uart_instance_t *inst)
{
    uart_send(inst, "OK\r\n");
}

void Scpi_ReplyFixed(uart_instance_t *inst, float v, uint8_t decimals)
{
    char num[24];
    char buf[40];

    Scpi_FormatFixed(num, sizeof(num), v, decimals);
    snprintf(buf, sizeof(buf), "OK %s\r\n", num);
    uart_send(inst, buf);
}

void Scpi_ReplyFloat(uart_instance_t *inst, float a)
{
    char x[24];
    char buf[40];

    Scpi_FormatFloat(a, x, sizeof(x));
    snprintf(buf, sizeof(buf), "OK %s\r\n", x);
    uart_send(inst, buf);
}

void Scpi_ReplyFloat3(uart_instance_t *inst, float a, float b, float c)
{
    char x[24], y[24], z[24];
    char buf[96];

    Scpi_FormatFloat(a, x, sizeof(x));
    Scpi_FormatFloat(b, y, sizeof(y));
    Scpi_FormatFloat(c, z, sizeof(z));
    snprintf(buf, sizeof(buf), "OK %s %s %s\r\n", x, y, z);
    uart_send(inst, buf);
}

void Scpi_ReplyU32List(uart_instance_t *inst, const uint32_t *values, uint16_t count)
{
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "OK %u", (unsigned int)count);

    for (uint16_t i = 0U; i < count; i++)
    {
        if (len > (int)sizeof(buf) - 12)                 /* room for " 4294967295" and the NUL */
        {
            uart_send(inst, buf);
            len = 0;
        }
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " %lu", (unsigned long)values[i]);
    }
    if (len > (int)sizeof(buf) - 3)
    {
        uart_send(inst, buf);
        len = 0;
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");
    uart_send(inst, buf);
}

void Scpi_ReplySet(uart_instance_t *inst, int result, const char *rangeMsg, const char *busyMsg)
{
    if (result == SCPI_SET_OK)
    {
        Scpi_ReplyOk(inst);
    }
    else if (result == SCPI_SET_BUSY)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, busyMsg);
    }
    else
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, rangeMsg);
    }
}

void Scpi_ReplyBadNumberErr(uart_instance_t *inst, int code, const char *tok)
{
    char shown[28];
    char msg[64];
    size_t i = 0U;

    if (tok != NULL)
    {
        for (; (tok[i] != '\0') && (i < (sizeof(shown) - 1U)); i++)
        {
            shown[i] = ((tok[i] >= ' ') && (tok[i] <= '~')) ? tok[i] : '?';     /* nothing unprintable goes back out */
        }
    }
    shown[i] = '\0';
    snprintf(msg, sizeof(msg), "not a number: %s", shown);
    Scpi_SendErr(inst, code, msg);
}

void Scpi_ReplyBadNumber(uart_instance_t *inst, const char *tok)
{
    Scpi_ReplyBadNumberErr(inst, ERR_INVALID_ARGS, tok);
}

/* ---- number to text ---- */

void Scpi_FormatFixed(char *buf, size_t n, float v, uint8_t decimals)
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

/* 10^k as a float (exact up to 1e10; only used as a scale and a threshold beyond that). */
static float Pow10f(int k)
{
    float r = 1.0f;

    while (k-- > 0)
    {
        r *= 10.0f;
    }
    return r;
}

/* A float as text like printf's "%g": 6 significant digits from a single scaling of the value (so the sixth
   digit can differ from printf's in about 2 % of arbitrary values -- a float has only ~7), trailing zeros
   dropped, fixed notation for exponents -4..5 and "d.ddde+XX" outside. */
void Scpi_FormatFloat(float v, char *out, size_t n)
{
    char tmp[32];
    char digits[7];
    size_t len = 0U;
    int e = 0;
    int nd = 6;
    uint32_t d;
    float a;
    float scaled;

    if (n == 0U)
    {
        return;
    }
    if (v != v)
    {
        snprintf(out, n, "nan");
        return;
    }
    a = (v < 0.0f) ? -v : v;
    if (a > 3.0e38f)
    {
        snprintf(out, n, "%s", (v < 0.0f) ? "-inf" : "inf");
        return;
    }
    if (a == 0.0f)
    {
        snprintf(out, n, "0");
        return;
    }

    /* the decimal exponent: 10^e <= a < 10^(e+1) */
    if (a >= 1.0f)
    {
        while (e < 38 && a >= Pow10f(e + 1)) { e++; }
    }
    else
    {
        while (e > -44 && (a * Pow10f(-e)) < 1.0f) { e--; }  /* a < 10^e  <=>  a * 10^-e < 1 */
    }
    scaled = (e >= 0) ? a / Pow10f(e) : a * Pow10f(-e);
    d = (uint32_t)(scaled * 100000.0f + 0.5f);
    if (d >= 1000000UL)                                    /* rounded up to the next power of ten */
    {
        d = 100000UL;
        e++;
    }

    for (int i = 5; i >= 0; i--)                           /* six digits, trailing zeros dropped */
    {
        digits[i] = (char)('0' + (d % 10U));
        d /= 10U;
    }
    digits[6] = '\0';
    while (nd > 1 && digits[nd - 1] == '0')
    {
        nd--;
    }

    if (v < 0.0f)
    {
        tmp[len++] = '-';
    }
    if (e >= -4 && e < 6)
    {
        if (e >= 0)
        {
            for (int i = 0; i <= e; i++)
            {
                tmp[len++] = (i < nd) ? digits[i] : '0';
            }
            if (nd > e + 1)
            {
                tmp[len++] = '.';
                for (int i = e + 1; i < nd; i++)
                {
                    tmp[len++] = digits[i];
                }
            }
        }
        else
        {
            tmp[len++] = '0';
            tmp[len++] = '.';
            for (int i = 0; i < (-e - 1); i++)
            {
                tmp[len++] = '0';
            }
            for (int i = 0; i < nd; i++)
            {
                tmp[len++] = digits[i];
            }
        }
    }
    else
    {
        int ae = (e < 0) ? -e : e;

        tmp[len++] = digits[0];
        if (nd > 1)
        {
            tmp[len++] = '.';
            for (int i = 1; i < nd; i++)
            {
                tmp[len++] = digits[i];
            }
        }
        tmp[len++] = 'e';
        tmp[len++] = (e < 0) ? '-' : '+';
        tmp[len++] = (char)('0' + (ae / 10) % 10);
        tmp[len++] = (char)('0' + ae % 10);
    }
    tmp[len] = '\0';
    snprintf(out, n, "%s", tmp);
}

/* ---- strict text to number ---- */


uint8_t Scpi_ParseLong(const char *tok, long *out)
{
    unsigned long v = 0UL;
    uint8_t neg = 0U;
    const char *p;

    if (tok == NULL)
    {
        return 0U;
    }
    p = tok;
    if ((*p == '+') || (*p == '-'))
    {
        neg = (uint8_t)(*p == '-');
        p++;
    }
    if (*p == '\0')
    {
        return 0U;
    }
    for (; *p != '\0'; p++)
    {
        unsigned long d;

        if ((*p < '0') || (*p > '9'))
        {
            return 0U;
        }
        d = (unsigned long)(*p - '0');
        if (v > ((~0UL) - d) / 10UL)
        {
            return 0U;
        }
        v = v * 10UL + d;
    }
    if (neg != 0U)
    {
        if (v > ((~0UL) >> 1) + 1UL)
        {
            return 0U;
        }
        *out = (long)(0UL - v);
    }
    else
    {
        if (v > ((~0UL) >> 1))
        {
            return 0U;
        }
        *out = (long)v;
    }
    return 1U;
}

uint8_t Scpi_ParseFloat(const char *tok, float *out)
{
    uint64_t mant = 0U;
    int exp10 = 0;
    int digits = 0;
    int e = 0;
    uint8_t neg = 0U;
    uint8_t seenPoint = 0U;
    uint8_t expNeg = 0U;
    double d;
    const char *p;

    if (tok == NULL)
    {
        return 0U;
    }
    p = tok;
    if ((*p == '+') || (*p == '-'))
    {
        neg = (uint8_t)(*p == '-');
        p++;
    }
    for (; (*p != '\0') && (*p != 'e') && (*p != 'E'); p++)
    {
        if ((*p == '.') && (seenPoint == 0U))
        {
            seenPoint = 1U;
        }
        else if ((*p >= '0') && (*p <= '9'))
        {
            if (mant > 900000000000000ULL)
            {
                return 0U;                                  /* more digits than a double holds exactly */
            }
            mant = mant * 10U + (uint64_t)(*p - '0');
            digits++;
            if (seenPoint != 0U)
            {
                exp10--;
            }
        }
        else
        {
            return 0U;
        }
    }
    if (digits == 0)
    {
        return 0U;
    }
    if ((*p == 'e') || (*p == 'E'))
    {
        int expDigits = 0;

        p++;
        if ((*p == '+') || (*p == '-'))
        {
            expNeg = (uint8_t)(*p == '-');
            p++;
        }
        for (; *p != '\0'; p++)
        {
            if ((*p < '0') || (*p > '9') || (e > 400))
            {
                return 0U;
            }
            e = e * 10 + (*p - '0');
            expDigits++;
        }
        if (expDigits == 0)
        {
            return 0U;
        }
        exp10 += (expNeg != 0U) ? -e : e;
    }

    d = (double)mant;
    if (exp10 > 60 || exp10 < -60)
    {
        return 0U;
    }
    while (exp10 > 0)
    {
        int step = (exp10 > 22) ? 22 : exp10;
        double p10 = 1.0;

        for (int i = 0; i < step; i++) { p10 *= 10.0; }
        d *= p10;
        exp10 -= step;
    }
    while (exp10 < 0)
    {
        int step = (exp10 < -22) ? 22 : -exp10;
        double p10 = 1.0;

        for (int i = 0; i < step; i++) { p10 *= 10.0; }
        d /= p10;
        exp10 += step;
    }
    if (d > 3.4e38)
    {
        return 0U;                                          /* beyond a float */
    }
    *out = (float)((neg != 0U) ? -d : d);
    return 1U;
}

uint8_t Scpi_ParseBool(const char *tok, uint8_t *out)
{
    if ((tok == NULL) || ((tok[0] != '0') && (tok[0] != '1')) || (tok[1] != '\0'))
    {
        return 0U;
    }
    *out = (uint8_t)(tok[0] - '0');
    return 1U;
}

uint8_t Scpi_ParseScaled(const char *text, uint8_t decimals, uint32_t *out)
{
    uint64_t whole = 0U;
    uint64_t frac = 0U;
    uint8_t  fracDigits = 0U;
    uint8_t  seenPoint = 0U;
    uint8_t  seenDigit = 0U;
    uint64_t scale = 1U;

    if (text == NULL)
    {
        return 0U;
    }
    for (uint8_t i = 0U; i < decimals; i++)
    {
        scale *= 10U;
    }
    for (const char *p = text; *p != '\0'; p++)
    {
        if ((*p >= '0') && (*p <= '9'))
        {
            seenDigit = 1U;
            if (seenPoint == 0U)
            {
                whole = whole * 10U + (uint64_t)(*p - '0');
                if (whole > 0xFFFFFFFFULL)
                {
                    return 0U;
                }
            }
            else
            {
                if (fracDigits >= decimals)
                {
                    return 0U;   /* more decimals than the unit has */
                }
                frac = frac * 10U + (uint64_t)(*p - '0');
                fracDigits++;
            }
        }
        else if ((*p == '.') && (seenPoint == 0U) && (decimals > 0U))
        {
            seenPoint = 1U;
        }
        else
        {
            return 0U;
        }
    }
    if (seenDigit == 0U)
    {
        return 0U;
    }
    for (uint8_t i = fracDigits; i < decimals; i++)
    {
        frac *= 10U;
    }
    whole = whole * scale + frac;
    if (whole > 0xFFFFFFFFULL)
    {
        return 0U;
    }
    *out = (uint32_t)whole;
    return 1U;
}

uint8_t Scpi_ParseUnsigned(const char *tok, uint8_t decimals, float *out)
{
    uint32_t v = 0U;
    float div = 1.0f;

    if (Scpi_ParseScaled(tok, decimals, &v) == 0U)
    {
        return 0U;
    }
    for (uint8_t i = 0U; i < decimals; i++) { div *= 10.0f; }
    *out = (float)v / div;
    return 1U;
}

uint8_t Scpi_GetLong(uart_instance_t *inst, int code, const char *tok, long *out)
{
    if (Scpi_ParseLong(tok, out) == 0U)
    {
        Scpi_ReplyBadNumberErr(inst, code, tok);
        return 0U;
    }
    return 1U;
}

uint8_t Scpi_GetFloat(uart_instance_t *inst, int code, const char *tok, float *out)
{
    if (Scpi_ParseFloat(tok, out) == 0U)
    {
        Scpi_ReplyBadNumberErr(inst, code, tok);
        return 0U;
    }
    return 1U;
}
