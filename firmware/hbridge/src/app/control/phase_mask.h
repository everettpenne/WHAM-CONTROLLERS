/*
 * phase_mask.h / phase_mask.c -- which output phases (U, V, W) are enabled, as a bit mask, and the
 * text form an operator types: PHASe:ENAble / PHASe:SELect (cmd_phase.c). Added 2026-10-05.
 * Pure logic: host-tested (tests/test_phase_mask.c).
 *
 * Bit k of a mask is HRTIM channel k, i.e. phase U, V, W (and X, Y, Z for builds with more
 * channels: HRTIM_NUM_CHANNELS, ctrlr_config.h). A phase list is typed as letters in any order
 * and case ("UW", "w", "wu"), as channel numbers ("13", "2"), or one of the words ALL / NONE;
 * commas are ignored. Letters and digits are not mixed in one list.
 */
#ifndef PHASE_MASK_H
#define PHASE_MASK_H

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    PHASE_PARSE_OK = 0,
    PHASE_PARSE_EMPTY,      /* no text, or only separators */
    PHASE_PARSE_BAD         /* a character that is not a phase of this build, mixed letters/digits, NONE where not allowed */
} phase_parse_t;

/* The letter of channel k: 'U', 'V', 'W', 'X', 'Y', 'Z'; '?' beyond that. */
char PhaseMask_Letter(uint8_t channel);

/* All phases of a `phaseCount`-phase build: bits 0..phaseCount-1. */
uint8_t PhaseMask_All(uint8_t phaseCount);

/* Parses a phase list into *mask. `allowNone` accepts the word NONE (an empty mask); otherwise it is BAD. */
phase_parse_t PhaseMask_Parse(const char *text, uint8_t phaseCount, uint8_t allowNone, uint8_t *mask);

/* The enabled phases as letters in order ("UW"), or "NONE" for an empty mask. */
void PhaseMask_Format(uint8_t mask, uint8_t phaseCount, char *out, size_t n);

#endif /* PHASE_MASK_H */
