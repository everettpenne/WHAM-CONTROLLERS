#ifndef SPARK_GAP_H
#define SPARK_GAP_H

#include <stdint.h>

/* --------------------------------------------------------------------------
 * spark_gap.h -- the spark-gap fault input, PF13 ("GPInput_12", docs/pin_mapping_v4.csv). Added 2026-11-16.
 *
 * Normally HIGH. A LOW reading is a SPARK GAP FAULT: the output is stopped at once, from the pin's own EXTI interrupt
 * (falling edge, line 13, the EXTI15_10 vector shared with the gate-driver inputs PE10/PE11 and the trigger PF15), and
 * the fault is latched until FAULT:CLEar -- the same mechanism, and the same unconditional stop, as a GateDriverStatus
 * fault (gate_driver.h). It shows as the single FAULT GENERAL state with SPARK=1 in FAULT:SOURces?, which is how a
 * spark-gap fault is told from the other sources.
 *
 * DEBUG:FAULT:BYPASS (state_machine.h) bypasses it like the other external fault inputs: while on, a LOW pin neither
 * stops the output nor latches; FAULT? and FAULT:SOURces? still report the true pin. Turning the bypass off re-checks
 * the pin at once (EXTI only sees an edge, so a pin that stayed LOW would otherwise never be noticed).
 *
 * The pin has NO internal pull resistor: the external circuit must hold it HIGH. An UNWIRED input floats and can read LOW, so
 * a board with nothing on PF13 can latch a spark-gap fault; switch the input off with SPARKgap:ENAble 0 (like GDS:ENAble:
 * RAM only, IDLE or FAULT only, DANGEROUS to the hub, restored by *RCL from a profile that stored it).
 *
 * Hardware-independent logic over board_io.h; host-tested (tests/test_spark_gap.c).
 * -------------------------------------------------------------------------- */

/* Back to the boot state: switched on (SPARK_GAP_DEFAULT_ENABLED, ctrlr_config.h), nothing latched is touched. */
void SparkGap_Reset(void);

/* The input's enable switch: while off it is never evaluated -- it cannot stop the output or latch a fault, whatever the pin
   reads. Switching it off does not clear a fault it already latched (FAULT:CLEar does, and its re-check then ignores it). */
uint8_t SparkGap_IsEnabled(void);
void    SparkGap_SetEnabled(uint8_t enabled);

/* The raw level now: 1 = HIGH (healthy), 0 = LOW (a fault); read whether or not the input is enabled. */
uint8_t SparkGap_Read(void);

/* From the EXTI interrupt, at boot, after FAULT:CLEar, when the bypass is turned off and when the input is switched on: if
   the input is enabled, the pin reads LOW and the bypass is off, stops the output (PFM_ForceStop) and latches. */
void SparkGap_CheckFault(void);

uint8_t SparkGap_FaultIsLatched(void);

/* Clears the latch, then re-checks the pin: a fault that is still physically present re-latches before this returns. */
void SparkGap_FaultClear(void);

#endif /* SPARK_GAP_H */
