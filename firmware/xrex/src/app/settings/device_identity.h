/*
 * device_identity.h -- which board is this (added 2026-10-07).
 *
 * Several copies of the board will be deployed, each controlling a different system, and a
 * host service has to know which one it is talking to. Design, storage plan and the
 * host side: docs/device_identity_and_host_service.md at the top of the repository.
 *
 * Two layers:
 *   - HARDWARE identity: the MCU's 96-bit unique ID and the flash chip's 64-bit unique ID.
 *     Read-only, on every board, no provisioning.
 *   - SERIAL (and an optional label): written ONCE at provisioning into the identity region of the
 *     external flash (nvstore.h, NVSTORE_IDENTITY_ADDR: two 4 KiB sectors, alternating copies, each
 *     record CRC-protected and bound to the two hardware IDs it was written on). `verified` says
 *     the record was written for THIS silicon: a flash chip moved to another board, or a copied
 *     image, reads as unverified.
 *
 * Record (88 bytes at the start of each sector, little-endian):
 *    0 magic "WID1"    4 version=1 (u16)   6 reserved (u16)   8 sequence (u32; the newer copy wins)
 *   12 serial[16]     28 label[32]         60 mcu uid (3 x u32)   72 flash uid[8]
 *   80 reserved (u32)  84 CRC-32 of bytes 0..83
 * A write erases the copy that is NOT the newest valid one and programs the new record there, so a
 * power cut at any moment leaves the previous record intact.
 *
 * Shared byte-for-byte by WHAM-XREX-PFMG474 and WHAM-HBRIDGE-PFMG474 (check_shared.py).
 * Identity is reported, never enforced: nothing in the controller depends on it. Main loop only
 * (QUADSPI); writes take tens of milliseconds, so callers write only while IDLE.
 */
#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#define DEVICE_UID_HEX_CHARS        (24U)   /* 96 bits */
#define DEVICE_FLASH_UID_HEX_CHARS  (16U)   /* 64 bits */
#define DEVICE_SERIAL_MAX           (16U)   /* characters, without the terminator */
#define DEVICE_LABEL_MAX            (32U)

typedef struct
{
    uint32_t uid[3];          /* MCU unique ID, word 0 first */
    uint8_t  flashUid[8];     /* W25Q128 unique ID, in the order the chip sends it */
    uint8_t  flashUidValid;   /* 0 when the chip did not answer (flashUid is then all zero) */
} device_hw_id_t;

typedef enum
{
    DEVICE_IDENTITY_UNSET = 0,   /* both copies blank: no serial provisioned */
    DEVICE_IDENTITY_OK,          /* a valid record is stored */
    DEVICE_IDENTITY_CORRUPT,     /* something is stored but no copy passes its checks */
    DEVICE_IDENTITY_NOCHIP       /* the flash chip did not answer */
} device_identity_state_t;

typedef enum
{
    DEVID_OK = 0,
    DEVID_ERR_BAD_SERIAL,        /* not 3-16 characters of A-Z 0-9 - (or "UNSET") */
    DEVID_ERR_BAD_LABEL,         /* not 1-32 characters of A-Z a-z 0-9 . _ - */
    DEVID_ERR_ALREADY_SET,       /* a serial is provisioned: clear it first */
    DEVID_ERR_NOT_SET,           /* no serial is provisioned (a label needs one) */
    DEVID_ERR_NO_HWID,           /* the flash unique ID could not be read, so nothing can be bound */
    DEVID_ERR_FLASH              /* the flash erase/program failed (the previous record is intact) */
} devid_result_t;

/* Reads both hardware IDs. The flash ID is read once and kept; a chip that did not answer is asked
   again next time. */
void DeviceIdentity_ReadHardware(device_hw_id_t *hw);

/* Loads the stored record. Call once at start-up, after the QUADSPI bring-up; every change reloads. */
void DeviceIdentity_Init(void);

device_identity_state_t DeviceIdentity_State(void);
const char *DeviceIdentity_StateName(void);   /* "unset", "ok", "corrupt", "nochip" */

/* The board serial number, or "UNSET" while none is provisioned (or the record is unusable). */
const char *DeviceIdentity_Serial(void);

/* The label, or "" for none. */
const char *DeviceIdentity_Label(void);

/* 1 when a record is stored AND was written for this MCU and this flash chip. */
uint8_t DeviceIdentity_Verified(void);

/* Format checks (no flash access). */
uint8_t DeviceIdentity_SerialIsValid(const char *serial);
uint8_t DeviceIdentity_LabelIsValid(const char *label);

/* Provisions the serial, bound to this board's hardware IDs. Only while UNSET or CORRUPT. */
devid_result_t DeviceIdentity_SetSerial(const char *serial);

/* Sets the label; "-" clears it. Needs a serial. Keeps the record's hardware binding. */
devid_result_t DeviceIdentity_SetLabel(const char *label);

/* Erases the stored record (both copies): back to UNSET. */
devid_result_t DeviceIdentity_Clear(void);

/* 24 / 16 upper-case hex characters plus the terminator ("0000000000000000" when the flash
   ID is not valid). */
void DeviceIdentity_FormatUid(const device_hw_id_t *hw, char out[DEVICE_UID_HEX_CHARS + 1U]);
void DeviceIdentity_FormatFlashUid(const device_hw_id_t *hw, char out[DEVICE_FLASH_UID_HEX_CHARS + 1U]);

/* The short form appended to *IDN? and the !BOOT banner:
       SN=<serial> UID=<24 hex>
   Returns the length written (always NUL-terminated; truncated to fit n). */
size_t DeviceIdentity_FormatTag(char *buf, size_t n);

/* The body of SYS:ID? (without the leading "OK "):
       serial=<sn> uid=<24 hex> flashuid=<16 hex> product=<p> hw=<rev> fw=<ver> commit=<c> label=<l|-> verified=<0|1> identity=<state>
   product, hw, fw and commit are the product's own strings (ctrlr_config.h, git_version.h). */
size_t DeviceIdentity_FormatId(char *buf, size_t n, const char *product, const char *hwRev,
                               const char *fwVersion, const char *commit);

#endif /* DEVICE_IDENTITY_H */
