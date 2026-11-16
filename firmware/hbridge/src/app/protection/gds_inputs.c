/*
 * gds_inputs.c -- see gds_inputs.h. Pure logic: polarity evaluation, the per-input enable mask, nicknames.
 */
#include "gds_inputs.h"
#include "ctrlr_config.h"
#include <string.h>

/* Read from the EXTI interrupts (gate_driver.c), written from the main loop. */
static volatile uint16_t g_enabledMask = (uint16_t)(GDS_DEFAULT_ENABLE_MASK & GDS_ALL_INPUTS_MASK);
/* 1 = HIGH is a fault (NORMALLY_LOW). Starts at, and Reset() returns to, the compiled GDS_FAULT_POLARITY. */
static volatile uint8_t g_normallyLow = (GDS_FAULT_POLARITY == GDS_NORMALLY_LOW) ? 1U : 0U;
static char g_nick[GDS_INPUT_COUNT][GDS_NICKNAME_MAX_LEN + 1U];

void GdsInputs_Reset(void)
{
    g_enabledMask = (uint16_t)(GDS_DEFAULT_ENABLE_MASK & GDS_ALL_INPUTS_MASK);
    g_normallyLow = (GDS_FAULT_POLARITY == GDS_NORMALLY_LOW) ? 1U : 0U;
    memset(g_nick, 0, sizeof(g_nick));
}

uint16_t GdsInputs_EnabledMask(void)
{
    return g_enabledMask;
}

uint8_t GdsInputs_IsEnabled(uint8_t input)
{
    if ((input < 1U) || (input > GDS_INPUT_COUNT)) { return 0U; }
    return (uint8_t)((g_enabledMask >> (input - 1U)) & 1U);
}

uint8_t GdsInputs_SetEnabled(uint8_t input, uint8_t enabled)
{
    uint16_t bits;

    if (input > GDS_INPUT_COUNT) { return 0U; }
    bits = (input == 0U) ? GDS_ALL_INPUTS_MASK : (uint16_t)(1U << (input - 1U));
    g_enabledMask = (enabled != 0U) ? (uint16_t)(g_enabledMask | bits) : (uint16_t)(g_enabledMask & (uint16_t)~bits);
    return 1U;
}

uint8_t GdsInputs_NicknameIsValid(const char *name)
{
    size_t n;

    if (name == NULL) { return 0U; }
    n = strlen(name);
    if ((n < 1U) || (n > GDS_NICKNAME_MAX_LEN)) { return 0U; }
    if ((n == 1U) && (name[0] == '-')) { return 0U; }          /* "-" is how a missing nickname is printed */
    for (size_t i = 0U; i < n; i++)
    {
        char c = name[i];
        uint8_t ok = (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) ||
                      (c == '_') || (c == '.') || (c == '-') || (c == '+')) ? 1U : 0U;
        if (ok == 0U) { return 0U; }
    }
    return 1U;
}

uint8_t GdsInputs_SetNickname(uint8_t input, const char *name)
{
    if ((input < 1U) || (input > GDS_INPUT_COUNT) || (name == NULL)) { return 0U; }
    if (strcmp(name, "-") == 0)
    {
        g_nick[input - 1U][0] = '\0';
        return 1U;
    }
    if (GdsInputs_NicknameIsValid(name) == 0U) { return 0U; }
    strcpy(g_nick[input - 1U], name);                          /* length checked above */
    return 1U;
}

const char *GdsInputs_Nickname(uint8_t input)
{
    if ((input < 1U) || (input > GDS_INPUT_COUNT)) { return ""; }
    return g_nick[input - 1U];
}

uint16_t GdsInputs_RawFaultBits(uint16_t raw, uint8_t normallyLow)
{
    return (normallyLow != 0U) ? (uint16_t)(raw & GDS_ALL_INPUTS_MASK)
                               : (uint16_t)((uint16_t)~raw & GDS_ALL_INPUTS_MASK);
}

uint8_t GdsInputs_PolarityIsNormallyLow(void)
{
    return g_normallyLow;
}

void GdsInputs_SetPolarityNormallyLow(uint8_t normallyLow)
{
    g_normallyLow = (normallyLow != 0U) ? 1U : 0U;
}

uint8_t GdsInputs_DefaultPolarityIsNormallyLow(void)
{
    return (GDS_FAULT_POLARITY == GDS_NORMALLY_LOW) ? 1U : 0U;
}

uint16_t GdsInputs_FaultBits(uint16_t raw)
{
    return (uint16_t)(GdsInputs_RawFaultBits(raw, GdsInputs_PolarityIsNormallyLow()) & g_enabledMask);
}
