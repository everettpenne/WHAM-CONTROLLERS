/*
 * gate_driver.c
 *
 * See gate_driver.h for scope. GPIOE clock enable + pin/EXTI/NVIC
 * config (input, interrupt on both edges, matching the sibling
 * PFM-STM32G474 project's gpio.c precedent for these same 12 pins as
 * far as the plain-input part goes) lives in BoardIo_Init() (board_io.h),
 * called from main.c's MX_GPIO_Init() -- this file is just the read and
 * the fault evaluation/latch.
 */

#include "gate_driver.h"
#include "ctrlr_config.h"
#include "hrtim.h"
#include "pfm.h"
#include "board_io.h"

/* GateDriverStatus_01..12 = bits 0..11 of BoardIo_ReadGateDriverStatus(). */
#define GDS_PIN_MASK   (0x0FFFU)

/* Set only from GateDriver_CheckFault() (ISR context, EXTI0..EXTI4,
   EXTI9_5, EXTI15_10 -- see stm32g4xx_it.c), cleared only from
   GateDriver_FaultClear() (main-loop context, state_machine.c's SM_ClearFault(), from
   FAULT:CLEAR). volatile: written from ISR, read from the main loop
   via GateDriver_FaultIsLatched(). */
static volatile uint8_t g_gdsFaultLatched = 0U;
static volatile uint16_t g_dbgBadRaw = 0U;     /* the raw pins when the fault last latched (GDS:LAST?) */
static volatile uint32_t g_dbgEdges = 0U;      /* GateDriver_CheckFault() calls since boot (GDS:LAST?) */
uint16_t GateDriver_DebugBadRaw(void) { return g_dbgBadRaw; }
uint32_t GateDriver_DebugEdges(void) { return g_dbgEdges; }

uint16_t GateDriver_Read(void)
{
    return BoardIo_ReadGateDriverStatus();
}

void GateDriver_CheckFault(void)
{
    uint16_t raw = GateDriver_Read();
    uint16_t badBits;

    g_dbgEdges++;

    /* See ctrlr_config.h's GDS_FAULT_POLARITY comment -- exactly the
       "bitwise read PE0..PE11, fault on HIGH if normally-low / fault
       on LOW if normally-high" logic requested when this was added. */
    if (GDS_FAULT_POLARITY == GDS_NORMALLY_LOW)
    {
        badBits = raw & GDS_PIN_MASK;
    }
    else
    {
        badBits = (uint16_t)(~raw) & GDS_PIN_MASK;
    }

    if (badBits != 0U)
    {
        g_dbgBadRaw = raw;
        PFM_ForceStop();
        g_gdsFaultLatched = 1U;
    }
}

uint8_t GateDriver_FaultIsLatched(void)
{
    return g_gdsFaultLatched;
}

void GateDriver_FaultClear(void)
{
    PFM_ForceStop();
    g_gdsFaultLatched = 0U;

    /* Re-validate immediately, rather than trusting the clear to mean
       the physical condition is actually gone. This matters because
       GateDriver_CheckFault() is only ever invoked by an EXTI EDGE
       (or the one explicit boot-time call in main.c) -- unlike
       PC10/HRTIM1_FLT6's own hardware fault latch, which re-trips on
       its own if its condition is still physically present, a
       cleared-but-still-bad GateDriverStatus pin produces no new edge
       by itself and would otherwise sit silently unlatched (FAULT?
       reporting OK 0) until something eventually toggled it again.
       Calling CheckFault() here closes that gap: if any pin is still
       in its fault state, this immediately re-latches (and re-calls
       PFM_ForceStop(), a harmless repeat) before FAULT:CLEAR's OK
       reply is even sent -- an operator can never clear away a fault
       that is still physically present. */
    GateDriver_CheckFault();
}
