/*
 * cmd_common.c -- shared pieces of the serial command layer, see
 * cmd_common.h.
 */
#include "cmd_common.h"
#include "shot_start.h"
#include "hrtim.h"
#include "gate_driver.h"
#include "pfm.h"
#include "state_machine.h"

/* True if EITHER fault source is latched: PC10/HRTIM1_FLT6 (native
   hardware, hrtim.h) or the GateDriverStatus EXTI interrupt
   (gate_driver.h, PE0..PE11). One combined answer for FAULT?/FIRE's
   ERR 6 gate/FAULT:CLEAR -- see cmd_fault_query()'s own comment for
   why these two structurally-independent mechanisms present as one
   fault state at the command layer. */
uint8_t AnyFaultLatched(void)
{
    return ShotStart_FaultLatched();
}
