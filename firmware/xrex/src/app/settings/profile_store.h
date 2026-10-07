/*
 * profile_store.h -- numbered slots of saved settings in the external
 * flash (nvstore.h). Knows nothing about what a payload means (that is
 * settings_codec.h); it stores, finds, validates and clears opaque
 * payloads, and survives a power cut at any point of a save.
 *
 * Layout: PROFILE_SLOT_COUNT slots, each two "copies" of PROFILE_COPY_SECTORS
 * 4 KiB sectors, starting at PROFILE_STORE_BASE. A save never touches the
 * copy that currently holds the newest valid data: it erases and writes the
 * OTHER copy, and only once the header (which carries the CRC) is programmed
 * LAST, and reads back valid, does that copy become the newest (higher
 * sequence number). A power cut mid-save therefore leaves the previous data
 * intact and valid: before the header is written the copy reads as empty, and
 * a half-written header fails its CRC. Clearing erases the older copy first
 * for the same reason. The alternation also halves the wear on each sector.
 *
 * Copy header (48 bytes, little-endian) followed by the payload:
 *    0  magic (PROFILE_MAGIC_BYTES, per product)   4  format version (u16)
 *    6  payload length (u16)    8  sequence (u32)   12  label[16]  (NUL-padded)
 *   28  firmware[16]           44  CRC-32 of bytes 0..43 and the payload
 *
 * Two ways to write and read:
 *   - buffered (ProfileStore_Write / _Read): the payload is a caller buffer;
 *   - streaming (ProfileWriter_* / ProfileReader_*): the payload goes through in
 *     pieces, so a payload larger than free RAM can be saved and recalled. The
 *     writer needs the TOTAL length up front (the header, written last, carries
 *     it and the CRC); a caller that generates the payload runs its generator
 *     twice, once to count and once to write.
 *
 * SHARED CODE: this file, profile_store.c, nvstore.h/.c and tests/test_profile_store.c are
 * byte-identical in WHAM-XREX-PFMG474 and WHAM-HBRIDGE-PFMG474. What differs per product is
 * configuration (ctrlr_config.h: PROFILE_STORE_BASE, PROFILE_COPY_SECTORS, PROFILE_SLOT_COUNT,
 * PROFILE_MAGIC_BYTES) and what the payload means (settings_codec.h). Change them together.
 *
 * Blocking, main-loop only, like nvstore.h; callers write only in IDLE.
 */
#ifndef PROFILE_STORE_H
#define PROFILE_STORE_H

#include <stdint.h>
#include "ctrlr_config.h"
#include "nvstore.h"

#ifndef PROFILE_SLOT_COUNT
#define PROFILE_SLOT_COUNT    (8U)
#endif
#ifndef PROFILE_COPY_SECTORS
#define PROFILE_COPY_SECTORS  (1U)
#endif
#ifndef PROFILE_MAGIC_BYTES
#define PROFILE_MAGIC_BYTES   { 'W', 'S', 'P', 'F' }
#endif

#ifndef PROFILE_STORE_BASE
#define PROFILE_STORE_BASE    (0UL)
#endif
#define PROFILE_HEADER_BYTES  (48U)
#define PROFILE_COPY_BYTES    (PROFILE_COPY_SECTORS * NVSTORE_SECTOR_BYTES)
#define PROFILE_PAYLOAD_MAX   (PROFILE_COPY_BYTES - PROFILE_HEADER_BYTES)
#define PROFILE_LABEL_MAX     (15U)
#define PROFILE_FW_MAX        (15U)

#if (PROFILE_PAYLOAD_MAX > 65535UL)
#error "the header's payload length is 16 bits: PROFILE_COPY_SECTORS is too large"
#endif
#if ((PROFILE_STORE_BASE + PROFILE_SLOT_COUNT * 2UL * PROFILE_COPY_BYTES) > NVSTORE_IDENTITY_ADDR)
#error "the profile slots overlap the identity sectors or the QSPI:SELFtest scratch sector"
#endif

typedef enum
{
    PROFILE_OK = 0,
    PROFILE_ERR_SLOT,      /* slot index out of range */
    PROFILE_ERR_EMPTY,     /* nothing saved in the slot */
    PROFILE_ERR_CORRUPT,   /* something is there but no copy passes its CRC */
    PROFILE_ERR_TOO_BIG,   /* payload larger than PROFILE_PAYLOAD_MAX / caller's buffer */
    PROFILE_ERR_STORAGE,   /* the flash driver failed; see ProfileStore_StorageStatus() */
    PROFILE_ERR_USAGE      /* a streaming writer was given more or fewer bytes than it was told */
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

/* ---- streaming write ---- */

typedef struct
{
    uint32_t addr;                 /* the copy being written */
    uint32_t seq;
    uint32_t crc;                  /* running, header bytes 0..43 first */
    uint16_t total;
    uint16_t pos;
    uint8_t  hdr[PROFILE_HEADER_BYTES];
    uint8_t  slot;
    uint8_t  copy;
} profile_writer_t;

/* Starts a save of exactly `total` payload bytes: picks the copy that does NOT hold the newest valid
   data and erases the sectors the save needs. Nothing is visible yet. */
profile_status_t ProfileWriter_Begin(profile_writer_t *w, uint8_t slot, const char *label, const char *fw,
                                     uint16_t total);

/* Appends payload bytes (in order, `total` of them across all calls). */
profile_status_t ProfileWriter_Write(profile_writer_t *w, const void *data, uint16_t n);

/* Programs the header last and checks the copy reads back valid. PROFILE_ERR_USAGE if fewer than `total`
   bytes were written. */
profile_status_t ProfileWriter_End(profile_writer_t *w);

/* ---- streaming read ---- */

typedef struct
{
    uint32_t addr;                 /* the payload's first byte in flash */
    uint16_t len;
} profile_reader_t;

/* Finds the newest copy whose CRC checks out (the whole payload is verified here). */
profile_status_t ProfileReader_Open(uint8_t slot, profile_reader_t *r, profile_info_t *info);

/* Reads n payload bytes from `offset`. PROFILE_ERR_TOO_BIG beyond the payload's end. */
profile_status_t ProfileReader_Read(const profile_reader_t *r, uint16_t offset, void *buf, uint16_t n);

#endif /* PROFILE_STORE_H */
