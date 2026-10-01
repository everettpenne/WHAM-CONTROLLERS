/*
 * cmd_adc.c -- serial command handlers: the four analog inputs, ADC:*.
 *
 *   ADC:INPut? <1-4>   OK <volts> <code>   AnalogIn_01..04 = PA0, PA1, PC0, PB0
 *   ADC:VREF?          OK <volts> <vrefint code> <factory code>
 *
 * ADC:INPut? converts a code to volts with ANALOG_VREF_MV (ctrlr_config.h, an
 * produced by the internal reference). ADC:VREF? measures the real voltage on VREF+
 * from the internal bandgap and the chip's own factory calibration, so ANALOG_VREF_MV
 * can be checked against it.
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "adc.h"
#include "shot_profile.h"   /* ShotProfile_ParseScaled(): the shared decimal parser */
#include <stdio.h>
#include <string.h>

void cmd_adc_input_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint32_t ch = 0U;
    uint16_t code = 0U;
    uint32_t mv;
    char buf[48];

    if ((tok == NULL) || (ShotProfile_ParseScaled(tok, 0U, &ch) == 0U) || (ch < 1U) || (ch > ADC_NUM_CHANNELS))
    {
        SendErr(inst, ERR_INVALID_ARGS, "channel must be 1 to 4");
        return;
    }
    if (Adc_Read((uint8_t)(ch - 1U), &code) == 0U)
    {
        SendErr(inst, ERR_INVALID_STATE, "ADC not ready or conversion timed out");
        return;
    }
    mv = ((uint32_t)code * ANALOG_VREF_MV + 2047U) / 4095U;
    snprintf(buf, sizeof(buf), "OK %u.%03u %u\r\n", (unsigned)(mv / 1000U), (unsigned)(mv % 1000U), (unsigned)code);
    uart_send(inst, buf);
}

void cmd_adc_vref_query(uart_instance_t *inst, char *args)
{
    uint16_t code = 0U;
    uint16_t cal = 0U;
    uint32_t mv;
    char buf[48];
    (void)args;

    if ((Adc_ReadVrefint(&code, &cal) == 0U) || (code == 0U) || (cal == 0U))
    {
        SendErr(inst, ERR_INVALID_STATE, "ADC not ready or conversion timed out");
        return;
    }
    mv = (3000U * (uint32_t)cal + (code / 2U)) / code;   /* the factory code was taken with VREF+ = 3.0 V */
    snprintf(buf, sizeof(buf), "OK %u.%03u %u %u\r\n", (unsigned)(mv / 1000U), (unsigned)(mv % 1000U),
             (unsigned)code, (unsigned)cal);
    uart_send(inst, buf);
}
