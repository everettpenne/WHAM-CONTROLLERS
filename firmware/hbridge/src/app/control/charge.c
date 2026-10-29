/*
 * charge.c -- the CHARGE output's policy: turn a frequency into timer settings, remember the setpoint. See charge.h.
 * No state-machine, fault or HRTIM dependency, by design.
 */
#include "charge.h"
#include "charge_out.h"
#include "ctrlr_config.h"

#define TIMER_RANGE   (65536UL)     /* a 16-bit counter's period count, and its prescaler's divide ceiling */

static uint32_t s_setHz;
static uint8_t  s_running;
static uint32_t s_psc, s_arr, s_ccr, s_actualMhz;

uint8_t Charge_Plan(uint32_t clock_hz, uint32_t hz, uint32_t *psc, uint32_t *arr, uint32_t *ccr)
{
    uint32_t ticks, divide, per;

    if ((hz < CHARGE_MIN_HZ) || (hz > CHARGE_MAX_HZ) || (clock_hz < (hz * 2UL)))
    {
        return 0U;
    }
    ticks = (clock_hz + (hz / 2UL)) / hz;               /* counts per output period, rounded */
    divide = ((ticks - 1UL) / TIMER_RANGE) + 1UL;       /* the smallest prescaler divide that fits one period in 16 bits */
    if (divide > TIMER_RANGE)
    {
        return 0U;
    }
    per = (ticks + (divide / 2UL)) / divide;            /* counter period, 2..65536, rounded */
    if ((per < 2UL) || (per > TIMER_RANGE))
    {
        return 0U;
    }
    *psc = divide - 1UL;
    *arr = per - 1UL;
    *ccr = per / 2UL;                                   /* 50 %; an odd period is one half-count off */
    return 1U;
}

void Charge_Init(void)
{
    ChargeOut_Init();
    s_setHz = 0U;
    s_running = 0U;
    s_psc = s_arr = s_ccr = s_actualMhz = 0U;
}

uint8_t Charge_SetFrequency(uint32_t hz)
{
    uint32_t psc, arr, ccr, clock;

    if (hz == 0U)
    {
        ChargeOut_Stop();
        s_setHz = 0U;
        s_running = 0U;
        s_psc = s_arr = s_ccr = s_actualMhz = 0U;
        return 1U;
    }
    clock = ChargeOut_ClockHz();
    if (Charge_Plan(clock, hz, &psc, &arr, &ccr) == 0U)
    {
        return 0U;
    }
    ChargeOut_Start(psc, arr, ccr);
    s_setHz = hz;
    s_running = 1U;
    s_psc = psc;
    s_arr = arr;
    s_ccr = ccr;
    {
        uint64_t denom = ((uint64_t)psc + 1ULL) * ((uint64_t)arr + 1ULL);
        s_actualMhz = (uint32_t)((((uint64_t)clock * 1000ULL) + (denom / 2ULL)) / denom);      /* rounded */
    }
    return 1U;
}

uint32_t Charge_GetFrequency(void)
{
    return s_setHz;
}

void Charge_GetStatus(charge_status_t *out)
{
    out->set_hz = s_setHz;
    out->running = s_running;
    out->psc = s_psc;
    out->arr = s_arr;
    out->ccr = s_ccr;
    out->actual_mhz = s_actualMhz;
}
