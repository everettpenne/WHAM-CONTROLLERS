/*
 * target_profile.h / target_profile.c -- the TARGET voltage of a closed-loop shot as a time
 * series of setpoints. Added 2026-10-02. Pure logic: host-tested (tests/test_target_profile.c).
 *
 * The closed-loop counterpart of shot_profile.h, deliberately separate: the open-loop shot
 * (frequency and duty over time) and the closed-loop target (volts over time) are different
 * things an operator may use on their own (decision 2026-10-02). Same rules as the shot:
 * the first setpoint is at t = 0, times strictly increase, the last time is the end of the
 * shot (at most SHOT_MAX_DURATION_US), linear between setpoints; at least two setpoints.
 *
 * Voltages are in millivolts, signed (the output can be bipolar), and each setpoint is
 * checked against a limit the caller passes (the controller's maximum voltage), so a target
 * the loop could never be allowed to reach is rejected at upload.
 *
 * Since 2026-10-13 every channel (phase U, V, W) has its own target voltage: the time grid is
 * shared, each setpoint carries one voltage per channel (a setpoint given ONE voltage gives every
 * channel that voltage). Each closed loop evaluates its own channel.
 */
#ifndef TARGET_PROFILE_H
#define TARGET_PROFILE_H

#include <stdint.h>
#include "ctrlr_config.h"

typedef struct
{
    uint32_t tUs;
    int32_t  milliVolts[HRTIM_NUM_CHANNELS];     /* one target per channel: U, V, W */
} target_setpoint_t;

typedef enum
{
    TARGET_APPEND_OK = 0,
    TARGET_APPEND_FULL,
    TARGET_APPEND_FIRST_NOT_ZERO,
    TARGET_APPEND_NOT_INCREASING,
    TARGET_APPEND_TOO_LONG,
    TARGET_APPEND_TOO_HIGH        /* |voltage| above the limit passed in */
} target_append_result_t;

void Target_Clear(void);

/* Appends a setpoint; nothing is stored unless TARGET_APPEND_OK. `maxAbsMilliVolts` is the
   largest |voltage| accepted, per channel. */
target_append_result_t Target_Append(uint32_t tUs, const int32_t milliVolts[HRTIM_NUM_CHANNELS],
                                     int32_t maxAbsMilliVolts);
/* The same with one voltage for every channel (the two-value TARGet:STEP). */
target_append_result_t Target_AppendUniform(uint32_t tUs, int32_t milliVolts, int32_t maxAbsMilliVolts);
/* After a TARGET_APPEND_TOO_HIGH: the channel whose voltage was refused. */
uint8_t Target_BadChannel(void);
/* 1 if every setpoint has the same voltage on every channel (saved profiles keep the older, smaller format then). */
uint8_t Target_IsUniform(void);

uint16_t Target_Count(void);
int32_t  Target_MinMilliVolts(uint8_t ch);         /* the lowest setpoint of a channel; 0 with none */
uint32_t Target_DurationUs(void);                  /* time of the last setpoint; 0 with none */
uint8_t  Target_Get(uint16_t index, target_setpoint_t *out);
uint8_t  Target_IsComplete(void);                  /* at least two setpoints */

/* Starts evaluation from t = 0. Target_ValueAt() is meant to be called with non-decreasing times
   (it keeps a segment cursor shared by the channels, so each call is O(1); an earlier time simply
   searches again from the start). */
void Target_Rewind(void);

/* The target of channel `ch` in volts at `tUs` microseconds into the shot: linear between setpoints,
   the last value beyond the end. 0 with no setpoints. */
float Target_ValueAt(uint8_t ch, uint32_t tUs);

/* Parses a signed decimal such as "1.25", "-0.5" or "12" into millivolts (up to 3 decimals).
   Returns 1 on success, 0 on anything else. */
uint8_t Target_ParseMilliVolts(const char *text, int32_t *out);

#endif /* TARGET_PROFILE_H */
