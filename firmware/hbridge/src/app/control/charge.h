/*
 * charge.h -- the CHARGE output (CHARGE:FREQuency): a 50 % square wave on PG0 whose frequency the operator sets, 0 = off.
 * It drives the frequency-to-voltage converter that charges the capacitor bank (hardware: drivers/charge_out.h).
 *
 * Deliberately independent of the controller's state: it can be switched on and off in IDLE, ARMED, FIRING and FAULT
 * alike (the operator is expected not to turn it on during FIRING, but nothing stops them), a latched fault does NOT
 * turn it off, and DISARM, a shot ending and FAULT:CLEar leave it alone. Only a reset (power, the E-stop, a firmware
 * swap) turns it off: it is OFF at every boot and is not stored in saved profiles (*SAV).
 *
 * Range: 0 (off) or CHARGE_MIN_HZ..CHARGE_MAX_HZ (ctrlr_config.h), whole hertz. The timer divides its clock to the
 * nearest achievable period: the status shows the frequency actually produced (within about 0.03 % at 100 kHz).
 */
#ifndef CHARGE_H
#define CHARGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t set_hz;        /* what was asked for; 0 = off */
    uint8_t  running;       /* 1 while the output is being driven */
    uint32_t psc;           /* timer prescaler as programmed (0 when off) */
    uint32_t arr;           /* timer auto-reload as programmed (0 when off) */
    uint32_t ccr;           /* the HIGH count (0 when off) */
    uint32_t actual_mhz;    /* the frequency the timer produces, in millihertz (0 when off) */
} charge_status_t;

/* The timer settings for `hz` at `clock_hz`. Returns 0 if hz is not 1..CHARGE_MAX_HZ or the clock cannot make it.
   Exposed for the host test. */
uint8_t Charge_Plan(uint32_t clock_hz, uint32_t hz, uint32_t *psc, uint32_t *arr, uint32_t *ccr);

void Charge_Init(void);                       /* output off. Once, from App_Init() */

/* 0 = off, CHARGE_MIN_HZ..CHARGE_MAX_HZ = run. Returns 1 on success, 0 if the value is out of range (nothing changed). */
uint8_t Charge_SetFrequency(uint32_t hz);

uint32_t Charge_GetFrequency(void);           /* the setpoint, 0 = off */
void     Charge_GetStatus(charge_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CHARGE_H */
