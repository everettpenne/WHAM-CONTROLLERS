/*
 * profile_store.h -- numbered slots of saved settings in the external
 * flash (nvstore.h). Knows nothing about what a payload means (that is
 * settings_codec.h); it stores, finds, validates and clears opaque
 * payloads, and survives a power cut at any point of a save.
 *
 * Layout: PROFILE_SLOT_COUNT slots, each two 4 KiB sectors ("copies")
 * starting at PROFILE_STORE_BASE. A save never touches the copy that
 * currently holds the newest valid data: it erases and writes the OTHER
 * copy, and only once that copy's CRC checks out does it become the
 * newest (higher sequence number). A power cut mid-save therefore leaves
 * the previous data intact and valid. Clearing erases the older copy
 * first for the same reason. The alternation also halves the wear on
 * each sector.
 *
 * Copy header (48 bytes, little-endian) followed by the payload:
 *    0  magic "WSPF"      4  format version (u16)    6  payload length (u16)
 *    8  sequence (u32)   12  label[16]  (NUL-padded)
 *   28  firmware[16]     44  CRC-32 of bytes 0..43 and the payload
 *
 * Blocking, main-loop only, like nvstore.h; callers write only in IDLE.
 */
#ifndef PROFILE_STORE_H
#define PROFILE_STORE_H

#include <stdint.h>
#include "nvstore.h"

#define PROFILE_SLOT_COUNT    (8U)
#define PROFILE_STORE_BASE    (0UL)
#define PROFILE_PAYLOAD_MAX   (1024U)
#define PROFILE_LABEL_MAX     (15U)
#define PROFILE_FW_MAX        (15U)

typedef enum
{
    PROFILE_OK = 0,
    PROFILE_ERR_SLOT,      /* slot index out of range */
    PROFILE_ERR_EMPTY,     /* nothing saved in the slot */
    PROFILE_ERR_CORRUPT,   /* something is there but no copy passes its CRC */
    PROFILE_ERR_TOO_BIG,   /* payload larger than PROFILE_PAYLOAD_MAX / caller's buffer */
    PROFILE_ERR_STORAGE    /* the flash driver failed; see ProfileStore_StorageStatus() */
} profile_status_t;

typedef enum { PROFILE_SLOT_EMPTY = 0, PROFILE_SLOT_VALID, PROFILE_SLOT_CORRUPT } profile_slot_state_t;

typedef struct
{
    profile_slot_state_t state;
    uint32_t seq;
    uint16_t payloadLen;
    char     label[PROFILE_LABEL_MAX + 1U];
    char     fw[PROFILE_FW_MAX + 1U];
} profile_info_t;

/* Short text for a status. */
const char *ProfileStore_StatusText(profile_status_t status);

/* The flash driver's own status behind the last PROFILE_ERR_STORAGE. */
nvstore_status_t ProfileStore_StorageStatus(void);

/* State and header details of a slot (0-based). Never reads the payload
   into caller memory, but does verify the CRC over it. */
profile_status_t ProfileStore_Info(uint8_t slot, profile_info_t *info);

/* Reads the newest valid copy's payload into buf (capacity cap). */
profile_status_t ProfileStore_Read(uint8_t slot, uint8_t *buf, uint16_t cap, uint16_t *len,
                                   profile_info_t *info);

/* Saves payload into the slot. label/fw may be NULL (stored empty);
   over-long strings are truncated. */
profile_status_t ProfileStore_Write(uint8_t slot, const char *label, const char *fw,
                                    const uint8_t *payload, uint16_t len);

/* Erases both copies (older first). */
profile_status_t ProfileStore_Clear(uint8_t slot);

#endif /* PROFILE_STORE_H */
