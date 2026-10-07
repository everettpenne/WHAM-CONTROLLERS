/*
 * profile_store.c -- see profile_store.h.
 */
#include "profile_store.h"
#include "settings_codec.h"
#include <string.h>

#define HDR_BYTES       (PROFILE_HEADER_BYTES)
#define HDR_CRC_OFFSET  (44U)
#define FORMAT_VERSION  (1U)
static const uint8_t kMagic[4] = PROFILE_MAGIC_BYTES;

static nvstore_status_t g_lastStorage = NVSTORE_OK;

typedef struct
{
    profile_slot_state_t state;   /* of this one copy */
    profile_info_t       info;
} copy_t;

const char *ProfileStore_StatusText(profile_status_t status)
{
    switch (status)
    {
    case PROFILE_OK:          return "ok";
    case PROFILE_ERR_SLOT:    return "no such slot";
    case PROFILE_ERR_EMPTY:   return "slot is empty";
    case PROFILE_ERR_CORRUPT: return "slot is corrupt";
    case PROFILE_ERR_TOO_BIG: return "payload too large";
    case PROFILE_ERR_STORAGE: return "flash error";
    case PROFILE_ERR_USAGE:   return "wrong byte count";
    default:                  return "unknown";
    }
}

nvstore_status_t ProfileStore_StorageStatus(void)
{
    return g_lastStorage;
}

static uint32_t CopyAddr(uint8_t slot, uint8_t copy)
{
    return PROFILE_STORE_BASE + ((uint32_t)slot * 2U + copy) * PROFILE_COPY_BYTES;
}

static uint32_t Rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void Wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static profile_status_t Storage(nvstore_status_t st)
{
    g_lastStorage = st;
    return (st == NVSTORE_OK) ? PROFILE_OK : PROFILE_ERR_STORAGE;
}

/* Classifies one copy: reads the header and checks the CRC over header + payload (64 bytes at a time, no
   payload buffer). */
static profile_status_t ReadCopy(uint8_t slot, uint8_t copy, copy_t *out)
{
    uint8_t  hdr[HDR_BYTES];
    uint8_t  chunk[64];
    uint32_t addr = CopyAddr(slot, copy);
    uint32_t crc;
    uint16_t len;
    profile_status_t rc;

    memset(out, 0, sizeof(*out));
    rc = Storage(NvStore_Read(addr, hdr, HDR_BYTES));
    if (rc != PROFILE_OK)
    {
        return rc;
    }

    if (hdr[0] == 0xFFU && hdr[1] == 0xFFU && hdr[2] == 0xFFU && hdr[3] == 0xFFU)
    {
        out->state = PROFILE_SLOT_EMPTY;
        return PROFILE_OK;
    }

    out->state = PROFILE_SLOT_CORRUPT;   /* until proven otherwise */
    len = (uint16_t)(hdr[6] | (hdr[7] << 8));
    if (memcmp(hdr, kMagic, 4) != 0 || (uint16_t)(hdr[4] | (hdr[5] << 8)) != FORMAT_VERSION ||
        len > PROFILE_PAYLOAD_MAX)
    {
        return PROFILE_OK;
    }

    crc = SettingsCodec_Crc32(0U, hdr, HDR_CRC_OFFSET);
    for (uint16_t off = 0U; off < len; off = (uint16_t)(off + sizeof(chunk)))
    {
        uint16_t n = (uint16_t)(((len - off) < sizeof(chunk)) ? (len - off) : sizeof(chunk));

        rc = Storage(NvStore_Read(addr + HDR_BYTES + off, chunk, n));
        if (rc != PROFILE_OK)
        {
            return rc;
        }
        crc = SettingsCodec_Crc32(crc, chunk, n);
    }
    if (crc != Rd32(&hdr[HDR_CRC_OFFSET]))
    {
        return PROFILE_OK;
    }

    out->state = PROFILE_SLOT_VALID;
    out->info.state = PROFILE_SLOT_VALID;
    out->info.seq = Rd32(&hdr[8]);
    out->info.payloadLen = len;
    memcpy(out->info.label, &hdr[12], PROFILE_LABEL_MAX);
    memcpy(out->info.fw, &hdr[28], PROFILE_FW_MAX);
    return PROFILE_OK;
}

/* Looks at both copies. *newest = index of the newest valid one, or -1. */
static profile_status_t Survey(uint8_t slot, copy_t c[2], int8_t *newest, profile_slot_state_t *state)
{
    profile_status_t rc;

    *newest = -1;
    for (uint8_t i = 0U; i < 2U; i++)
    {
        rc = ReadCopy(slot, i, &c[i]);
        if (rc != PROFILE_OK)
        {
            return rc;
        }
        if (c[i].state == PROFILE_SLOT_VALID &&
            (*newest < 0 || c[i].info.seq > c[*newest].info.seq))
        {
            *newest = (int8_t)i;
        }
    }
    if (*newest >= 0)
    {
        *state = PROFILE_SLOT_VALID;
    }
    else if (c[0].state == PROFILE_SLOT_CORRUPT || c[1].state == PROFILE_SLOT_CORRUPT)
    {
        *state = PROFILE_SLOT_CORRUPT;
    }
    else
    {
        *state = PROFILE_SLOT_EMPTY;
    }
    return PROFILE_OK;
}

profile_status_t ProfileStore_Info(uint8_t slot, profile_info_t *info)
{
    copy_t c[2];
    int8_t newest;
    profile_slot_state_t state;
    profile_status_t rc;

    if (slot >= PROFILE_SLOT_COUNT)
    {
        return PROFILE_ERR_SLOT;
    }
    rc = Survey(slot, c, &newest, &state);
    if (rc != PROFILE_OK)
    {
        return rc;
    }
    memset(info, 0, sizeof(*info));
    if (newest >= 0)
    {
        *info = c[newest].info;
    }
    info->state = state;
    return PROFILE_OK;
}

/* ---- streaming read ---- */

profile_status_t ProfileReader_Open(uint8_t slot, profile_reader_t *r, profile_info_t *info)
{
    copy_t c[2];
    int8_t newest;
    profile_slot_state_t state;
    profile_status_t rc;

    if (slot >= PROFILE_SLOT_COUNT)
    {
        return PROFILE_ERR_SLOT;
    }
    if ((rc = Survey(slot, c, &newest, &state)) != PROFILE_OK)
    {
        return rc;
    }
    if (state == PROFILE_SLOT_EMPTY)
    {
        return PROFILE_ERR_EMPTY;
    }
    if (newest < 0)
    {
        return PROFILE_ERR_CORRUPT;
    }
    r->addr = CopyAddr(slot, (uint8_t)newest) + HDR_BYTES;
    r->len = c[newest].info.payloadLen;
    if (info != NULL)
    {
        *info = c[newest].info;
    }
    return PROFILE_OK;
}

profile_status_t ProfileReader_Read(const profile_reader_t *r, uint16_t offset, void *buf, uint16_t n)
{
    if (((uint32_t)offset + n) > r->len)
    {
        return PROFILE_ERR_TOO_BIG;
    }
    return Storage(NvStore_Read(r->addr + offset, buf, n));
}

profile_status_t ProfileStore_Read(uint8_t slot, uint8_t *buf, uint16_t cap, uint16_t *len,
                                   profile_info_t *info)
{
    profile_reader_t r;
    profile_status_t rc = ProfileReader_Open(slot, &r, info);

    if (rc != PROFILE_OK)
    {
        return rc;
    }
    if (r.len > cap)
    {
        return PROFILE_ERR_TOO_BIG;
    }
    if (r.len != 0U && (rc = ProfileReader_Read(&r, 0U, buf, r.len)) != PROFILE_OK)
    {
        return rc;
    }
    *len = r.len;
    return PROFILE_OK;
}

/* ---- streaming write ---- */

profile_status_t ProfileWriter_Begin(profile_writer_t *w, uint8_t slot, const char *label, const char *fw,
                                     uint16_t total)
{
    copy_t c[2];
    int8_t newest;
    profile_slot_state_t state;
    profile_status_t rc;
    uint32_t sectors;

    if (slot >= PROFILE_SLOT_COUNT)
    {
        return PROFILE_ERR_SLOT;
    }
    if (total > PROFILE_PAYLOAD_MAX)
    {
        return PROFILE_ERR_TOO_BIG;
    }
    if ((rc = Survey(slot, c, &newest, &state)) != PROFILE_OK)
    {
        return rc;
    }
    memset(w, 0, sizeof(*w));
    w->slot = slot;
    w->copy = (newest == 0) ? 1U : 0U;
    w->seq = (newest >= 0) ? (c[newest].info.seq + 1U) : 1U;
    w->addr = CopyAddr(slot, w->copy);
    w->total = total;

    memcpy(w->hdr, kMagic, 4);
    w->hdr[4] = (uint8_t)FORMAT_VERSION;
    w->hdr[5] = (uint8_t)(FORMAT_VERSION >> 8);
    w->hdr[6] = (uint8_t)total;
    w->hdr[7] = (uint8_t)(total >> 8);
    Wr32(&w->hdr[8], w->seq);
    if (label != NULL)
    {
        strncpy((char *)&w->hdr[12], label, PROFILE_LABEL_MAX);
    }
    if (fw != NULL)
    {
        strncpy((char *)&w->hdr[28], fw, PROFILE_FW_MAX);
    }
    w->crc = SettingsCodec_Crc32(0U, w->hdr, HDR_CRC_OFFSET);

    /* erase exactly the sectors this save will use (the first holds the header, which stays blank until the end) */
    sectors = (HDR_BYTES + (uint32_t)total + NVSTORE_SECTOR_BYTES - 1U) / NVSTORE_SECTOR_BYTES;
    for (uint32_t s = 0U; s < sectors; s++)
    {
        if ((rc = Storage(NvStore_EraseSector(w->addr + s * NVSTORE_SECTOR_BYTES))) != PROFILE_OK)
        {
            return rc;
        }
    }
    return PROFILE_OK;
}

profile_status_t ProfileWriter_Write(profile_writer_t *w, const void *data, uint16_t n)
{
    profile_status_t rc;

    if (((uint32_t)w->pos + n) > w->total)
    {
        return PROFILE_ERR_USAGE;
    }
    if (n == 0U)
    {
        return PROFILE_OK;
    }
    if ((rc = Storage(NvStore_Program(w->addr + HDR_BYTES + w->pos, data, n))) != PROFILE_OK)
    {
        return rc;
    }
    w->crc = SettingsCodec_Crc32(w->crc, (const uint8_t *)data, n);
    w->pos = (uint16_t)(w->pos + n);
    return PROFILE_OK;
}

profile_status_t ProfileWriter_End(profile_writer_t *w)
{
    copy_t check;
    profile_status_t rc;

    if (w->pos != w->total)
    {
        return PROFILE_ERR_USAGE;
    }
    Wr32(&w->hdr[HDR_CRC_OFFSET], w->crc);
    /* the header goes in LAST: until it is there (and its CRC is right) this copy is not a profile at all */
    if ((rc = Storage(NvStore_Program(w->addr, w->hdr, HDR_BYTES))) != PROFILE_OK)
    {
        return rc;
    }
    /* Only a copy that reads back valid counts as saved. */
    if ((rc = ReadCopy(w->slot, w->copy, &check)) != PROFILE_OK)
    {
        return rc;
    }
    return (check.state == PROFILE_SLOT_VALID && check.info.seq == w->seq) ? PROFILE_OK
                                                                           : PROFILE_ERR_CORRUPT;
}

profile_status_t ProfileStore_Write(uint8_t slot, const char *label, const char *fw,
                                    const uint8_t *payload, uint16_t len)
{
    profile_writer_t w;
    profile_status_t rc = ProfileWriter_Begin(&w, slot, label, fw, len);

    if (rc != PROFILE_OK)
    {
        return rc;
    }
    if ((rc = ProfileWriter_Write(&w, payload, len)) != PROFILE_OK)
    {
        return rc;
    }
    return ProfileWriter_End(&w);
}

profile_status_t ProfileStore_Clear(uint8_t slot)
{
    copy_t c[2];
    int8_t newest;
    profile_slot_state_t state;
    profile_status_t rc;
    uint8_t first;

    if (slot >= PROFILE_SLOT_COUNT)
    {
        return PROFILE_ERR_SLOT;
    }
    if ((rc = Survey(slot, c, &newest, &state)) != PROFILE_OK)
    {
        return rc;
    }
    /* Older copy first: a power cut between the two erases must leave the
       newest data (the slot as it was), never resurrect stale data. Erasing a
       copy's FIRST sector (the header) is enough: the rest is unreachable. */
    first = (newest == 0) ? 1U : 0U;
    if ((rc = Storage(NvStore_EraseSector(CopyAddr(slot, first)))) != PROFILE_OK)
    {
        return rc;
    }
    return Storage(NvStore_EraseSector(CopyAddr(slot, (uint8_t)(1U - first))));
}
