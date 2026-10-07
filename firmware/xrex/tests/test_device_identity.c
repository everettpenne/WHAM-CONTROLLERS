/*
 * test_device_identity.c -- the hardware-identity formatting (src/app/settings/device_identity.c) on the host,
 * with the MCU ID and the flash ID faked. Shared byte-for-byte by both products.
 */
#include "device_identity.h"
#include "device_id.h"
#include "nvstore.h"
#include "test_util.h"
#include <string.h>

/* ---- fakes ------------------------------------------------------------- */
static uint32_t         f_uid[3]     = { 0x00112233U, 0xAABBCCDDU, 0x0000BEEFU };
static uint8_t          f_fuid[8]    = { 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05, 0xFE, 0x10 };
static nvstore_status_t f_fuidStatus = NVSTORE_OK;
static int              f_fuidReads;

void DeviceId_ReadMcuUid(uint32_t uid[3]) { memcpy(uid, f_uid, sizeof(f_uid)); }
nvstore_status_t NvStore_ReadUniqueId(uint8_t id[8])
{
    f_fuidReads++;
    if (f_fuidStatus == NVSTORE_OK) { memcpy(id, f_fuid, 8); }
    return f_fuidStatus;
}

/* ---- tests ------------------------------------------------------------- */
/* Runs first: the module keeps the flash ID once it has a good one, so the failures come before it. */
static void TestChipAbsentOrUnplausible(void)
{
    device_hw_id_t hw;
    char fuid[DEVICE_FLASH_UID_HEX_CHARS + 1U];

    f_fuidStatus = NVSTORE_ERR_NOT_READY;
    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 0U);
    DeviceIdentity_FormatFlashUid(&hw, fuid);
    CHECK(strcmp(fuid, "0000000000000000") == 0);

    f_fuidStatus = NVSTORE_OK;                       /* answered, but with what an empty bus reads */
    memset(f_fuid, 0xFF, sizeof(f_fuid));
    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 0U);
    memset(f_fuid, 0x00, sizeof(f_fuid));
    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 0U);

    { const uint8_t good[8] = { 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05, 0xFE, 0x10 };
      memcpy(f_fuid, good, 8); }                     /* the chip comes up: it is asked again */
    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 1U);
}

static void TestFormats(void)
{
    device_hw_id_t hw;
    char uid[DEVICE_UID_HEX_CHARS + 1U], fuid[DEVICE_FLASH_UID_HEX_CHARS + 1U];

    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 1U);
    DeviceIdentity_FormatUid(&hw, uid);
    DeviceIdentity_FormatFlashUid(&hw, fuid);
    CHECK(strcmp(uid, "00112233AABBCCDD0000BEEF") == 0);
    CHECK(strcmp(fuid, "EF0102030405FE10") == 0);
    CHECK(strlen(uid) == DEVICE_UID_HEX_CHARS && strlen(fuid) == DEVICE_FLASH_UID_HEX_CHARS);
}

static void TestTagAndId(void)
{
    char buf[200];
    size_t n = DeviceIdentity_FormatTag(buf, sizeof(buf));
    CHECK(strcmp(buf, "SN=UNSET UID=00112233AABBCCDD0000BEEF") == 0);
    CHECK(n == strlen(buf));

    n = DeviceIdentity_FormatId(buf, sizeof(buf), "WHAM-X", "REVA", "v1.2", "abc123-dirty");
    CHECK(strcmp(buf, "serial=UNSET uid=00112233AABBCCDD0000BEEF flashuid=EF0102030405FE10 "
                      "product=WHAM-X hw=REVA fw=v1.2 commit=abc123-dirty label=- verified=0") == 0);
    CHECK(n == strlen(buf));
}

static void TestTruncation(void)
{
    char buf[16];
    size_t n = DeviceIdentity_FormatTag(buf, sizeof(buf));
    CHECK(n == sizeof(buf) - 1U && strlen(buf) == n);                  /* cut, still terminated */
    CHECK(DeviceIdentity_FormatTag(buf, 0U) == 0U);
    CHECK(DeviceIdentity_FormatId(buf, 1U, "p", "h", "f", "c") == 0U && buf[0] == '\0');
}

static void TestFlashIdCachedOnce(void)
{
    device_hw_id_t hw;
    f_fuidReads = 0;
    f_fuidStatus = NVSTORE_ERR_IO;                 /* a later failure does not lose the ID already read */
    DeviceIdentity_ReadHardware(&hw);
    DeviceIdentity_ReadHardware(&hw);
    CHECK(f_fuidReads == 0 && hw.flashUidValid == 1U);
}

int main(void)
{
    TestChipAbsentOrUnplausible();
    TestFormats();
    TestTagAndId();
    TestTruncation();
    TestFlashIdCachedOnce();
    return test_report("test_device_identity");
}
