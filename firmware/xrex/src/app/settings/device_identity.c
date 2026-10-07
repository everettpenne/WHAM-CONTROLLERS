/*
 * device_identity.c -- see device_identity.h.
 */
#include "device_identity.h"
#include "device_id.h"
#include "nvstore.h"
#include <stdio.h>
#include <string.h>

static uint8_t s_flashUid[8];
static uint8_t s_flashUidValid = 0U;

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

const char *DeviceIdentity_Serial(void)
{
    return "UNSET";
}

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

size_t DeviceIdentity_FormatTag(char *buf, size_t n)
{
    device_hw_id_t hw;
    char uid[DEVICE_UID_HEX_CHARS + 1U];
    int len;

    if (n == 0U)
    {
        return 0U;
    }
    DeviceIdentity_ReadHardware(&hw);
    DeviceIdentity_FormatUid(&hw, uid);
    len = snprintf(buf, n, "SN=%s UID=%s", DeviceIdentity_Serial(), uid);
    return (len < 0) ? 0U : (((size_t)len < n) ? (size_t)len : (n - 1U));
}

size_t DeviceIdentity_FormatId(char *buf, size_t n, const char *product, const char *hwRev,
                               const char *fwVersion, const char *commit)
{
    device_hw_id_t hw;
    char uid[DEVICE_UID_HEX_CHARS + 1U];
    char fuid[DEVICE_FLASH_UID_HEX_CHARS + 1U];
    int len;

    if (n == 0U)
    {
        return 0U;
    }
    DeviceIdentity_ReadHardware(&hw);
    DeviceIdentity_FormatUid(&hw, uid);
    DeviceIdentity_FormatFlashUid(&hw, fuid);
    len = snprintf(buf, n, "serial=%s uid=%s flashuid=%s product=%s hw=%s fw=%s commit=%s label=- verified=0",
                   DeviceIdentity_Serial(), uid, fuid, product, hwRev, fwVersion, commit);
    return (len < 0) ? 0U : (((size_t)len < n) ? (size_t)len : (n - 1U));
}
