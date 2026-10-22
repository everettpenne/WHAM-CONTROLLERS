/*
 * cmd_io.c -- serial command handlers: GDS?, GDS:LAST?, DIAGnostic:LEDs?, EXTernal:TRIGger*.
 * Split out of the old single commands.c (2026-09-29); the handlers are
 * unchanged. See commands.h for the file map.
 */
#include "commands.h"
#include "led_indicators.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "gate_driver.h"
#include "shot_trigger.h"
#include "board_io.h"
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
   boot>: which input(s) tripped the last GateDriverStatus fault. */
void cmd_gds_last_query(uart_instance_t *inst, char *args)
{
    char buf[48];
    (void)args;
    snprintf(buf, sizeof(buf), "OK %03X %lu\r\n", (unsigned)GateDriver_DebugBadRaw(), (unsigned long)GateDriver_DebugEdges());
    uart_send(inst, buf);
}

/* --------------------------------------------------------------------------
 * DIAGnostic:LEDs? -- OK TX=<0|1> RX=<0|1> IDLE=<0|1> ARMED=<0|1> FIRING=<0|1> FAULT=<0|1>
 *
 * Added 2026-10-07 with the front-panel LEDs (led_indicators.h): which LEDs
 * the firmware is driving lit right now (the output register, not the pin
 * voltage, so it reads the same with or without LEDs fitted). The state LEDs
 * always agree with STATE?; TX/RX are the activity LEDs, which light for
 * LED_ACTIVITY_MS after serial traffic -- this reply's own request lit RX, and
 * the previous reply lit TX, so a query sent right after another shows both
 * and one sent a moment later shows both off. All zero when
 * LED_INDICATORS_ENABLED is 0.
 * -------------------------------------------------------------------------- */
void cmd_diag_leds_query(uart_instance_t *inst, char *args)
{
    uint8_t m = LedIndicators_GetMask();
    char buf[80];
    (void)args;

    snprintf(buf, sizeof(buf), "OK TX=%u RX=%u IDLE=%u ARMED=%u FIRING=%u FAULT=%u\r\n",
             (unsigned)((m & LED_MASK_TX) != 0U),     (unsigned)((m & LED_MASK_RX) != 0U),
             (unsigned)((m & LED_MASK_IDLE) != 0U),   (unsigned)((m & LED_MASK_ARMED) != 0U),
             (unsigned)((m & LED_MASK_FIRING) != 0U), (unsigned)((m & LED_MASK_FAULT) != 0U));
    uart_send(inst, buf);
}

/* --------------------------------------------------------------------------
 * EXTernal:TRIGger <0|1> / EXTernal:TRIGger? / EXTernal:TRIGger:INPut? / EXTernal:TRIGger:STATus?
 *
 * The external shot trigger (shot_trigger.h): a rising edge on PF15 while ARMED starts the shot exactly as FIRE does,
 * from an interrupt, not by polling. Added 2026-10-20 (same command names and meaning as XREX's). ON at every boot
 * (SHOT_TRIGGER_DEFAULT_ENABLED); not stored in saved profiles, like XREX.
 *   EXTernal:TRIGger 1|0       OK
 *   EXTernal:TRIGger?          OK <0|1>
 *   EXTernal:TRIGger:INPut?    OK <0|1>   the raw level of PF15 right now, whether or not the trigger is on
 *   EXTernal:TRIGger:STATus?   OK enabled=<0|1> input=<0|1> edges=<n> started=<n> ignored_disabled=<n>
 *                              ignored_state=<n> ignored_glitch=<n> refused=<n> busy=<n> last=<result>
 * `last` is the result of the most recent attempt to start a shot: NONE (never tried), STARTED, FAULT, NOT_ARMED,
 * NO_SHOT, CONTROL:<n> (a closed-loop refusal, n = the ctl_result_t code), BUSY (a FIRE command was starting the
 * shot), CHANGED. Counters are since boot.
 * -------------------------------------------------------------------------- */
void cmd_ext_trigger(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;

    if ((tok == NULL) || ((strcmp(tok, "0") != 0) && (strcmp(tok, "1") != 0)))
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "EXTernal:TRIGger needs one argument: 0|1");
        return;
    }
    ShotTrigger_SetEnabled((uint8_t)(tok[0] - '0'));
    Scpi_ReplyOk(inst);
}

void cmd_ext_trigger_query(uart_instance_t *inst, char *args)
{
    char buf[16];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned)ShotTrigger_GetEnabled());
    uart_send(inst, buf);
}

void cmd_ext_trigger_input_query(uart_instance_t *inst, char *args)
{
    char buf[16];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned)BoardIo_ReadExtTrigger());
    uart_send(inst, buf);
}

void cmd_ext_trigger_status_query(uart_instance_t *inst, char *args)
{
    shot_trigger_status_t st;
    char last[24];
    char buf[256];
    (void)args;

    ShotTrigger_GetStatus(&st);
    if (st.attempts == 0U)
    {
        snprintf(last, sizeof(last), "NONE");
    }
    else
    {
        switch (st.last.code)
        {
            case SHOT_START_OK:         snprintf(last, sizeof(last), "STARTED"); break;
            case SHOT_START_FAULT:      snprintf(last, sizeof(last), "FAULT"); break;
            case SHOT_START_NOT_ARMED:  snprintf(last, sizeof(last), "NOT_ARMED"); break;
            case SHOT_START_NO_SHOT:    snprintf(last, sizeof(last), "NO_SHOT"); break;
            case SHOT_START_CONTROL:    snprintf(last, sizeof(last), "CONTROL:%u", (unsigned)st.last.ctl); break;
            case SHOT_START_BUSY:       snprintf(last, sizeof(last), "BUSY"); break;
            default:                    snprintf(last, sizeof(last), "CHANGED"); break;
        }
    }
    snprintf(buf, sizeof(buf),
             "OK enabled=%u input=%u edges=%lu started=%lu ignored_disabled=%lu ignored_state=%lu ignored_glitch=%lu "
             "refused=%lu busy=%lu last=%s\r\n",
             (unsigned)ShotTrigger_GetEnabled(), (unsigned)BoardIo_ReadExtTrigger(), (unsigned long)st.edges,
             (unsigned long)st.started, (unsigned long)st.ignored_disabled, (unsigned long)st.ignored_state,
             (unsigned long)st.ignored_glitch, (unsigned long)st.refused, (unsigned long)st.busy, last);
    uart_send(inst, buf);
}
