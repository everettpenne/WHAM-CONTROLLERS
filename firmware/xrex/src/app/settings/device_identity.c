/*
 * device_identity.c -- see device_identity.h.
 */
#include "device_identity.h"
#include "device_id.h"
#include "nvstore.h"
#include "settings_codec.h"      /* SettingsCodec_Crc32 */
#include <stdio.h>
#include <string.h>

#define REC_BYTES        (88U)
#define REC_CRC_OFFSET   (84U)
#define REC_VERSION      (1U)
static const uint8_t kMagic[4] = { 'W', 'I', 'D', '1' };

typedef struct
{
    char     serial[DEVICE_SERIAL_MAX + 1U];
    char     label[DEVICE_LABEL_MAX + 1U];
    uint32_t uid[3];
    uint8_t  flashUid[8];
    uint32_t seq;
} record_t;

static uint8_t s_flashUid[8];
static uint8_t s_flashUidValid = 0U;

static device_identity_state_t s_state = DEVICE_IDENTITY_UNSET;
static record_t                s_rec;          /* the newest valid record, when s_state is OK */
static uint8_t                 s_newestIsB = 0U;

static uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void PutLe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* A chip that is absent or unpowered reads back all zeros or all ones, never a real ID. */
static uint8_t PlausibleId(const uint8_t id[8])
{
    uint8_t zeros = 1U;
    uint8_t ones  = 1U;

    for (uint8_t i = 0U; i < 8U; i++)
    {
        if (id[i] != 0x00U) { zeros = 0U; }
        if (id[i] != 0xFFU) { ones = 0U; }
    }
    return ((zeros == 0U) && (ones == 0U)) ? 1U : 0U;
}

void DeviceIdentity_ReadHardware(device_hw_id_t *hw)
{
    uint8_t id[8];

    DeviceId_ReadMcuUid(hw->uid);

    if (s_flashUidValid == 0U)
    {
        memset(id, 0, sizeof(id));
        if ((NvStore_ReadUniqueId(id) == NVSTORE_OK) && (PlausibleId(id) != 0U))
        {
            memcpy(s_flashUid, id, sizeof(s_flashUid));
            s_flashUidValid = 1U;
        }
    }
    memset(hw->flashUid, 0, sizeof(hw->flashUid));
    if (s_flashUidValid != 0U)
    {
        memcpy(hw->flashUid, s_flashUid, sizeof(hw->flashUid));
    }
    hw->flashUidValid = s_flashUidValid;
}

/* ---- the stored record ------------------------------------------------- */

static void Encode(const record_t *r, uint8_t out[REC_BYTES])
{
    memset(out, 0, REC_BYTES);
    memcpy(&out[0], kMagic, 4);
    out[4] = (uint8_t)REC_VERSION;
    PutLe32(&out[8], r->seq);
    memcpy(&out[12], r->serial, DEVICE_SERIAL_MAX);
    memcpy(&out[28], r->label, DEVICE_LABEL_MAX);
    for (uint8_t i = 0U; i < 3U; i++)
    {
        PutLe32(&out[60U + (4U * i)], r->uid[i]);
    }
    memcpy(&out[72], r->flashUid, 8);
    PutLe32(&out[REC_CRC_OFFSET], SettingsCodec_Crc32(0U, out, REC_CRC_OFFSET));
}

/* 1 if `in` holds a valid record (magic, version, CRC); fills *r. */
static uint8_t Decode(const uint8_t in[REC_BYTES], record_t *r)
{
    if ((memcmp(&in[0], kMagic, 4) != 0) || (in[4] != REC_VERSION) || (in[5] != 0U) ||
        (Le32(&in[REC_CRC_OFFSET]) != SettingsCodec_Crc32(0U, in, REC_CRC_OFFSET)))
    {
        return 0U;
    }
    memset(r, 0, sizeof(*r));
    r->seq = Le32(&in[8]);
    memcpy(r->serial, &in[12], DEVICE_SERIAL_MAX);   /* terminated: the arrays are one longer, zeroed */
    memcpy(r->label, &in[28], DEVICE_LABEL_MAX);
    for (uint8_t i = 0U; i < 3U; i++)
    {
        r->uid[i] = Le32(&in[60U + (4U * i)]);
    }
    memcpy(r->flashUid, &in[72], 8);
    return 1U;
}

static uint8_t IsBlank(const uint8_t *p, size_t n)
{
    for (size_t i = 0U; i < n; i++)
    {
        if (p[i] != 0xFFU) { return 0U; }
    }
    return 1U;
}

static void Load(void)
{
    uint8_t  raw[2][REC_BYTES];
    record_t rec[2];
    uint8_t  valid[2];

    memset(&s_rec, 0, sizeof(s_rec));
    s_newestIsB = 0U;
    for (uint8_t c = 0U; c < 2U; c++)
    {
        if (NvStore_Read(NVSTORE_IDENTITY_ADDR + (c * NVSTORE_SECTOR_BYTES), raw[c], REC_BYTES) != NVSTORE_OK)
        {
            s_state = DEVICE_IDENTITY_NOCHIP;
            return;
        }
        valid[c] = Decode(raw[c], &rec[c]);
    }
    if ((valid[0] == 0U) && (valid[1] == 0U))
    {
        s_state = ((IsBlank(raw[0], REC_BYTES) != 0U) && (IsBlank(raw[1], REC_BYTES) != 0U))
                      ? DEVICE_IDENTITY_UNSET : DEVICE_IDENTITY_CORRUPT;
        return;
    }
    s_newestIsB = ((valid[0] == 0U) || ((valid[1] != 0U) && (rec[1].seq > rec[0].seq))) ? 1U : 0U;
    s_rec = rec[s_newestIsB];
    s_state = DEVICE_IDENTITY_OK;
}

/* Writes `r` as the new newest record: into the copy that is not the newest valid one. */
static devid_result_t Write(record_t *r)
{
    uint8_t  raw[REC_BYTES];
    uint32_t target;
    nvstore_status_t st;

    r->seq = (s_state == DEVICE_IDENTITY_OK) ? (s_rec.seq + 1U) : 1U;
    target = NVSTORE_IDENTITY_ADDR;
    if ((s_state == DEVICE_IDENTITY_OK) && (s_newestIsB == 0U))
    {
        target += NVSTORE_SECTOR_BYTES;
    }
    Encode(r, raw);

    NvStore_IdentityWriteEnable(1U);
    st = NvStore_EraseSector(target);
    if (st == NVSTORE_OK)
    {
        st = NvStore_Program(target, raw, REC_BYTES);
    }
    NvStore_IdentityWriteEnable(0U);

    Load();    /* the stored state is the truth, whatever happened */
    return (st == NVSTORE_OK) ? DEVID_OK : DEVID_ERR_FLASH;
}

void DeviceIdentity_Init(void)
{
    Load();
}

device_identity_state_t DeviceIdentity_State(void)
{
    return s_state;
}

const char *DeviceIdentity_StateName(void)
{
    switch (s_state)
    {
        case DEVICE_IDENTITY_UNSET:   return "unset";
        case DEVICE_IDENTITY_OK:      return "ok";
        case DEVICE_IDENTITY_CORRUPT: return "corrupt";
        default:                      return "nochip";
    }
}

const char *DeviceIdentity_Serial(void)
{
    return (s_state == DEVICE_IDENTITY_OK) ? s_rec.serial : "UNSET";
}

const char *DeviceIdentity_Label(void)
{
    return (s_state == DEVICE_IDENTITY_OK) ? s_rec.label : "";
}

uint8_t DeviceIdentity_Verified(void)
{
    device_hw_id_t hw;

    if (s_state != DEVICE_IDENTITY_OK)
    {
        return 0U;
    }
    DeviceIdentity_ReadHardware(&hw);
    return ((hw.flashUidValid != 0U) && (memcmp(hw.uid, s_rec.uid, sizeof(hw.uid)) == 0) &&
            (memcmp(hw.flashUid, s_rec.flashUid, sizeof(hw.flashUid)) == 0)) ? 1U : 0U;
}

/* ---- format checks and changes ----------------------------------------- */

uint8_t DeviceIdentity_SerialIsValid(const char *serial)
{
    size_t n = (serial != NULL) ? strlen(serial) : 0U;

    if ((n < 3U) || (n > DEVICE_SERIAL_MAX) || (strcmp(serial, "UNSET") == 0))
    {
        return 0U;
    }
    for (size_t i = 0U; i < n; i++)
    {
        char c = serial[i];
        if (!(((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '-')))
        {
            return 0U;
        }
    }
    return 1U;
}

uint8_t DeviceIdentity_LabelIsValid(const char *label)
{
    size_t n = (label != NULL) ? strlen(label) : 0U;

    if ((n < 1U) || (n > DEVICE_LABEL_MAX))
    {
        return 0U;
    }
    for (size_t i = 0U; i < n; i++)
    {
        char c = label[i];
        if (!(((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) ||
              (c == '.') || (c == '_') || (c == '-')))
        {
            return 0U;
        }
    }
    return 1U;
}

devid_result_t DeviceIdentity_SetSerial(const char *serial)
{
    device_hw_id_t hw;
    record_t r;

    if (DeviceIdentity_SerialIsValid(serial) == 0U)
    {
        return DEVID_ERR_BAD_SERIAL;
    }
    if (s_state == DEVICE_IDENTITY_OK)
    {
        return DEVID_ERR_ALREADY_SET;
    }
    if (s_state == DEVICE_IDENTITY_NOCHIP)
    {
        return DEVID_ERR_FLASH;
    }
    DeviceIdentity_ReadHardware(&hw);
    if (hw.flashUidValid == 0U)
    {
        return DEVID_ERR_NO_HWID;
    }
    memset(&r, 0, sizeof(r));
    memcpy(r.serial, serial, strlen(serial));
    memcpy(r.uid, hw.uid, sizeof(r.uid));
    memcpy(r.flashUid, hw.flashUid, sizeof(r.flashUid));
    return Write(&r);
}

devid_result_t DeviceIdentity_SetLabel(const char *label)
{
    record_t r;

    if ((strcmp(label, "-") != 0) && (DeviceIdentity_LabelIsValid(label) == 0U))
    {
        return DEVID_ERR_BAD_LABEL;
    }
    if (s_state != DEVICE_IDENTITY_OK)
    {
        return DEVID_ERR_NOT_SET;
    }
    r = s_rec;
    memset(r.label, 0, sizeof(r.label));
    if (strcmp(label, "-") != 0)
    {
        memcpy(r.label, label, strlen(label));
    }
    return Write(&r);
}

devid_result_t DeviceIdentity_Clear(void)
{
    nvstore_status_t st;

    NvStore_IdentityWriteEnable(1U);
    st = NvStore_EraseSector(NVSTORE_IDENTITY_ADDR);
    if (st == NVSTORE_OK)
    {
        st = NvStore_EraseSector(NVSTORE_IDENTITY_ADDR + NVSTORE_SECTOR_BYTES);
    }
    NvStore_IdentityWriteEnable(0U);
    Load();
    return (st == NVSTORE_OK) ? DEVID_OK : DEVID_ERR_FLASH;
}

/* ---- text -------------------------------------------------------------- */

void DeviceIdentity_FormatUid(const device_hw_id_t *hw, char out[DEVICE_UID_HEX_CHARS + 1U])
{
    (void)snprintf(out, DEVICE_UID_HEX_CHARS + 1U, "%08lX%08lX%08lX",
                   (unsigned long)hw->uid[0], (unsigned long)hw->uid[1], (unsigned long)hw->uid[2]);
}

void DeviceIdentity_FormatFlashUid(const device_hw_id_t *hw, char out[DEVICE_FLASH_UID_HEX_CHARS + 1U])
{
    static const char kHex[] = "0123456789ABCDEF";

    for (uint8_t i = 0U; i < 8U; i++)
    {
        out[2U * i]      = kHex[(hw->flashUid[i] >> 4) & 0x0FU];
        out[2U * i + 1U] = kHex[hw->flashUid[i] & 0x0FU];
    }
    out[DEVICE_FLASH_UID_HEX_CHARS] = '\0';
}

static size_t Clip(int len, size_t n)
{
    return (len < 0) ? 0U : (((size_t)len < n) ? (size_t)len : (n - 1U));
}

size_t DeviceIdentity_FormatTag(char *buf, size_t n)
{
    device_hw_id_t hw;
    char uid[DEVICE_UID_HEX_CHARS + 1U];

    if (n == 0U)
    {
        return 0U;
    }
    DeviceIdentity_ReadHardware(&hw);
    DeviceIdentity_FormatUid(&hw, uid);
    return Clip(snprintf(buf, n, "SN=%s UID=%s", DeviceIdentity_Serial(), uid), n);
}

size_t DeviceIdentity_FormatId(char *buf, size_t n, const char *product, const char *hwRev,
                               const char *fwVersion, const char *commit)
{
    device_hw_id_t hw;
    char uid[DEVICE_UID_HEX_CHARS + 1U];
    char fuid[DEVICE_FLASH_UID_HEX_CHARS + 1U];
    const char *label = DeviceIdentity_Label();

    if (n == 0U)
    {
        return 0U;
    }
    DeviceIdentity_ReadHardware(&hw);
    DeviceIdentity_FormatUid(&hw, uid);
    DeviceIdentity_FormatFlashUid(&hw, fuid);
    return Clip(snprintf(buf, n,
                         "serial=%s uid=%s flashuid=%s product=%s hw=%s fw=%s commit=%s label=%s verified=%u identity=%s",
                         DeviceIdentity_Serial(), uid, fuid, product, hwRev, fwVersion, commit,
                         (label[0] != '\0') ? label : "-", (unsigned)DeviceIdentity_Verified(),
                         DeviceIdentity_StateName()), n);
}
