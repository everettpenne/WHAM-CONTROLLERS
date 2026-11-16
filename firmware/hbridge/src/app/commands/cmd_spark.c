/*
 * cmd_spark.c -- serial command handlers for the spark-gap fault input (PF13, spark_gap.h): SPARKgap:INPut?, SPARKgap:ENAble,
 * SPARKgap:ENAble?. See commands.h for the file map.
 */
#include "commands.h"
#include "cmd_common.h"
#include "spark_gap.h"
#include "state_machine.h"
#include <stdio.h>
#include <string.h>

/* SPARKgap:INPut? -- OK <0|1>: the raw level of the spark-gap input PF13 right now, 1 = HIGH (healthy), 0 = LOW (a spark-gap
   fault). Read whether or not the input is switched on, latched or bypassed. FAULT:SOURces? shows SPARK=1 once one has latched. */
void cmd_spark_gap_input_query(uart_instance_t *inst, char *args)
{
    char buf[16];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned)SparkGap_Read());
    uart_send(inst, buf);
}

/* --------------------------------------------------------------------------
 * SPARKgap:ENAble <0|1> / SPARKgap:ENAble?      (added 2026-11-16)
 *
 * Switches the spark-gap fault input off (0) and on (1), like GDS:ENAble does for the gate-driver inputs. A switched-off input is not
 * evaluated at all: it cannot latch a fault or stop the output, whatever PF13 reads. On at every boot (SPARK_GAP_DEFAULT_ENABLED,
 * ctrlr_config.h); not remembered across a reboot except in a saved profile (*SAV stores it, *RCL puts it back -- and the reply
 * carries SPARKOFF=1 when a recall left it off). This switches a protection off: the hub announces it as DANGEROUS ("interlock").
 *
 *   SPARKgap:ENAble 0    OK     ERR 12 not exactly 0 or 1, ERR 13 when switching OFF outside IDLE/FAULT
 *   SPARKgap:ENAble?     OK <0|1>
 *
 * Switching it OFF needs IDLE or FAULT (a protection is not removed from an armed or running controller); switching it ON is
 * allowed in any state, and the pin is re-checked at once, so turning it on while PF13 reads LOW faults immediately. Switching it
 * off does not clear a latched fault: FAULT:CLEar does, and its re-check then ignores the switched-off input.
 * -------------------------------------------------------------------------- */
void cmd_spark_gap_enable(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint8_t on;

    if ((tok == NULL) || (strtok(NULL, " \r\n") != NULL))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "SPARKgap:ENAble needs one argument: 0 or 1");
        return;
    }
    if ((strcmp(tok, "0") != 0) && (strcmp(tok, "1") != 0))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "SPARKgap:ENAble: the argument must be 0 or 1");
        return;
    }
    on = (uint8_t)(tok[0] - '0');

    if ((on == 0U) && (SM_GetState() != SM_STATE_IDLE) && (SM_GetState() != SM_STATE_FAULT))
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "the spark-gap input can only be switched off in IDLE or FAULT");
        return;
    }
    SparkGap_SetEnabled(on);
    SparkGap_CheckFault();                   /* an input just switched on may already be reading LOW */
    Scpi_ReplyOk(inst);
}

void cmd_spark_gap_enable_query(uart_instance_t *inst, char *args)
{
    char buf[16];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned)SparkGap_IsEnabled());
    uart_send(inst, buf);
}
