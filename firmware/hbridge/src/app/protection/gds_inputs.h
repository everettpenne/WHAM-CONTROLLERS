#ifndef GDS_INPUTS_H
#define GDS_INPUTS_H

#include <stdint.h>

/* --------------------------------------------------------------------------
 * gds_inputs.h -- the 12 GateDriverStatus fault inputs as a configurable set: which polarity counts as a fault,
 * which inputs are switched on, and a nickname for each. Pure logic, no hardware (host-tested,
 * tests/test_gds_inputs.c); gate_driver.c reads the pins and asks this module which of them are faults.
 * Added 2026-11-13.
 *
 *   input numbers   1..12 = GateDriverStatus_01..12 (PE0..PE11), as in GDS?; 0 means "all" where noted.
 *   polarity        one setting for all twelve inputs. Boots at the compiled GDS_FAULT_POLARITY (ctrlr_config.h) and can be
 *                   flipped at runtime (GDS:POLarity, 2026-11-16); a reboot returns to the compiled one, a profile stores it.
 *   enabled         runtime. Every input is ON at boot except those cleared in GDS_DEFAULT_ENABLE_MASK
 *                   (ctrlr_config.h). A switched-off input is never evaluated: it cannot latch a fault or stop the
 *                   output. A reboot returns to the compiled default; only an explicit *RCL of a profile that holds a mask
 *                   (saved by *SAV, settings.c) puts it back (changed 2026-11-16: it used to be RAM only).
 *   nickname        runtime, 1..GDS_NICKNAME_MAX_LEN characters; purely a label; saved in profiles like the mask.
 *
 * The mask is written from the main loop and read from the EXTI interrupts: a single 16-bit store, volatile.
 * -------------------------------------------------------------------------- */

#define GDS_INPUT_COUNT        (12U)
#define GDS_ALL_INPUTS_MASK    (0x0FFFU)
#define GDS_NICKNAME_MAX_LEN   (15U)

/* Back to the compiled defaults: GDS_DEFAULT_ENABLE_MASK and no nicknames. (Boot state; tests use it between cases.) */
void GdsInputs_Reset(void);

/* Bit i = input i+1 is switched on. */
uint16_t GdsInputs_EnabledMask(void);
uint8_t  GdsInputs_IsEnabled(uint8_t input);

/* Switches input 1..12 (or all of them for 0) on or off. Returns 0 for any other input number, 1 otherwise. */
uint8_t  GdsInputs_SetEnabled(uint8_t input, uint8_t enabled);

/* A nickname is 1..GDS_NICKNAME_MAX_LEN characters from letters, digits and _ . - + (no spaces: replies are
   space-separated), and not "-" alone. */
uint8_t  GdsInputs_NicknameIsValid(const char *name);

/* Sets the nickname of input 1..12; the single character "-" clears it. Returns 0 (nothing changed) for a bad input
   number or an invalid nickname. */
uint8_t  GdsInputs_SetNickname(uint8_t input, const char *name);

/* The nickname, or "" when none is set or the input number is not 1..12. */
const char *GdsInputs_Nickname(uint8_t input);

/* Which of the 12 raw pins (bit 0 = input 1, a set bit = pin HIGH) are in their fault state for the given polarity,
   whether switched on or not: normallyLow != 0 means HIGH is the fault. */
uint16_t GdsInputs_RawFaultBits(uint16_t raw, uint8_t normallyLow);

/* The pins that count: in fault under the polarity now in force AND switched on. What gate_driver.c latches on. */
uint16_t GdsInputs_FaultBits(uint16_t raw);

/* 1 if the polarity in force is NORMALLY_LOW (HIGH = fault), 0 if NORMALLY_HIGH (LOW = fault). */
uint8_t  GdsInputs_PolarityIsNormallyLow(void);

/* Sets the polarity in force for all twelve inputs (nonzero = NORMALLY_LOW). The caller re-checks the pins afterwards. */
void     GdsInputs_SetPolarityNormallyLow(uint8_t normallyLow);

/* The polarity this firmware was built with (what a reboot returns to), same encoding. */
uint8_t  GdsInputs_DefaultPolarityIsNormallyLow(void);

#endif /* GDS_INPUTS_H */
