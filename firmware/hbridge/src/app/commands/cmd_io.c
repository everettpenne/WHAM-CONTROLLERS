/*
 * cmd_io.c -- serial command handlers: DIAGnostic:LEDs?, EXTernal:TRIGger*, SPARKgap:INPut?.
 * Split out of the old single commands.c (2026-09-29); the handlers are
 * unchanged. (GDS? and GDS:LAST? moved to cmd_gds.c, 2026-11-13.) See commands.h for the file map.
 */
#include "commands.h"
#include "led_indicators.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "shot_trigger.h"
#include "board_io.h"
#include "spark_gap.h"
#include <stdio.h>
#include <string.h>

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

/* SPARKgap:INPut? -- OK <0|1>: the raw level of the spark-gap input PF13 right now, 1 = HIGH (healthy), 0 = LOW (a spark-gap
   fault; whether or not the DEBUG:FAULT:BYPASS or a latch is in the way). FAULT:SOURces? shows SPARK=1 once one has latched. */
void cmd_spark_gap_input_query(uart_instance_t *inst, char *args)
{
    char buf[16];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned)SparkGap_Read());
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
