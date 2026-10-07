/*
 * fake_nvstore.h -- a RAM stand-in for the external NOR flash, behind the
 * real src/drivers/nvstore.h interface, with NOR semantics (erase sets
 * 0xFF, program can only clear bits) and a power-cut injector: a budget
 * of "units" (one per sector erase, one per programmed byte); when it
 * runs out the operation in progress is left half done and every further
 * call fails until FakeNv_RestorePower().
 */
#ifndef FAKE_NVSTORE_H
#define FAKE_NVSTORE_H

#include <stdint.h>
#include "nvstore.h"

void     FakeNv_Reset(void);                       /* all 0xFF, power on, no budget */
void     FakeNv_CutPowerAfter(uint32_t units);     /* start counting down */
void     FakeNv_RestorePower(void);                /* power back, budget removed */
uint32_t FakeNv_UnitsUsed(void);                   /* units consumed since Reset/last CutPowerAfter */
void     FakeNv_Poke(uint32_t addr, uint8_t xorMask);  /* corrupt a byte in place */


/* What NvStore_ReadUniqueId() returns: the 8 bytes and the status. */
void     FakeNv_SetUniqueId(const uint8_t id[8], nvstore_status_t status);

#endif /* FAKE_NVSTORE_H */
