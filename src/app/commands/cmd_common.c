/*
 * cmd_common.c -- shared pieces of the serial command layer, see
 * cmd_common.h.
 */
#include "cmd_common.h"
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
    return (HRTIM1_FaultIsTripped() != 0U) || (GateDriver_FaultIsLatched() != 0U) || (PFM_UnderrunLatched() != 0U) ||
           ((SM_GetFaultSources() & (SM_FAULT_SRC_OVERVOLT | SM_FAULT_SRC_FEEDBACK)) != 0U);
}
