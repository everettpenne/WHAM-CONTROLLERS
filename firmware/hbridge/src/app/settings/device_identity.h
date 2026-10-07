/*
 * device_identity.h -- which board is this (added 2026-10-07).
 *
 * Several copies of the board will be deployed, each controlling a different system, and a
 * host service has to know which one it is talking to. Design, storage plan and the
 * host side: docs/device_identity_and_host_service.md at the top of the repository.
 *
 * Step 1 (this file today): the HARDWARE identity only -- the MCU's 96-bit unique ID and
 * the flash chip's 64-bit unique ID, both read-only and present on every board. The
 * serial number, label and "verified" flag arrive with the persistent identity record
 * (step 2); until then the serial reads "UNSET".
 *
 * Shared byte-for-byte by WHAM-XREX-PFMG474 and WHAM-HBRIDGE-PFMG474 (check_shared.py).
 * Identity is reported, never enforced: nothing in the controller depends on it.
 * Main loop only: reading the flash ID uses the QUADSPI.
 */
#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#define DEVICE_UID_HEX_CHARS        (24U)   /* 96 bits */
#define DEVICE_FLASH_UID_HEX_CHARS  (16U)   /* 64 bits */

typedef struct
{
    uint32_t uid[3];          /* MCU unique ID, word 0 first */
    uint8_t  flashUid[8];     /* W25Q128 unique ID, in the order the chip sends it */
    uint8_t  flashUidValid;   /* 0 when the chip did not answer (flashUid is then all zero) */
} device_hw_id_t;

/* Reads both IDs. The flash ID is read once and kept; a chip that did not answer is asked
   again next time. */
void DeviceIdentity_ReadHardware(device_hw_id_t *hw);

/* The board serial number, or "UNSET" while none has been provisioned. */
const char *DeviceIdentity_Serial(void);

/* 24 / 16 upper-case hex characters plus the terminator ("0000000000000000" when the flash
   ID is not valid). */
void DeviceIdentity_FormatUid(const device_hw_id_t *hw, char out[DEVICE_UID_HEX_CHARS + 1U]);
void DeviceIdentity_FormatFlashUid(const device_hw_id_t *hw, char out[DEVICE_FLASH_UID_HEX_CHARS + 1U]);

/* The short form appended to *IDN? and the !BOOT banner:
       SN=<serial> UID=<24 hex>
   Returns the length written (always NUL-terminated; truncated to fit n). */
size_t DeviceIdentity_FormatTag(char *buf, size_t n);

/* The body of SYS:ID? (without the leading "OK "):
       serial=<sn> uid=<24 hex> flashuid=<16 hex> product=<p> hw=<rev> fw=<ver> commit=<c> label=<l> verified=<0|1>
   product, hw, fw and commit are the product's own strings (ctrlr_config.h, git_version.h). */
size_t DeviceIdentity_FormatId(char *buf, size_t n, const char *product, const char *hwRev,
                               const char *fwVersion, const char *commit);

#endif /* DEVICE_IDENTITY_H */
