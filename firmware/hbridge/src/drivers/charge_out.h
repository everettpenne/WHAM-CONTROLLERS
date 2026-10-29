/*
 * charge_out.h -- the CHARGE output: one 50 % square wave on PG0 ("GPOut_01") that drives a frequency-to-voltage
 * converter charging the capacitor bank. Hardware interface; the policy (what frequency, what range) is
 * src/app/control/charge.h.
 *
 * Independent of everything else the controller does: its own timer (TIM20, channel 1 complementary output, PG0
 * alternate function 2), no relation to the HRTIM, the PFM_Input capture timers or the state machine. Nothing in this
 * interface blocks or raises an interrupt.
 *
 * Off means the pin is a plain GPIO output driven LOW and the timer is stopped, never a floating pin and never a
 * pin held at the timer's last level. Until ChargeOut_Init() has run (reset, and the first moments after power-on)
 * the pin is whatever the chip's reset state is (a floating input): the converter's input needs a pull-down.
 */
#ifndef CHARGE_OUT_H
#define CHARGE_OUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pin as an output driven LOW, timer clock on, timer configured and stopped. Once, at start-up. */
void ChargeOut_Init(void);

/* The clock the timer counts before its prescaler, in Hz. */
uint32_t ChargeOut_ClockHz(void);

/* Runs the output, or re-times it while it runs. The counter counts 0..arr at timer_clock/(psc+1); the output is HIGH for
   the first `ccr` counts and LOW for the rest. Re-timing restarts the cycle (one short or long cycle at the change). */
void ChargeOut_Start(uint32_t psc, uint32_t arr, uint32_t ccr);

/* Pin LOW at once, then the timer stopped. */
void ChargeOut_Stop(void);

#ifdef __cplusplus
}
#endif

#endif /* CHARGE_OUT_H */
