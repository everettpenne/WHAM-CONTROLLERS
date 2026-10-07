/*
 * supply_mode.h / supply_mode.c -- which H-bridge supply this controller is
 * currently driving, and that supply's limits. Added 2026-09-29.
 *
 * One firmware, two supplies (see ctrlr_config.h for the values):
 *   LIMITER  PWM-driven:  75-125 kHz carrier, per-phase 5-95% duty
 *   HVPS     PFM-driven:  10-30 kHz carrier, shared 40-60% duty
 * The operator selects one with CONFig:MODE. Until one is selected the mode
 * is NONE and no table can be uploaded (cmd_table.c) -- a table built for one
 * supply must never be accepted for the other by default. Every TABle:STEP
 * is then checked with SupplyMode_CheckStep(); changing the mode clears the
 * uploaded table (cmd_config.c), so a table never outlives the limits it was
 * checked against.
 *
 * Pure logic, no hardware: host-tested (tests/test_supply_mode.c).
 */
#ifndef SUPPLY_MODE_H
#define SUPPLY_MODE_H

#include <stdint.h>

typedef enum
{
    SUPPLY_MODE_NONE = 0,
    SUPPLY_MODE_LIMITER,
    SUPPLY_MODE_HVPS
} supply_mode_t;

typedef struct
{
    uint32_t freqMinHz;
    uint32_t freqMaxHz;
    uint8_t  dutyMinPct;
    uint8_t  dutyMaxPct;
} supply_limits_t;

typedef enum
{
    SUPPLY_STEP_OK = 0,
    SUPPLY_STEP_NO_MODE,     /* no mode selected */
    SUPPLY_STEP_FREQ_LOW,    /* carrier below the mode's minimum */
    SUPPLY_STEP_FREQ_HIGH,   /* carrier above the mode's maximum */
    SUPPLY_STEP_DUTY_LOW,    /* some channel's duty below the mode's minimum */
    SUPPLY_STEP_DUTY_HIGH    /* some channel's duty above the mode's maximum */
} supply_step_result_t;

/* Boot state: SUPPLY_MODE_NONE. */
supply_mode_t SupplyMode_Get(void);

/* Selects a mode (NONE, LIMITER or HVPS). Returns 1 if the mode CHANGED,
   0 if it was already selected. The caller (cmd_config.c) enforces the
   IDLE-only rule and clears the table on a change. */
uint8_t SupplyMode_Set(supply_mode_t mode);

/* "NONE", "LIMITER" or "HVPS". */
const char *SupplyMode_Name(supply_mode_t mode);

/* The limits of `mode`; returns 0 (and leaves *out untouched) for NONE. */
uint8_t SupplyMode_GetLimits(supply_mode_t mode, supply_limits_t *out);

/* Checks one table step against the current mode: the carrier implied by
   `per` (HRTIM clock / (per + 1)) and each channel's duty (cmp / per). */
supply_step_result_t SupplyMode_CheckStep(uint16_t per, const uint16_t *cmp, uint8_t channels);

#endif /* SUPPLY_MODE_H */
