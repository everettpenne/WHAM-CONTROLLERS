/*
 * cmd_charge.c -- serial command handlers for the CHARGE output: CHARGE:FREQuency, CHARGE:FREQuency?, CHARGE:STATus?.
 *
 * Any state: the handlers do not look at the state machine or the fault latch (charge.h says why). The argument is a
 * whole number of hertz: 0 turns the output off, otherwise CHARGE_MIN_HZ..CHARGE_MAX_HZ.
 */
#include "commands.h"
#include "cmd_common.h"
#include "charge.h"
#include "ctrlr_config.h"
#include <stdio.h>
#include <string.h>

void cmd_charge_freq(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    long hz;

    if ((tok == NULL) || (strtok(NULL, " \r\n") != NULL))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "CHARGE:FREQuency needs one argument: <hz> (0 = off)");
        return;
    }
    SCPI_NEED_LONG(inst, tok, hz);
    if ((hz < 0L) || (hz > (long)CHARGE_MAX_HZ) || ((hz > 0L) && (hz < (long)CHARGE_MIN_HZ)))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "CHARGE:FREQuency must be 0 (off) or 1-100000 Hz");
        return;
    }
    if (Charge_SetFrequency((uint32_t)hz) == 0U)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "CHARGE:FREQuency cannot be made with this timer clock");
        return;
    }
    Scpi_ReplyOk(inst);
}

void cmd_charge_freq_query(uart_instance_t *inst, char *args)
{
    char buf[24];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %lu\r\n", (unsigned long)Charge_GetFrequency());
    uart_send(inst, buf);
}

void cmd_charge_status_query(uart_instance_t *inst, char *args)
{
    charge_status_t st;
    char buf[128];
    (void)args;

    Charge_GetStatus(&st);
    snprintf(buf, sizeof(buf), "OK freq=%lu running=%u actual=%lu.%03lu psc=%lu arr=%lu ccr=%lu\r\n",
             (unsigned long)st.set_hz, (unsigned)st.running, (unsigned long)(st.actual_mhz / 1000UL),
             (unsigned long)(st.actual_mhz % 1000UL), (unsigned long)st.psc, (unsigned long)st.arr, (unsigned long)st.ccr);
    uart_send(inst, buf);
}
