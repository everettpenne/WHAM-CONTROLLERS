#ifndef __FAULT_MASK_H__
#define __FAULT_MASK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "ctrlr_config.h"

/* --------------------------------------------------------------------------
 * fault_mask.h / fault_mask.c -- the set of faults the controller has MEASURED
 * since it entered FAULT, as one bit mask, and the pure functions around it.
 * Added 2026-11-09, direct request: STATE? used to name only the FIRST fault
 * that latched (`OK FAULT GENERAL`), so an operator with two things wrong
 * (say XR1 water and XR3 overcurrent) saw one of them and had to find the
 * other by trial. state_machine.c keeps the mask (SM_NoteFaults(),
 * SM_GetActiveFaults()); STATE? prints it.
 *
 * Hardware-independent on purpose (src/app rule 7, AGENTS.md "Source
 * layout"): no HAL, no pins -- the callers read the pins and pass the raw
 * levels in. Host-tested in tests/test_fault_mask.c.
 *
 * Bit layout: one group of FM_CHANNELS bits per per-channel category, channel
 * index 0..3 within the group, then three system-wide bits. The WATER, TMP and
 * ENERPRO groups are laid out exactly like the GateDriverStatus pins they come
 * from (PE0..PE3, PE4..PE7, PE8..PE11), which is also why the three
 * XR_*_FLT_BIT_BASE values below live here, next to the layout.
 * -------------------------------------------------------------------------- */

#define FM_CHANNELS   (4U)   /* the pin tables (board_io.c) and the GateDriverStatus layout stop at 4 */

#if (HRTIM_NUM_CHANNELS > FM_CHANNELS)
#error "fault_mask.h: the fault pins are laid out for 4 Transrex channels"
#endif

/* Base bit position, within GateDriver_Read()'s 12-bit PE0..PE11 result, of each
   category's XR1 entry (channel index ch uses bit base + ch). Verified
   programmatically (not by eye) against docs/pin_mapping_v4.csv's "XREX Pin
   Name" column, 2026-09-17:
     XR1_WATER_FLT=PE0(bit0)   XR2=PE1  XR3=PE2  XR4=PE3
     XR1_TMP_FLT=PE4(bit4)     XR2=PE5  XR3=PE6  XR4=PE7
     XR1_ENERPRO_FLT=PE8(bit8) XR2=PE9  XR3=PE10 XR4=PE11 */
#define XR_WATER_FLT_BIT_BASE     (0U)
#define XR_TMP_FLT_BIT_BASE       (4U)
#define XR_ENERPRO_FLT_BIT_BASE   (8U)

/* The mask itself. ch is 0-based. */
#define FM_WATER(ch)       (1UL << (0U  + (ch)))
#define FM_TMP(ch)         (1UL << (4U  + (ch)))
#define FM_ENERPRO(ch)     (1UL << (8U  + (ch)))
#define FM_OCP(ch)         (1UL << (12U + (ch)))
#define FM_ENABLE_OUT(ch)  (1UL << (16U + (ch)))   /* the channel's ENA_OUT/CONTACT_OUT not both HIGH while armed/firing */
#define FM_EXT_ENABLE      (1UL << 20)             /* the PF13 interlock input lost while firing */
#define FM_PC10            (1UL << 21)             /* the HRTIM native fault input (PC10/FLT6) tripped */
#define FM_TEST            (1UL << 22)             /* GENERAL:TEST:FAULT (a software-injected fault) */

#define FM_WATER_TMP_MASK  (0x000000FFUL)          /* all WATER and TMP bits: the faults that stop every channel */
#define FM_ENERPRO_MASK    (0x00000F00UL)

/* The WATER, TMP and ENERPRO bits for one raw 12-bit GateDriverStatus read.
   `enabledChannels` has bit ch set for every channel that is enabled
   (SOURce:ENAble): a disabled channel's pins never count. The three polarities
   are ctrlr_config.h's FAULT_POLARITY_NORMALLY_HIGH (a LOW pin is the fault)
   or _LOW (a HIGH pin is the fault). Pure: no pins read, nothing latched. */
uint32_t FaultMask_FromGateDriver(uint16_t raw12, uint8_t enabledChannels,
                                  uint32_t waterPolarity, uint32_t tmpPolarity,
                                  uint32_t enerproPolarity);

/* Writes the mask as comma-separated names -- WATER1, TMP2, ENERPRO1, OCP3,
   ENABLE_OUTPUT4 (channels 1-based, the wire convention), EXTERNAL_ENABLE,
   PC10, TEST -- in that order, or NONE for an empty mask, and returns the length.
   Always NUL-terminated when cap > 0; a list too long for `cap` is cut at a
   whole name. FAULT_MASK_TEXT_MAX is enough for every bit set. */
#define FAULT_MASK_TEXT_MAX  (256U)
size_t FaultMask_Format(uint32_t mask, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* __FAULT_MASK_H__ */
