/*
 * nvstore.h -- non-volatile byte storage on the board's external NOR
 * flash (Winbond W25Q128JVS on QUADSPI, docs/pin_mapping_v4.csv).
 * Implemented in src/bsp/<chip>/nvstore.c.
 *
 * Added 2026-09-29 as the storage layer for saved settings profiles
 * (*SAV / *RCL). It is deliberately a dumb byte store -- read, erase a
 * sector, program bytes -- with no idea what is kept in it; the record
 * format, slots and power-fail handling are built on top of it in
 * src/app. Nothing here knows about settings.
 *
 * Why the external chip and not the MCU's own flash: both internal
 * banks belong to the dual-bank firmware update (flash_bank.h), so data
 * kept there would be erased or left behind by a bank swap; and erasing
 * an internal page stalls instruction fetch, which would freeze the
 * control loop's ISR. The external chip is independent of both --
 * settings survive firmware updates and rollbacks untouched.
 *
 * NOR flash rules the caller must respect:
 *   - a byte can only be programmed from 1 to 0; only a SECTOR ERASE
 *     (4 KiB, sets every bit back to 1 = 0xFF) goes the other way. So
 *     erase a sector, then program it, never program over old data.
 *   - each sector survives about 100 000 erase cycles (datasheet).
 *
 * Every call blocks (polling, no DMA/interrupts) and must be made from
 * the main loop, never from an ISR: it uses HAL_GetTick() for its
 * timeouts. A sector erase can take up to ~400 ms (typically ~45 ms),
 * page programs up to ~3 ms; the control loop runs from the HRTIM
 * interrupt and is unaffected, but the serial command link is not
 * serviced meanwhile. Callers therefore only write while the controller
 * is IDLE.
 *
 * Removable like the other optional modules: NVSTORE_FEATURE_ENABLED
 * below is the single point of control. Set to 0 and every call
 * returns NVSTORE_ERR_NOT_READY. It also needs the QUADSPI bring-up
 * (QSPI_TEST_FEATURE_ENABLED, qspi_test.h) -- without it the chip is
 * simply never initialised and the same error comes back.
 */
#ifndef NVSTORE_H
#define NVSTORE_H

#include <stdint.h>

#ifndef NVSTORE_FEATURE_ENABLED
#define NVSTORE_FEATURE_ENABLED (1)
#endif

/* Chip geometry (W25Q128JVS: 128 Mbit). */
#define NVSTORE_CAPACITY_BYTES  (16UL * 1024UL * 1024UL)
#define NVSTORE_SECTOR_BYTES    (4096UL)
#define NVSTORE_PAGE_BYTES      (256UL)

/* Last sector of the chip: reserved for the QSPI:SELFtest bench test, which
   erases and rewrites it freely. Nothing else may store data here (saved
   settings live at the low addresses). */
#define NVSTORE_SCRATCH_ADDR    (NVSTORE_CAPACITY_BYTES - NVSTORE_SECTOR_BYTES)

typedef enum
{
    NVSTORE_OK = 0,
    NVSTORE_ERR_NOT_READY,   /* feature/QUADSPI disabled, or the chip did not answer as a W25Q128 */
    NVSTORE_ERR_RANGE,       /* address/length outside the chip, or erase address not sector-aligned */
    NVSTORE_ERR_TIMEOUT,     /* chip stayed busy longer than its datasheet maximum */
    NVSTORE_ERR_IO,          /* QUADSPI transfer failed */
    NVSTORE_ERR_PROTECTED,   /* block-protect bits set in the chip's status registers */
    NVSTORE_ERR_VERIFY       /* read-back after an erase/program did not match */
} nvstore_status_t;

/* Short fixed text for a status ("ok", "not ready", ...), for replies
   and logs. Never NULL. */
const char *NvStore_StatusText(nvstore_status_t status);

/* Confirms the chip is there and is a W25Q128 (JEDEC ID EF 40 18).
   Every other call does this itself first; exposed for diagnostics. */
nvstore_status_t NvStore_Probe(void);

/* Reads len bytes starting at addr into buf. */
nvstore_status_t NvStore_Read(uint32_t addr, void *buf, uint32_t len);

/* Erases the 4 KiB sector that starts at addr (must be a multiple of
   NVSTORE_SECTOR_BYTES) and checks it reads back as all 0xFF. */
nvstore_status_t NvStore_EraseSector(uint32_t addr);

/* Programs len bytes at addr, any alignment and length (split at page
   boundaries internally), then reads them back and compares. The bytes
   must currently be erased (0xFF) -- see the NOR rules above. */
nvstore_status_t NvStore_Program(uint32_t addr, const void *data, uint32_t len);

/* Raw status registers 1, 2 and 3 (Winbond layout) into sr[0..2].
   Bring-up/diagnostics: sr[0] bit0 = BUSY, bit1 = write-enable latch,
   bits 2-4 = block-protect. */
nvstore_status_t NvStore_ReadStatusRegs(uint8_t sr[3]);

#endif /* NVSTORE_H */
