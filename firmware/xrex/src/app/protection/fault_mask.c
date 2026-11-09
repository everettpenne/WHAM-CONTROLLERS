/*
 * fault_mask.c -- see fault_mask.h.
 */
#include "fault_mask.h"
#include <stdio.h>
#include <string.h>

static uint8_t BitIsFault(uint8_t bit, uint32_t polarity)
{
    return (polarity == FAULT_POLARITY_NORMALLY_HIGH) ? (bit == 0U) : (bit != 0U);
}

uint32_t FaultMask_FromGateDriver(uint16_t raw12, uint8_t enabledChannels,
                                  uint32_t waterPolarity, uint32_t tmpPolarity,
                                  uint32_t enerproPolarity)
{
    uint32_t mask = 0UL;

    for (uint8_t ch = 0U; ch < FM_CHANNELS; ch++)
    {
        if ((enabledChannels & (1U << ch)) == 0U)
        {
            continue;   /* a disabled channel's own pins never count toward a fault */
        }
        if (BitIsFault((uint8_t)((raw12 >> (XR_WATER_FLT_BIT_BASE + ch)) & 1U), waterPolarity) != 0U)
        {
            mask |= FM_WATER(ch);
        }
        if (BitIsFault((uint8_t)((raw12 >> (XR_TMP_FLT_BIT_BASE + ch)) & 1U), tmpPolarity) != 0U)
        {
            mask |= FM_TMP(ch);
        }
        if (BitIsFault((uint8_t)((raw12 >> (XR_ENERPRO_FLT_BIT_BASE + ch)) & 1U), enerproPolarity) != 0U)
        {
            mask |= FM_ENERPRO(ch);
        }
    }
    return mask;
}

/* Appends one name (and a comma first, if the list is not empty). Returns 0 when it
   does not fit whole, leaving the list as it was. */
static uint8_t Append(char *buf, size_t cap, size_t *len, const char *item)
{
    const size_t n   = strlen(item);
    const size_t sep = (*len != 0U) ? 1U : 0U;

    if (*len + sep + n + 1U > cap)
    {
        return 0U;
    }
    if (sep != 0U)
    {
        buf[(*len)++] = ',';
    }
    memcpy(&buf[*len], item, n + 1U);
    *len += n;
    return 1U;
}

size_t FaultMask_Format(uint32_t mask, char *buf, size_t cap)
{
    static const struct { const char *name; uint8_t shift; } perChannel[] = {
        { "WATER", 0U }, { "TMP", 4U }, { "ENERPRO", 8U }, { "OCP", 12U }, { "ENABLE_OUTPUT", 16U }
    };
    static const struct { const char *name; uint32_t bit; } system[] = {
        { "EXTERNAL_ENABLE", FM_EXT_ENABLE }, { "PC10", FM_PC10 }, { "TEST", FM_TEST }
    };
    size_t len = 0U;
    char   item[24];

    if (cap == 0U)
    {
        return 0U;
    }
    buf[0] = '\0';

    if (mask == 0UL)
    {
        (void)Append(buf, cap, &len, "NONE");
        return len;
    }
    for (size_t g = 0U; g < (sizeof(perChannel) / sizeof(perChannel[0])); g++)
    {
        for (size_t ch = 0U; ch < FM_CHANNELS; ch++)
        {
            if ((mask & (1UL << (perChannel[g].shift + ch))) != 0UL)
            {
                (void)snprintf(item, sizeof(item), "%s%u", perChannel[g].name, (unsigned)(ch + 1U));
                if (Append(buf, cap, &len, item) == 0U)
                {
                    return len;
                }
            }
        }
    }
    for (size_t s = 0U; s < (sizeof(system) / sizeof(system[0])); s++)
    {
        if (((mask & system[s].bit) != 0UL) && (Append(buf, cap, &len, system[s].name) == 0U))
        {
            return len;
        }
    }
    return len;
}
