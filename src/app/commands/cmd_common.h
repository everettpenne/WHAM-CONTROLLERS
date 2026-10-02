/*
 * cmd_common.h -- shared pieces of the serial command layer: the error
 * codes of the wire protocol and the reply helper.
 *
 * Replies are "OK\r\n", "OK <value>\r\n" or "ERR <n> <msg>\r\n". The
 * numbers below are a frozen wire contract (docs/command_reference.md,
 * "Error codes"): never renumber or reuse one.
 */
#ifndef CMD_COMMON_H
#define CMD_COMMON_H

#include <stdint.h>
#include "uart.h"

typedef enum
{
    ERR_UNKNOWN_COMMAND     = 1,   /* no command_table[] pattern matched */
    ERR_TABLE_NOT_UPLOADING = 2,   /* TABle:STEP/END without TABle:BEGin */
    ERR_TABLE_FULL          = 3,   /* PFM_TABLE_SIZE table entries or SHOT_SETPOINT_MAX setpoints already stored */
    ERR_TABLE_STEP_INVALID  = 4,   /* TABle:STEP: wrong count (1 + HRTIM_NUM_CHANNELS)
                                      or a value outside uint16 */
    ERR_TABLE_EMPTY         = 5,   /* FIRE with nothing uploaded */
    ERR_FAULT_LATCHED       = 6,   /* PC10/HRTIM1_FLT6 or GateDriverStatus latch --
                                      FAULT:CLEar first */
    ERR_QSPI_FAILED         = 7,   /* QSPI:ID? command failed or timed out */
    ERR_PFMIN_BAD_CHANNEL   = 8,   /* PFM_Input channel not 1-6 */
    ERR_PFMIN_BAD_COUNT     = 9,   /* PFMIN:CAPTURE M out of 1-PFM_INPUT_MAX_PERIODS */
    ERR_OUT_OF_MODE_LIMITS  = 10,  /* TABle:STEP carrier or duty outside the selected supply mode's
                                      limits (was ERR_CARRIER_TOO_HIGH: carrier above 100 kHz) */
    ERR_INVALID_CHANNEL     = 11,  /* PID:/SOURce:/LOG: loop number other than 1 (XREX: channel not 1-N) */
    /* 14, 15, 16: used by WHAM-XREX-PFMG474 only (nickname, external enable) --
       reserved so the shared numbers mean the same thing on both projects. */
    ERR_INVALID_ARGS        = 12,  /* wrong argument count, or non-numeric/out-of-range */
    ERR_INVALID_STATE       = 13,  /* not allowed in the current state */
    ERR_FWUPDATE            = 17   /* FWUPdate:* flash/CRC/image/option-byte failure */
} cmd_err_t;

/* Send "ERR <code> <msg>\r\n". code is an int, not cmd_err_t, so every call
   site compiles exactly as it did with literal numbers. */
void SendErr(uart_instance_t *inst, int code, const char *msg);

/* True if any fault source (PC10/HRTIM1_FLT6, GateDriverStatus, or a PFM table
   underrun) is latched -- FAULT? and FIRE's ERR 6 gate. */
uint8_t AnyFaultLatched(void);

#endif /* CMD_COMMON_H */
