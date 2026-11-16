/*
 * cmd_gds.c -- serial command handlers for the 12 GateDriverStatus inputs (PE0..PE11): GDS?, GDS:LAST?,
 * GDS:POLarity?, GDS:ENAble, GDS:NICKname. GDS? and GDS:LAST? were in cmd_io.c until 2026-11-13; the handlers
 * are unchanged. See commands.h for the file map and gds_inputs.h for what the enable switch and nicknames are.
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "gate_driver.h"
#include "gds_inputs.h"
#include "state_machine.h"
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * GDS?
 *
 * Raw HIGH/LOW snapshot of all 12 GateDriverStatus pins (PE0..PE11,
 * gate_driver.h) -- added 2026-09-08 as a diagnostic while chasing why
 * a fault wasn't being registered. Deliberately a single OK line (one
 * "NN=HIGH" or "NN=LOW" token per pin, NN = 01..12 matching the
 * GateDriverStatus_01..12 silkscreen/schematic numbering, space
 * separated, PE0 first) rather than the sibling PFM-STM32G474
 * project's multi-line/bitmask GDS? formats -- matches this project's
 * existing single-"OK <value>"-line convention (commands.h) instead of
 * introducing a new multi-line reply shape for just this one command.
 * Raw and uncached like the sibling project's GDS?: reads GPIOE->IDR
 * live at the moment of the query, no debounce, no polarity
 * interpretation, no fault-latching of its own -- this command itself
 * is read-only visibility and cannot stop PWM output by itself. (These
 * same 12 pins DO now gate PWM output, via a separate path: the
 * GateDriverStatus EXTI interrupt, gate_driver.h's
 * GateDriver_CheckFault(), added 2026-09-08 -- GDS? is unaffected by
 * and independent of that mechanism, just a raw snapshot either way.)
 * -------------------------------------------------------------------------- */
void cmd_gds_query(uart_instance_t *inst, char *args)
{
    uint16_t mask = GateDriver_Read();
    char buf[160];
    int  len = 0;
    (void)args;

    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, "OK");
    for (uint8_t pin = 0U; pin < 12U; pin++)
    {
        int high = ((mask >> pin) & 1U) != 0U;
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len,
                         " %02u=%s", (unsigned int)(pin + 1U), high ? "HIGH" : "LOW");
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");

    uart_send(inst, buf);
}

/* GDS:LAST? -- OK <raw pins when the last gate-driver fault latched, hex, bit 0 = GateDriverStatus_01> <fault checks since
   boot>: which input(s) tripped the last GateDriverStatus fault. A set bit is a pin that read HIGH, which is a fault only
   under NORMALLY_LOW -- GDS:POLarity? says which this build uses. */
void cmd_gds_last_query(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;
    snprintf(buf, sizeof(buf), "OK %03X %lu\r\n", (unsigned)GateDriver_DebugBadRaw(), (unsigned long)GateDriver_DebugEdges());
    uart_send(inst, buf);
}

/* GDS:POLarity? -- OK NORMALLY_HIGH | NORMALLY_LOW: the fault polarity this firmware was BUILT with (GDS_FAULT_POLARITY in
   ctrlr_config.h). NORMALLY_HIGH: the pins read HIGH when healthy, LOW is a fault. Read-only: it is a compile-time setting. */
void cmd_gds_polarity_query(uart_instance_t *inst, char *args)
{
    (void)args;
    uart_send(inst, (GdsInputs_PolarityIsNormallyLow() != 0U) ? "OK NORMALLY_LOW\r\n" : "OK NORMALLY_HIGH\r\n");
}

/* The input argument of GDS:ENAble / GDS:NICKname: a whole-token integer 0..12 (minInput 0 for "all", else 1).
   Replies ERR 12 itself and returns 0 when it is not. */
static uint8_t ParseInput(uart_instance_t *inst, const char *tok, uint8_t minInput, const char *usage, uint8_t *out)
{
    long v;
    char msg[64];

    if (Scpi_ParseLong(tok, &v) == 0U)
    {
        Scpi_ReplyBadNumberErr(inst, ERR_INVALID_ARGS, tok);
        return 0U;
    }
    if ((v < (long)minInput) || (v > (long)GDS_INPUT_COUNT))
    {
        snprintf(msg, sizeof(msg), "%s: input must be %u-%u%s", usage, (unsigned)minInput, (unsigned)GDS_INPUT_COUNT,
                 (minInput == 0U) ? " (0 = all)" : "");
        Scpi_SendErr(inst, ERR_INVALID_ARGS, msg);
        return 0U;
    }
    *out = (uint8_t)v;
    return 1U;
}

/* --------------------------------------------------------------------------
 * GDS:ENAble <input> <0|1> / GDS:ENAble? [input]      (added 2026-11-13)
 *
 * Switches individual GateDriverStatus fault inputs off and on. input = 1..12 (GateDriverStatus_01..12), or 0 for all
 * twelve. A switched-OFF input is not evaluated at all: it cannot latch a fault or stop the output, whatever it reads.
 * Not remembered across a reboot (only a saved profile carries it, 2026-11-16) -- every input is on at boot (except any cleared in GDS_DEFAULT_ENABLE_MASK, ctrlr_config.h) and a reboot
 * always returns to that. Not stored in a profile. This switches a protection off: the hub announces it as DANGEROUS
 * ("interlock"); use it only for an input that is not wired or is known faulty.
 *
 *   GDS:ENAble 5 0     OK            ERR 12 not 0/1 or input outside 0-12, ERR 13 when disabling outside IDLE/FAULT
 *   GDS:ENAble? 5      OK <0|1>
 *   GDS:ENAble?        OK 01=1 02=1 ... 12=0     (all twelve)
 *
 * Switching an input OFF needs IDLE or FAULT (never while ARMED or FIRING: a protection is not removed from a
 * controller that is armed or running). Switching one ON is allowed in any state. Either way the pins are re-checked at
 * once, so turning on an input whose pin is in its fault state faults immediately (as turning DEBUG:FAULT:BYPASS off
 * does). Switching an input off does not clear a fault it already latched: FAULT:CLEar does, and the re-check inside
 * FAULT:CLEar then ignores the switched-off input.
 * -------------------------------------------------------------------------- */
void cmd_gds_enable(uart_instance_t *inst, char *args)
{
    char *tokIn = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokOn = (tokIn != NULL) ? strtok(NULL, " \r\n") : NULL;
    uint8_t input = 0U;
    uint8_t on;

    if ((tokIn == NULL) || (tokOn == NULL))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "GDS:ENAble needs two arguments: input 0|1");
        return;
    }
    if (ParseInput(inst, tokIn, 0U, "GDS:ENAble", &input) == 0U) { return; }
    if ((strcmp(tokOn, "0") != 0) && (strcmp(tokOn, "1") != 0))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "GDS:ENAble: the second argument must be 0 or 1");
        return;
    }
    on = (uint8_t)(tokOn[0] - '0');

    if ((on == 0U) && (SM_GetState() != SM_STATE_IDLE) && (SM_GetState() != SM_STATE_FAULT))
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "a GateDriverStatus input can only be switched off in IDLE or FAULT");
        return;
    }
    (void)GdsInputs_SetEnabled(input, on);
    GateDriver_CheckFault();                 /* an input just switched on may already be in fault */
    Scpi_ReplyOk(inst);
}

void cmd_gds_enable_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char buf[96];
    uint8_t input = 0U;
    int len = 0;

    if (tok != NULL)
    {
        if (ParseInput(inst, tok, 1U, "GDS:ENAble?", &input) == 0U) { return; }
        snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned)GdsInputs_IsEnabled(input));
        uart_send(inst, buf);
        return;
    }
    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, "OK");
    for (uint8_t i = 1U; i <= GDS_INPUT_COUNT; i++)
    {
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " %02u=%u", (unsigned)i, (unsigned)GdsInputs_IsEnabled(i));
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");
    uart_send(inst, buf);
}

/* --------------------------------------------------------------------------
 * GDS:NICKname <input> <name> / GDS:NICKname? [input]      (added 2026-11-13)
 *
 * A label for each GateDriverStatus input, so a fault can be traced to "PhaseU_Hi" instead of "input 3". input = 1..12.
 * name = 1-15 characters from letters, digits and _ . - + (no spaces); the single character "-" clears the nickname
 * (and is how an unset nickname is printed). Lost at reboot unless saved in a profile (a profile holds the nicknames, 2026-11-16).
 *
 *   GDS:NICKname 3 PhaseU_Hi     OK        ERR 12 input not 1-12, ERR 14 invalid nickname
 *   GDS:NICKname? 3              OK PhaseU_Hi        (OK - when none is set)
 *   GDS:NICKname?                OK 01=- 02=- 03=PhaseU_Hi ... 12=-     (all twelve)
 * -------------------------------------------------------------------------- */
void cmd_gds_nickname(uart_instance_t *inst, char *args)
{
    char *tokIn = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokName = (tokIn != NULL) ? strtok(NULL, " \r\n") : NULL;
    uint8_t input = 0U;

    if ((tokIn == NULL) || (tokName == NULL))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "GDS:NICKname needs two arguments: input name");
        return;
    }
    if (ParseInput(inst, tokIn, 1U, "GDS:NICKname", &input) == 0U) { return; }
    if (GdsInputs_SetNickname(input, tokName) == 0U)
    {
        Scpi_SendErr(inst, ERR_INVALID_NICKNAME, "Invalid nickname: 1-15 characters from letters, digits and _ . - + "
                                                 "(\"-\" clears it)");
        return;
    }
    Scpi_ReplyOk(inst);
}

void cmd_gds_nickname_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char buf[24U + (GDS_INPUT_COUNT * (GDS_NICKNAME_MAX_LEN + 5U))];
    uint8_t input = 0U;
    int len = 0;
    const char *name;

    if (tok != NULL)
    {
        if (ParseInput(inst, tok, 1U, "GDS:NICKname?", &input) == 0U) { return; }
        name = GdsInputs_Nickname(input);
        snprintf(buf, sizeof(buf), "OK %s\r\n", (name[0] == '\0') ? "-" : name);
        uart_send(inst, buf);
        return;
    }
    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, "OK");
    for (uint8_t i = 1U; i <= GDS_INPUT_COUNT; i++)
    {
        name = GdsInputs_Nickname(i);
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " %02u=%s", (unsigned)i, (name[0] == '\0') ? "-" : name);
    }
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");
    uart_send(inst, buf);
}
