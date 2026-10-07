/*
 * cmd_dac.c -- serial command handlers: the two analog outputs, DAC:*.
 *
 *   DAC:OUTPut <ch> <volts>   ch 1 = PA4 (AnalogOut_01), 2 = PA5 (AnalogOut_02);
 *                             volts from 0 to ANALOG_VREF_MV / 1000, up to 3 decimals
 *   DAC:OUTPut? <ch>          OK <volts> <code commanded> <code in the DAC register>
 *
 * Both outputs are enabled at boot at 0 V and are not affected by the operating
 * state, faults or FIRE: they are plain analog outputs.
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "dac.h"
#include "shot_profile.h"   /* Scpi_ParseScaled(): the shared decimal parser */
#include <stdio.h>
#include <string.h>

static uint8_t ParseChannel(uart_instance_t *inst, const char *tok, uint8_t *channel)
{
    uint32_t v = 0U;

    if ((tok == NULL) || (Scpi_ParseScaled(tok, 0U, &v) == 0U) || (v < 1U) || (v > DAC_NUM_CHANNELS))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "channel must be 1 or 2");
        return 0U;
    }
    *channel = (uint8_t)(v - 1U);
    return 1U;
}

void cmd_dac_output(uart_instance_t *inst, char *args)
{
    char *tokCh = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokV  = (tokCh != NULL) ? strtok(NULL, " \r\n") : NULL;
    char *extra = (tokV != NULL) ? strtok(NULL, " \r\n") : NULL;
    uint8_t  channel;
    uint32_t milliVolts = 0U;
    char msg[64];

    if (ParseChannel(inst, tokCh, &channel) == 0U)
    {
        return;
    }
    if ((tokV == NULL) || (extra != NULL) || (Scpi_ParseScaled(tokV, 3U, &milliVolts) == 0U))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "DAC:OUTPut needs a channel and a voltage: DAC:OUTPut <1|2> <volts>");
        return;
    }
    if ((milliVolts > ANALOG_VREF_MV) || (Dac_SetMilliVolts(channel, (uint16_t)milliVolts) == 0U))
    {
        snprintf(msg, sizeof(msg), "voltage must be 0 to %u.%03u V", (unsigned)(ANALOG_VREF_MV / 1000U),
                 (unsigned)(ANALOG_VREF_MV % 1000U));
        Scpi_SendErr(inst, ERR_INVALID_ARGS, msg);
        return;
    }
    Scpi_ReplyOk(inst);
}

void cmd_dac_output_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint8_t channel;
    uint32_t code;
    uint32_t milliVolts;
    char buf[64];

    if (ParseChannel(inst, tok, &channel) == 0U)
    {
        return;
    }
    code = Dac_GetCode(channel);
    milliVolts = (code * ANALOG_VREF_MV + 2047U) / 4095U;   /* the voltage that code produces */
    snprintf(buf, sizeof(buf), "OK %u.%03u %u %u\r\n", (unsigned)(milliVolts / 1000U), (unsigned)(milliVolts % 1000U),
             (unsigned)code, (unsigned)Dac_GetOutputCode(channel));
    uart_send(inst, buf);
}
