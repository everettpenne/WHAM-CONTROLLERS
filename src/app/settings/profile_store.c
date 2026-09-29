/*
 * profile_store.c -- see profile_store.h.
 */
#include "profile_store.h"
#include "settings_codec.h"
#include <string.h>

#define HDR_BYTES       (48U)
#define HDR_CRC_OFFSET  (44U)
#define FORMAT_VERSION  (1U)
static const uint8_t kMagic[4] = { 'W', 'S', 'P', 'F' };

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
    default:                  return "unknown";
    }
}

nvstore_status_t ProfileStore_StorageStatus(void)
{
    return g_lastStorage;
}

static uint32_t CopyAddr(uint8_t slot, uint8_t copy)
{
    return PROFILE_STORE_BASE + ((uint32_t)slot * 2U + copy) * NVSTORE_SECTOR_BYTES;
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

/* Classifies one copy. When payload != NULL the payload is copied there as
   it is CRC-checked (the caller has sized it for the copy's length; the
   contents are only meaningful if the copy comes back VALID). */
static profile_status_t ReadCopy(uint8_t slot, uint8_t copy, copy_t *out, uint8_t *payload)
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
        if (payload != NULL)
        {
            memcpy(&payload[off], chunk, n);
        }
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
        rc = ReadCopy(slot, i, &c[i], NULL);
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

profile_status_t ProfileStore_Read(uint8_t slot, uint8_t *buf, uint16_t cap, uint16_t *len,
                                   profile_info_t *info)
{
    copy_t c[2];
    copy_t again;
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
    if (c[newest].info.payloadLen > cap)
    {
        return PROFILE_ERR_TOO_BIG;
    }
    if ((rc = ReadCopy(slot, (uint8_t)newest, &again, buf)) != PROFILE_OK)
    {
        return rc;
    }
    if (again.state != PROFILE_SLOT_VALID)
    {
        return PROFILE_ERR_CORRUPT;
    }
    *len = again.info.payloadLen;
    if (info != NULL)
    {
        *info = again.info;
    }
    return PROFILE_OK;
}

profile_status_t ProfileStore_Write(uint8_t slot, const char *label, const char *fw,
                                    const uint8_t *payload, uint16_t len)
{
    copy_t c[2];
    copy_t check;
    int8_t newest;
    profile_slot_state_t state;
    profile_status_t rc;
    uint8_t hdr[HDR_BYTES];
    uint8_t target;
    uint32_t seq;
    uint32_t crc;
    uint32_t addr;

    if (slot >= PROFILE_SLOT_COUNT)
    {
        return PROFILE_ERR_SLOT;
    }
    if (len > PROFILE_PAYLOAD_MAX)
    {
        return PROFILE_ERR_TOO_BIG;
    }
    if ((rc = Survey(slot, c, &newest, &state)) != PROFILE_OK)
    {
        return rc;
    }
    target = (newest == 0) ? 1U : 0U;
    seq = (newest >= 0) ? (c[newest].info.seq + 1U) : 1U;
    addr = CopyAddr(slot, target);

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, kMagic, 4);
    hdr[4] = (uint8_t)FORMAT_VERSION;
    hdr[5] = (uint8_t)(FORMAT_VERSION >> 8);
    hdr[6] = (uint8_t)len;
    hdr[7] = (uint8_t)(len >> 8);
    Wr32(&hdr[8], seq);
    if (label != NULL)
    {
        strncpy((char *)&hdr[12], label, PROFILE_LABEL_MAX);
    }
    if (fw != NULL)
    {
        strncpy((char *)&hdr[28], fw, PROFILE_FW_MAX);
    }
    crc = SettingsCodec_Crc32(0U, hdr, HDR_CRC_OFFSET);
    crc = SettingsCodec_Crc32(crc, payload, len);
    Wr32(&hdr[HDR_CRC_OFFSET], crc);

    if ((rc = Storage(NvStore_EraseSector(addr))) != PROFILE_OK ||
        (rc = Storage(NvStore_Program(addr, hdr, HDR_BYTES))) != PROFILE_OK ||
        (len != 0U && (rc = Storage(NvStore_Program(addr + HDR_BYTES, payload, len))) != PROFILE_OK))
    {
        return rc;
    }

    /* Only a copy that reads back valid counts as saved. */
    if ((rc = ReadCopy(slot, target, &check, NULL)) != PROFILE_OK)
    {
        return rc;
    }
    return (check.state == PROFILE_SLOT_VALID && check.info.seq == seq) ? PROFILE_OK
                                                                        : PROFILE_ERR_CORRUPT;
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
       newest data (the slot as it was), never resurrect stale data. */
    first = (newest == 0) ? 1U : 0U;
    if ((rc = Storage(NvStore_EraseSector(CopyAddr(slot, first)))) != PROFILE_OK)
    {
        return rc;
    }
    return Storage(NvStore_EraseSector(CopyAddr(slot, (uint8_t)(1U - first))));
}
