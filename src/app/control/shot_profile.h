/*
 * shot_profile.h / shot_profile.c -- a shot as a time series of setpoints, and
 * the generator that turns it into per-period table entries. Added 2026-10-01.
 *
 * The operator uploads (SHOT:BEGin / SHOT:STEP / SHOT:END) up to
 * SHOT_SETPOINT_MAX setpoints, each {time, carrier frequency, duty}:
 *   - the first setpoint must be at t = 0 -- the shot always starts at the
 *     frequency and duty of the first slot;
 *   - times strictly increase; the last setpoint's time is the shot's end
 *     (at most SHOT_MAX_DURATION_US);
 *   - between setpoints frequency and duty ramp LINEARLY in time;
 *   - every setpoint is checked against the selected supply mode's limits
 *     (supply_mode.h) when it is uploaded; a straight line between two
 *     setpoints inside the limits stays inside them.
 *
 * ShotGen_* then walks that curve one switching period at a time -- entry k
 * is the {per, cmp} for the period that starts at the time all the earlier
 * periods add up to -- and the engine (pfm.c) plays the entries back through
 * its small per-period ring table, refilling it as it goes. The shot ends at
 * the first period that starts at or after the last setpoint's time (so it
 * overshoots by less than one period, and the last setpoint's own values are
 * approached, not held).
 *
 * Units: time in microseconds; frequency in millihertz; duty in hundredths of
 * a percent (so 47.5 % is 4750). The wire takes Hz and percent with decimals
 * (Scpi_ParseScaled(), scpi_util.h).
 *
 * All integer math (64-bit where needed), so the host tool can reproduce the
 * generated table exactly: python/pfm_shot_test.py generate_profile(). Pure
 * logic, no hardware: host-tested (tests/test_shot_profile.c).
 */
#ifndef SHOT_PROFILE_H
#define SHOT_PROFILE_H

#include <stdint.h>
#include "ctrlr_config.h"

typedef struct
{
    uint32_t tUs;        /* time since the start of the shot */
    uint32_t freqMilliHz;
    uint16_t dutyHundredths;
} shot_setpoint_t;

typedef enum
{
    SHOT_APPEND_OK = 0,
    SHOT_APPEND_NO_MODE,        /* no supply mode selected */
    SHOT_APPEND_FULL,           /* SHOT_SETPOINT_MAX already stored */
    SHOT_APPEND_FIRST_NOT_ZERO, /* the first setpoint's time must be 0 */
    SHOT_APPEND_NOT_INCREASING, /* time not greater than the previous setpoint's */
    SHOT_APPEND_TOO_LONG,       /* time beyond SHOT_MAX_DURATION_US */
    SHOT_APPEND_FREQ_LOW,       /* below the mode's minimum */
    SHOT_APPEND_FREQ_HIGH,      /* above the mode's maximum */
    SHOT_APPEND_DUTY_LOW,
    SHOT_APPEND_DUTY_HIGH
} shot_append_result_t;

/* Drops every setpoint. */
void ShotProfile_Clear(void);

/* Appends a setpoint after checking it against the selected supply mode and
   the ordering rules above. Nothing is stored unless SHOT_APPEND_OK. */
shot_append_result_t ShotProfile_Append(uint32_t tUs, uint32_t freqMilliHz, uint16_t dutyHundredths);

uint16_t ShotProfile_Count(void);
/* Time of the last setpoint = the shot's length; 0 with no setpoints. */
uint32_t ShotProfile_DurationUs(void);
/* 1 and *out filled for index < ShotProfile_Count(), else 0. */
uint8_t  ShotProfile_Get(uint16_t index, shot_setpoint_t *out);
/* A usable shot: at least two setpoints (the first is t = 0 by construction). */
uint8_t  ShotProfile_IsComplete(void);

/* ---- generator ---- */

/* Starts walking the stored setpoints from t = 0. `ticksPerEntry` scales the
   time one entry advances: 1 for one entry per period, PFM_HOLD_PERIODS when
   each entry is held that many periods. Returns 0 if there is no complete
   shot or no mode is selected. */
uint8_t ShotGen_Start(uint8_t periodsPerEntry);

/* Produces the next entry. Returns 0 when the shot is over (nothing written);
   otherwise 1, with `*per`/`*cmp` set (the same compare for every channel),
   and `*last` = 1 if no entry follows this one. The values satisfy
   SupplyMode_CheckStep() by construction (clamped onto the mode's limits). */
uint8_t ShotGen_Next(uint16_t *per, uint16_t *cmp, uint8_t *last);

/* The {per, cmp} for one period at a carrier frequency (millihertz) and duty (hundredths
   of a percent), clamped onto the selected supply mode's limits exactly as the generator
   does -- the closed-loop controller (closed_loop.c) builds its steps with this. Returns 0
   (nothing written) with no mode selected or a zero frequency. */
uint8_t ShotProfile_StepFor(uint32_t freqMilliHz, uint32_t dutyHundredths, uint16_t *per, uint16_t *cmp);

#endif /* SHOT_PROFILE_H */
