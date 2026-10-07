/*
 * phase_mask.c -- see phase_mask.h.
 */
#include "phase_mask.h"
#include <string.h>

char PhaseMask_Letter(uint8_t channel)
{
    static const char kLetters[] = "UVWXYZ";

    return (channel < 6U) ? kLetters[channel] : '?';
}

uint8_t PhaseMask_All(uint8_t phaseCount)
{
    return (phaseCount >= 8U) ? 0xFFU : (uint8_t)((1U << phaseCount) - 1U);
}

static char Upper(char c)
{
    return ((c >= 'a') && (c <= 'z')) ? (char)(c - 'a' + 'A') : c;
}

static uint8_t WordIs(const char *text, const char *word)
{
    size_t i = 0U;

    for (; word[i] != '\0'; i++)
    {
        if (Upper(text[i]) != word[i])
        {
            return 0U;
        }
    }
    return (uint8_t)(text[i] == '\0');
}

phase_parse_t PhaseMask_Parse(const char *text, uint8_t phaseCount, uint8_t allowNone, uint8_t *mask)
{
    uint8_t m = 0U;
    uint8_t letters = 0U;
    uint8_t digits = 0U;

    if (text == NULL)
    {
        return PHASE_PARSE_EMPTY;
    }
    if (WordIs(text, "ALL"))
    {
        *mask = PhaseMask_All(phaseCount);
        return PHASE_PARSE_OK;
    }
    if (WordIs(text, "NONE"))
    {
        if (allowNone == 0U)
        {
            return PHASE_PARSE_BAD;
        }
        *mask = 0U;
        return PHASE_PARSE_OK;
    }

    for (const char *p = text; *p != '\0'; p++)
    {
        char c = Upper(*p);

        if (c == ',')
        {
            continue;
        }
        if ((c >= '1') && (c <= '9') && ((uint8_t)(c - '1') < phaseCount))
        {
            m = (uint8_t)(m | (1U << (c - '1')));
            digits++;
            continue;
        }
        {
            uint8_t k;

            for (k = 0U; k < phaseCount; k++)
            {
                if (PhaseMask_Letter(k) == c)
                {
                    break;
                }
            }
            if (k >= phaseCount)
            {
                return PHASE_PARSE_BAD;                 /* not a phase of this build */
            }
            m = (uint8_t)(m | (1U << k));
            letters++;
        }
    }
    if ((letters == 0U) && (digits == 0U))
    {
        return PHASE_PARSE_EMPTY;
    }
    if ((letters != 0U) && (digits != 0U))
    {
        return PHASE_PARSE_BAD;                         /* "U2": pick one way of naming them */
    }
    *mask = m;
    return PHASE_PARSE_OK;
}

void PhaseMask_Format(uint8_t mask, uint8_t phaseCount, char *out, size_t n)
{
    size_t len = 0U;

    if (n == 0U)
    {
        return;
    }
    for (uint8_t k = 0U; k < phaseCount && (len + 1U) < n; k++)
    {
        if (((mask >> k) & 1U) != 0U)
        {
            out[len++] = PhaseMask_Letter(k);
        }
    }
    if (len == 0U)
    {
        strncpy(out, "NONE", n - 1U);
        out[(n < 5U) ? (n - 1U) : 4U] = '\0';
        return;
    }
    out[len] = '\0';
}
