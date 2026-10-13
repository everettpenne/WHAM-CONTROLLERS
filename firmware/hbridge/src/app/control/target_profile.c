/*
 * target_profile.c -- see target_profile.h.
 */
#include "target_profile.h"
#include "scpi_util.h"      /* Scpi_ParseScaled(): the shared unsigned decimal parser */
#include <stddef.h>

static target_setpoint_t g_sp[TARGET_SETPOINT_MAX];
static uint16_t          g_count = 0U;
static uint16_t          g_cursor = 0U;      /* the segment (g_cursor, g_cursor + 1) last evaluated */

void Target_Clear(void)
{
    g_count = 0U;
    g_cursor = 0U;
}

static uint8_t g_badChannel = 0U;

target_append_result_t Target_Append(uint32_t tUs, const int32_t milliVolts[HRTIM_NUM_CHANNELS],
                                     int32_t maxAbsMilliVolts)
{
    if (g_count >= TARGET_SETPOINT_MAX)
    {
        return TARGET_APPEND_FULL;
    }
    if ((g_count == 0U) && (tUs != 0U))
    {
        return TARGET_APPEND_FIRST_NOT_ZERO;
    }
    if ((g_count > 0U) && (tUs <= g_sp[g_count - 1U].tUs))
    {
        return TARGET_APPEND_NOT_INCREASING;
    }
    if (tUs > SHOT_MAX_DURATION_US)
    {
        return TARGET_APPEND_TOO_LONG;
    }
    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        int32_t mag = (milliVolts[ch] < 0) ? -milliVolts[ch] : milliVolts[ch];

        if (mag > maxAbsMilliVolts)
        {
            g_badChannel = ch;
            return TARGET_APPEND_TOO_HIGH;
        }
    }
    g_sp[g_count].tUs = tUs;
    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        g_sp[g_count].milliVolts[ch] = milliVolts[ch];
    }
    g_count++;
    return TARGET_APPEND_OK;
}

target_append_result_t Target_AppendUniform(uint32_t tUs, int32_t milliVolts, int32_t maxAbsMilliVolts)
{
    int32_t v[HRTIM_NUM_CHANNELS];

    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        v[ch] = milliVolts;
    }
    return Target_Append(tUs, v, maxAbsMilliVolts);
}

uint8_t Target_BadChannel(void)
{
    return g_badChannel;
}

uint8_t Target_IsUniform(void)
{
    for (uint16_t i = 0U; i < g_count; i++)
    {
        for (uint8_t ch = 1U; ch < HRTIM_NUM_CHANNELS; ch++)
        {
            if (g_sp[i].milliVolts[ch] != g_sp[i].milliVolts[0])
            {
                return 0U;
            }
        }
    }
    return 1U;
}

int32_t Target_MinMilliVolts(uint8_t ch)
{
    int32_t lowest = 0;

    if (ch >= HRTIM_NUM_CHANNELS)
    {
        return 0;
    }
    for (uint16_t i = 0U; i < g_count; i++)
    {
        if ((i == 0U) || (g_sp[i].milliVolts[ch] < lowest))
        {
            lowest = g_sp[i].milliVolts[ch];
        }
    }
    return lowest;
}

uint16_t Target_Count(void)
{
    return g_count;
}

uint32_t Target_DurationUs(void)
{
    return (g_count == 0U) ? 0U : g_sp[g_count - 1U].tUs;
}

uint8_t Target_Get(uint16_t index, target_setpoint_t *out)
{
    if (index >= g_count)
    {
        return 0U;
    }
    *out = g_sp[index];
    return 1U;
}

uint8_t Target_IsComplete(void)
{
    return (g_count >= 2U) ? 1U : 0U;
}

void Target_Rewind(void)
{
    g_cursor = 0U;
}

float Target_ValueAt(uint8_t ch, uint32_t tUs)
{
    if ((g_count == 0U) || (ch >= HRTIM_NUM_CHANNELS))
    {
        return 0.0f;
    }
    if (tUs >= g_sp[g_count - 1U].tUs)
    {
        return (float)g_sp[g_count - 1U].milliVolts[ch] / 1000.0f;
    }
    if (tUs < g_sp[g_cursor].tUs)
    {
        g_cursor = 0U;                 /* an earlier time than the cursor: search again from the start */
    }
    while (((g_cursor + 2U) < g_count) && (tUs >= g_sp[g_cursor + 1U].tUs))
    {
        g_cursor++;
    }
    {
        const target_setpoint_t *a = &g_sp[g_cursor];
        const target_setpoint_t *b = &g_sp[g_cursor + 1U];
        float span = (float)(b->tUs - a->tUs);
        float frac = (float)(tUs - a->tUs) / span;

        return ((float)a->milliVolts[ch] + ((float)(b->milliVolts[ch] - a->milliVolts[ch]) * frac)) / 1000.0f;
    }
}

uint8_t Target_ParseMilliVolts(const char *text, int32_t *out)
{
    uint32_t mag = 0U;
    uint8_t neg = 0U;

    if (text == NULL)
    {
        return 0U;
    }
    if (*text == '-')
    {
        neg = 1U;
        text++;
    }
    if ((Scpi_ParseScaled(text, 3U, &mag) == 0U) || (mag > 2147483647UL))
    {
        return 0U;
    }
    *out = neg ? -(int32_t)mag : (int32_t)mag;
    return 1U;
}
