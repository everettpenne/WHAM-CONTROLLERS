/*
 * cmd_io.c -- serial command handlers: DIAGnostic:LEDs?, EXTernal:TRIGger*.
 * Split out of the old single commands.c (2026-09-29); the handlers are
 * unchanged. (GDS? and GDS:LAST? moved to cmd_gds.c, 2026-11-13.) See commands.h for the file map.
 */
#include "commands.h"
#include "led_indicators.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "shot_trigger.h"
#include "shot_start.h"
#include "pfm.h"
#include "board_io.h"
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

/* EXTernal:TRIGger:LATency? -- OK edge_us=<t|NA> ring_us=<t> preload_us=<t> capture_us=<t> pwm_us=<t> start_us=<t> total_us=<t|NA>
   Where the time went in the LAST SUCCESSFUL shot start, in microseconds with one decimal (cycle counter, 170 MHz):
   edge_us    the accepted PF15 edge to ShotStart_Run() being entered (NA if the last start was a FIRE/SHOT:STARt);
   ring_us    PFM_Restart(): filling the ring;   preload_us  step 0 written and forced into the HRTIM;
   capture_us the PFM_Input capture started;     pwm_us      HRTIM1_PWM_Start() (cold-start settling, outputs connected);
   start_us   ShotStart_Run() entry to the outputs connected (everything the firmware does after the edge, except edge_us);
   total_us   edge to outputs connected. An error before the first shot. */
static void FormatTenths(char *dst, size_t n, uint32_t cycles)
{
    uint32_t t10 = (uint32_t)(((uint64_t)cycles * 10U) / 170U);
    snprintf(dst, n, "%lu.%lu", (unsigned long)(t10 / 10U), (unsigned long)(t10 % 10U));
}

void cmd_ext_trigger_latency_query(uart_instance_t *inst, char *args)
{
    uint32_t s[PFM_RESTART_STAMPS];
    uint32_t edge = 0U;
    char ring[16], pre[16], cap[16], pwm[16], start[16], e[16] = "NA", tot[16] = "NA";
    char buf[200];
    (void)args;

    if (ShotStart_StartCount() == 0U)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "no shot has been started since boot");
        return;
    }
    PFM_GetRestartStamps(s);
    FormatTenths(ring, sizeof(ring), s[1] - s[0]);
    FormatTenths(pre, sizeof(pre), s[2] - s[1]);
    FormatTenths(cap, sizeof(cap), s[3] - s[2]);
    FormatTenths(pwm, sizeof(pwm), s[4] - s[3]);
    FormatTenths(start, sizeof(start), s[4] - ShotStart_LastEntryStamp());
    if (ShotTrigger_LastEdgeToEntry(&edge) != 0U)
    {
        FormatTenths(e, sizeof(e), edge);
        FormatTenths(tot, sizeof(tot), (s[4] - ShotStart_LastEntryStamp()) + edge);
    }
    snprintf(buf, sizeof(buf), "OK edge_us=%s ring_us=%s preload_us=%s capture_us=%s pwm_us=%s start_us=%s total_us=%s\r\n",
             e, ring, pre, cap, pwm, start, tot);
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
