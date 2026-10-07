/*
 * cmd_state.c -- serial command handlers: FAULT?/FAULT:CLEar, ARM/DISARM/
 * STATE?, GENERAL:TEST:FAULT. The states and fault policy are
 * state_machine.h. See commands.h for the file map.
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "state_machine.h"
#include "pfm.h"
#include <stdio.h>

/* --------------------------------------------------------------------------
 * FAULT? / FAULT:CLEar
 *
 * FAULT? reports 1 while the controller is in FAULT or either hardware
 * fault source (PC10/HRTIM1_FLT6, GateDriverStatus) is latched. FAULT:CLEar
 * clears both sources, which re-check their physical inputs, and returns
 * the state to IDLE if both are clear (SM_ClearFault()). The reply is "OK"
 * either way -- a condition that is still present re-latches before this
 * returns; check FAULT?/STATE? afterwards for the result. Clearing never
 * restarts the output: that needs ARM and FIRE again.
 * -------------------------------------------------------------------------- */
void cmd_fault_query(uart_instance_t *inst, char *args)
{
    (void)args;
    uart_send(inst, ((SM_GetState() == SM_STATE_FAULT) || (AnyFaultLatched() != 0U))
                        ? "OK 1\r\n" : "OK 0\r\n");
}

/* FAULT:SOURces? -- OK PC10=<0|1> GATEDRV=<0|1> TABLE=<0|1> TEST=<0|1>: which sources
   hold a fault latched (all of them show as the single FAULT GENERAL state). TABLE is
   the per-period table running empty before the shot was over (an underrun). */
void cmd_fault_sources_query(uart_instance_t *inst, char *args)
{
    char buf[96];
    uint8_t m = SM_GetFaultSources();
    (void)args;

    snprintf(buf, sizeof(buf), "OK PC10=%u GATEDRV=%u TABLE=%u TEST=%u OVP=%u FB=%u\r\n",
             (unsigned)((m & SM_FAULT_SRC_PC10) != 0U), (unsigned)((m & SM_FAULT_SRC_GATEDRV) != 0U),
             (unsigned)((m & SM_FAULT_SRC_UNDERRUN) != 0U), (unsigned)((m & SM_FAULT_SRC_TEST) != 0U),
             (unsigned)((m & SM_FAULT_SRC_OVERVOLT) != 0U), (unsigned)((m & SM_FAULT_SRC_FEEDBACK) != 0U));
    uart_send(inst, buf);
}

void cmd_fault_clear(uart_instance_t *inst, char *args)
{
    (void)args;
    (void)SM_ClearFault();
    Scpi_ReplyOk(inst);
}

/* --------------------------------------------------------------------------
 * ARM / DISARM / STATE?
 *
 * ARM: IDLE -> ARMED. ERR 6 if a fault is latched, ERR 13 if not IDLE.
 * DISARM: ARMED -> IDLE; "OK" (a harmless no-op) in any other state.
 * STATE?: OK IDLE|ARMED|FIRING, or OK FAULT GENERAL.
 * -------------------------------------------------------------------------- */
void cmd_arm(uart_instance_t *inst, char *args)
{
    (void)args;

    if ((SM_GetState() == SM_STATE_FAULT) || (AnyFaultLatched() != 0U))
    {
        Scpi_SendErr(inst, ERR_FAULT_LATCHED, "Fault latched -- send FAULT:CLEAR first");
        return;
    }
    if (SM_GetState() != SM_STATE_IDLE)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "Can't ARM -- not currently IDLE");
        return;
    }
    if (PFM_GetPhaseMask() == 0U)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "Can't ARM -- no phase is enabled (PHASe:ENAble U|V|W|ALL 1)");
        return;
    }
    if (SM_Arm() == 0U)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "Can't ARM -- arm conditions not met");   /* narrow race only */
        return;
    }
    Scpi_ReplyOk(inst);
}

void cmd_disarm(uart_instance_t *inst, char *args)
{
    (void)args;
    SM_Disarm();
    Scpi_ReplyOk(inst);
}

void cmd_state_query(uart_instance_t *inst, char *args)
{
    (void)args;

    switch (SM_GetState())
    {
        case SM_STATE_IDLE:   uart_send(inst, "OK IDLE\r\n");   break;
        case SM_STATE_ARMED:  uart_send(inst, "OK ARMED\r\n");  break;
        case SM_STATE_FIRING: uart_send(inst, "OK FIRING\r\n"); break;
        case SM_STATE_FAULT:  uart_send(inst, "OK FAULT GENERAL\r\n"); break;
        default:              uart_send(inst, "OK UNKNOWN\r\n"); break;
    }
}

/* --------------------------------------------------------------------------
 * GENERAL:TEST:FAULT
 *
 * Software fault injection for bench-testing the fault path end to end
 * (same name as WHAM-XREX-PFMG474's): enters FAULT from any state, force-
 * stopping the output exactly as a real fault would. Cleared by
 * FAULT:CLEar like any other fault. It can only stop output, never start
 * it.
 * -------------------------------------------------------------------------- */
void cmd_general_test_fault(uart_instance_t *inst, char *args)
{
    (void)args;
    SM_ReportGeneralFault();
    Scpi_ReplyOk(inst);
}
