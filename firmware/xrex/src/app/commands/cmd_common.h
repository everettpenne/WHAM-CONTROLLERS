/*
 * cmd_common.h -- pieces of the serial command layer every handler file uses: it pulls in the protocol's error
 * codes and reply/argument helpers (middleware/scpi: scpi_err.h, scpi_util.h -- shared with HBRIDGE, see
 * docs/scpi_conventions.md) and holds what is specific to this product's fault and interlock logic.
 */
#ifndef CMD_COMMON_H
#define CMD_COMMON_H

#include <stdint.h>
#include "uart.h"
#include "scpi_err.h"
#include "scpi_util.h"

/* The numeric error codes (ERR <code> <msg>) are src/middleware/scpi/scpi_err.h, shared with HBRIDGE and
   documented in docs/scpi_conventions.md; the reply and argument helpers are scpi_util.h. */

/* True if either fault source (PC10/HRTIM1_FLT6 or GateDriverStatus) is
   latched -- FAULT? and FIRE's ERR 6 gate. */
uint8_t AnyFaultLatched(void);

/* Fire-attempt precondition shared by FIRE, SOURce:RUN and SHOT:STARt:
   ARMED and the external-enable interlock satisfied. Returns 1 to proceed;
   0 means an ERR has already been sent. */
uint8_t RequireArmedAndEnabled(uart_instance_t *inst);

#endif /* CMD_COMMON_H */
