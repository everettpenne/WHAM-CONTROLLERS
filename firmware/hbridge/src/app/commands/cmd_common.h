/*
 * cmd_common.h -- pieces of the serial command layer every handler file uses: it pulls in the protocol's error
 * codes and reply/argument helpers (middleware/scpi: scpi_err.h, scpi_util.h -- shared with XREX, see
 * docs/scpi_conventions.md) and holds what is specific to this product's fault logic.
 */
#ifndef CMD_COMMON_H
#define CMD_COMMON_H

#include <stdint.h>
#include "uart.h"
#include "scpi_err.h"
#include "scpi_util.h"

/* The numeric error codes (ERR <code> <msg>) are src/middleware/scpi/scpi_err.h, shared with XREX and
   documented in docs/scpi_conventions.md; the reply and argument helpers are scpi_util.h. */

/* True if any fault source (PC10/HRTIM1_FLT6, GateDriverStatus, or a PFM table
   underrun) is latched -- FAULT? and FIRE's ERR 6 gate. */
uint8_t AnyFaultLatched(void);

#endif /* CMD_COMMON_H */
