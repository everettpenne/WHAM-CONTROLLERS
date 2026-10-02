/*
 * sim_plant.h / sim_plant.c -- a simulated supply for testing the closed loop with nothing wired.
 * Added 2026-10-02. Pure logic, host-tested through the closed-loop tests and on the board.
 *
 * Takes the {per, cmp} the controller commands and returns the output voltage a supply would
 * produce, a first-order lag behind the steady-state value:
 *   LIMITER  V = gain * vscale * (2D - 1)                   D = cmp / per
 *   HVPS     V = gain * vpeak / (1 + ((f - resonance) / width)^2)     f = clock / (per + 1)
 * The model is separate from the controller's own feedforward model on purpose: set them
 * differently to test the loop against a plant it does not know exactly. `gain` is the
 * disturbance knob (a supply that loses or gains output mid-shot).
 */
#ifndef SIM_PLANT_H
#define SIM_PLANT_H

#include <stdint.h>

typedef struct
{
    float tau;          /* seconds */
    float gain;         /* multiplies the steady-state voltage */
    float resonance;    /* HVPS, Hz */
    float width;        /* HVPS, Hz: the curve is half its peak this far from resonance */
    float vpeak;        /* HVPS, volts at resonance */
    float vscale;       /* LIMITER, volts at |2D - 1| = 1 */
    float v;            /* the output voltage now */
} sim_plant_t;

void SimPlant_Init(sim_plant_t *p);
void SimPlant_Reset(sim_plant_t *p, float volts);

/* One step of `dt` seconds with the step {per, cmp} applied; `limiter` selects the model.
   Returns the output voltage. */
float SimPlant_Step(sim_plant_t *p, uint8_t limiter, uint16_t per, uint16_t cmp, float dt);

#endif /* SIM_PLANT_H */
