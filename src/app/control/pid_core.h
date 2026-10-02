/*
 * pid_core.h / pid_core.c -- the PID arithmetic of the closed-loop controller, and
 * nothing else. Added 2026-10-02. Pure logic, no hardware: host-tested
 * (tests/test_pid_core.c) against simulated plants.
 *
 * One controller = one output. Everything it needs (setpoint, measurement, a
 * feedforward base) is passed to Pid_Update() each tick, so the same code serves
 * the HVPS loop (output in Hz) and the Limiter loop (output in percent duty); the
 * caller (closed_loop.c) owns units, limits and polarity.
 *
 *   output = base + kp*e + I + D          e = setpoint - measurement
 *     I  integrates ki*e*dt
 *     D  = -kd * d(measurement)/dt, taken on the MEASUREMENT (not the error) so a
 *        step in the setpoint does not kick the output, then low-pass filtered
 *   the result is clamped to [outMin, outMax] and limited to outSlew change per tick.
 *
 * Anti-windup: at the hard output clamp the integrator is back-calculated to the value
 * that yields the clamped output, so it never winds past what the actuator can do and
 * recovers at once when the error reverses; at the slew limit the tick's accumulation is
 * undone (conditional integration), because back-calculating there would stall the ramp. (XREX found a real windup bug on
 * the bench, docs/changelog.txt; this is built to not have it.)
 *
 * The time step is fixed (the caller ticks at a fixed rate), which is why dt is part
 * of the state.
 */
#ifndef PID_CORE_H
#define PID_CORE_H

#include <stdint.h>

typedef struct
{
    /* configuration */
    float kp;
    float ki;            /* per second */
    float kd;            /* seconds */
    float dt;            /* seconds per tick, > 0 */
    float outMin;
    float outMax;
    float outSlew;       /* largest change of the output per tick; 0 = unlimited */
    float dFilter;       /* derivative low-pass coefficient 0..1 (1 = none, small = heavy); default 0.2 */

    /* state */
    float integ;
    float prevPv;
    float dState;
    float lastOut;
    uint8_t havePrev;
} pid_core_t;

/* Zeroes the state and sets the configuration to a safe, inert default: all gains 0,
   no slew limit, dFilter 0.2, the given dt and output range. */
void Pid_Init(pid_core_t *p, float dt, float outMin, float outMax);

/* Clears the state (integrator, derivative history) and starts the output at
   `startOut`, as if it had just been applied. Gains and limits are kept. */
void Pid_Reset(pid_core_t *p, float startOut);

/* One tick. `base` is the feedforward the correction rides on. Returns the output to
   apply (already clamped and slew-limited). */
float Pid_Update(pid_core_t *p, float setpoint, float measurement, float base);

#endif /* PID_CORE_H */
