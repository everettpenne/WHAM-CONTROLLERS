/*
 * test_device_identity.c -- the device identity (src/app/settings/device_identity.c) on the host: hardware-ID
 * formatting, the persistent serial/label record, power cuts at every point, corruption, and the flash guard,
 * against the RAM flash model (fake_nvstore). Shared byte-for-byte by both products.
 */
#include "device_identity.h"
#include "device_id.h"
#include "nvstore.h"
#include "fake_nvstore.h"
#include "test_util.h"
#include <string.h>

/* ---- fakes ------------------------------------------------------------- */
static uint32_t f_uid[3] = { 0x00112233U, 0xAABBCCDDU, 0x0000BEEFU };
static const uint8_t kChipA[8] = { 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05, 0xFE, 0x10 };
static const uint8_t kChipB[8] = { 0x00, 0x00, 0x00, 0x00, 0xE4, 0x65, 0x3C, 0x34 };

void DeviceId_ReadMcuUid(uint32_t uid[3]) { memcpy(uid, f_uid, sizeof(f_uid)); }

#define A (NVSTORE_IDENTITY_ADDR)
#define B (NVSTORE_IDENTITY_ADDR + NVSTORE_SECTOR_BYTES)

/* A fresh board: blank flash, this MCU, this chip. The module keeps the flash ID once good, so a test that
   needs a different chip says so (ChipChange). */
static void Boot(void)
{
    DeviceIdentity_Init();
}

static void FreshBoard(void)
{
    FakeNv_Reset();
    FakeNv_SetUniqueId(kChipA, NVSTORE_OK);
    f_uid[0] = 0x00112233U; f_uid[1] = 0xAABBCCDDU; f_uid[2] = 0x0000BEEFU;
    Boot();
}

/* ---- tests ------------------------------------------------------------- */
/* Runs first: the module keeps the flash ID once it has a good one, so the failures come before it. */
static void TestChipAbsentOrUnplausible(void)
{
    device_hw_id_t hw;
    char fuid[DEVICE_FLASH_UID_HEX_CHARS + 1U];

    FakeNv_Reset();
    FakeNv_SetUniqueId(kChipA, NVSTORE_ERR_NOT_READY);
    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 0U);
    DeviceIdentity_FormatFlashUid(&hw, fuid);
    CHECK(strcmp(fuid, "0000000000000000") == 0);
    CHECK(DeviceIdentity_SetSerial("WB-0001") == DEVID_ERR_NO_HWID);      /* nothing to bind to */

    { uint8_t ones[8]; memset(ones, 0xFF, 8); FakeNv_SetUniqueId(ones, NVSTORE_OK); }
    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 0U);
    { uint8_t zeros[8]; memset(zeros, 0, 8); FakeNv_SetUniqueId(zeros, NVSTORE_OK); }
    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 0U);

    FakeNv_SetUniqueId(kChipA, NVSTORE_OK);          /* the chip comes up: it is asked again */
    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 1U);
}

static void TestFormats(void)
{
    device_hw_id_t hw;
    char uid[DEVICE_UID_HEX_CHARS + 1U], fuid[DEVICE_FLASH_UID_HEX_CHARS + 1U], buf[240];
    size_t n;

    FreshBoard();
    DeviceIdentity_ReadHardware(&hw);
    DeviceIdentity_FormatUid(&hw, uid);
    DeviceIdentity_FormatFlashUid(&hw, fuid);
    CHECK(strcmp(uid, "00112233AABBCCDD0000BEEF") == 0);
    CHECK(strcmp(fuid, "EF0102030405FE10") == 0);

    n = DeviceIdentity_FormatTag(buf, sizeof(buf));
    CHECK(strcmp(buf, "SN=UNSET UID=00112233AABBCCDD0000BEEF") == 0 && n == strlen(buf));

    n = DeviceIdentity_FormatId(buf, sizeof(buf), "WHAM-X", "REVA", "v1.2", "abc123-dirty");
    CHECK(strcmp(buf, "serial=UNSET uid=00112233AABBCCDD0000BEEF flashuid=EF0102030405FE10 product=WHAM-X "
                      "hw=REVA fw=v1.2 commit=abc123-dirty label=- verified=0 identity=unset") == 0);
    CHECK(n == strlen(buf));

    CHECK(DeviceIdentity_SetSerial("WB-0007-K") == DEVID_OK);
    CHECK(DeviceIdentity_SetLabel("magnet.2_east") == DEVID_OK);
    (void)DeviceIdentity_FormatId(buf, sizeof(buf), "WHAM-X", "REVA", "v1.2", "abc123");
    CHECK(strcmp(buf, "serial=WB-0007-K uid=00112233AABBCCDD0000BEEF flashuid=EF0102030405FE10 product=WHAM-X "
                      "hw=REVA fw=v1.2 commit=abc123 label=magnet.2_east verified=1 identity=ok") == 0);
    (void)DeviceIdentity_FormatTag(buf, sizeof(buf));
    CHECK(strcmp(buf, "SN=WB-0007-K UID=00112233AABBCCDD0000BEEF") == 0);
}

static void TestTruncation(void)
{
    char buf[16];
    size_t n = DeviceIdentity_FormatTag(buf, sizeof(buf));
    CHECK(n == sizeof(buf) - 1U && strlen(buf) == n);                  /* cut, still terminated */
    CHECK(DeviceIdentity_FormatTag(buf, 0U) == 0U);
    CHECK(DeviceIdentity_FormatId(buf, 1U, "p", "h", "f", "c") == 0U && buf[0] == '\0');
}

static void TestValidation(void)
{
    CHECK(DeviceIdentity_SerialIsValid("WB-0007-K") == 1U);
    CHECK(DeviceIdentity_SerialIsValid("ABC") == 1U);
    CHECK(DeviceIdentity_SerialIsValid("0123456789ABCDEF") == 1U);       /* 16 characters */
    CHECK(DeviceIdentity_SerialIsValid("0123456789ABCDEFG") == 0U);      /* 17 */
    CHECK(DeviceIdentity_SerialIsValid("AB") == 0U);
    CHECK(DeviceIdentity_SerialIsValid("") == 0U);
    CHECK(DeviceIdentity_SerialIsValid("wb-0007") == 0U);                /* lower case */
    CHECK(DeviceIdentity_SerialIsValid("WB 0007") == 0U);
    CHECK(DeviceIdentity_SerialIsValid("WB_0007") == 0U);
    CHECK(DeviceIdentity_SerialIsValid("UNSET") == 0U);
    CHECK(DeviceIdentity_SerialIsValid(NULL) == 0U);
    CHECK(DeviceIdentity_LabelIsValid("a.b_c-D9") == 1U);
    CHECK(DeviceIdentity_LabelIsValid("") == 0U);
    CHECK(DeviceIdentity_LabelIsValid("has space") == 0U);
    CHECK(DeviceIdentity_LabelIsValid("12345678901234567890123456789012") == 1U);   /* 32 */
    CHECK(DeviceIdentity_LabelIsValid("123456789012345678901234567890123") == 0U);  /* 33 */
}

static void TestProvisionAndReboot(void)
{
    FreshBoard();
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_UNSET);
    CHECK(strcmp(DeviceIdentity_Serial(), "UNSET") == 0 && DeviceIdentity_Verified() == 0U);
    CHECK(DeviceIdentity_SetLabel("x") == DEVID_ERR_NOT_SET);            /* a label needs a serial */
    CHECK(DeviceIdentity_SetSerial("bad serial") == DEVID_ERR_BAD_SERIAL);
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_UNSET);              /* nothing was written */

    CHECK(DeviceIdentity_SetSerial("WB-0007-K") == DEVID_OK);
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_OK);
    CHECK(strcmp(DeviceIdentity_Serial(), "WB-0007-K") == 0 && DeviceIdentity_Verified() == 1U);
    CHECK(strcmp(DeviceIdentity_Label(), "") == 0);
    CHECK(DeviceIdentity_SetSerial("WB-0008-L") == DEVID_ERR_ALREADY_SET);
    CHECK(strcmp(DeviceIdentity_Serial(), "WB-0007-K") == 0);

    Boot();                                                              /* power cycle */
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_OK && strcmp(DeviceIdentity_Serial(), "WB-0007-K") == 0);

    CHECK(DeviceIdentity_SetLabel("bad label") == DEVID_ERR_BAD_LABEL);
    CHECK(DeviceIdentity_SetLabel("lab.A") == DEVID_OK);
    Boot();
    CHECK(strcmp(DeviceIdentity_Label(), "lab.A") == 0 && strcmp(DeviceIdentity_Serial(), "WB-0007-K") == 0);
    CHECK(DeviceIdentity_SetLabel("-") == DEVID_OK);                     /* "-" clears the label */
    CHECK(strcmp(DeviceIdentity_Label(), "") == 0);

    CHECK(DeviceIdentity_Clear() == DEVID_OK);
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_UNSET && strcmp(DeviceIdentity_Serial(), "UNSET") == 0);
    Boot();
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_UNSET);
    CHECK(DeviceIdentity_SetSerial("WB-0009-M") == DEVID_OK);            /* provisioning again after a clear */
    CHECK(strcmp(DeviceIdentity_Serial(), "WB-0009-M") == 0);
}

static uint32_t SeqAt(uint32_t addr)
{
    uint8_t b[16];
    (void)NvStore_Read(addr, b, sizeof(b));
    return (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
}

static void TestCopiesAlternate(void)
{
    FreshBoard();
    CHECK(DeviceIdentity_SetSerial("WB-0001") == DEVID_OK);
    CHECK(SeqAt(A) == 1U);                                               /* first record: copy A */
    CHECK(DeviceIdentity_SetLabel("one") == DEVID_OK);
    CHECK(SeqAt(A) == 1U && SeqAt(B) == 2U);                             /* the update went to B; A is untouched */
    CHECK(DeviceIdentity_SetLabel("two") == DEVID_OK);
    CHECK(SeqAt(A) == 3U && SeqAt(B) == 2U);
    Boot();
    CHECK(strcmp(DeviceIdentity_Label(), "two") == 0);                   /* the newest wins after a reboot */
}

/* A power cut at every point of a label update: the serial is never lost, and the label is the old or the new one. */
static void TestPowerCutDuringUpdate(void)
{
    uint32_t total;

    FreshBoard();
    CHECK(DeviceIdentity_SetSerial("WB-0002") == DEVID_OK);
    CHECK(DeviceIdentity_SetLabel("old") == DEVID_OK);
    FakeNv_CutPowerAfter(100000U);
    CHECK(DeviceIdentity_SetLabel("new") == DEVID_OK);
    total = FakeNv_UnitsUsed();
    CHECK(total > 80U);                                                  /* an erase and 88 programmed bytes */

    for (uint32_t cut = 0U; cut < total; cut++)
    {
        FreshBoard();
        (void)DeviceIdentity_SetSerial("WB-0002");
        (void)DeviceIdentity_SetLabel("old");
        FakeNv_CutPowerAfter(cut);
        (void)DeviceIdentity_SetLabel("new");          /* fails somewhere inside */
        FakeNv_RestorePower();
        Boot();
        if (DeviceIdentity_State() != DEVICE_IDENTITY_OK || strcmp(DeviceIdentity_Serial(), "WB-0002") != 0 ||
            (strcmp(DeviceIdentity_Label(), "old") != 0 && strcmp(DeviceIdentity_Label(), "new") != 0))
        {
            CHECK(0 && "identity lost after a power cut during a label update");
            break;
        }
    }
    CHECK(1);
}

/* A power cut during the very first write leaves nothing usable, but never a wrong serial; provisioning again works. */
static void TestPowerCutDuringFirstWrite(void)
{
    for (uint32_t cut = 0U; cut < 120U; cut++)
    {
        FreshBoard();
        FakeNv_CutPowerAfter(cut);
        (void)DeviceIdentity_SetSerial("WB-0003");
        FakeNv_RestorePower();
        Boot();
        if (DeviceIdentity_State() == DEVICE_IDENTITY_OK)
        {
            if (strcmp(DeviceIdentity_Serial(), "WB-0003") != 0) { CHECK(0 && "wrong serial after a cut"); break; }
        }
        else if (DeviceIdentity_State() != DEVICE_IDENTITY_UNSET && DeviceIdentity_State() != DEVICE_IDENTITY_CORRUPT)
        {
            CHECK(0 && "unexpected state after a cut");
            break;
        }
        CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_OK || DeviceIdentity_SetSerial("WB-0003") == DEVID_OK);
        CHECK(strcmp(DeviceIdentity_Serial(), "WB-0003") == 0);
    }
}

static void TestCorruption(void)
{
    FreshBoard();
    (void)DeviceIdentity_SetSerial("WB-0004");
    (void)DeviceIdentity_SetLabel("keep");                               /* A: seq 1 (no label), B: seq 2 (label) */
    FakeNv_Poke(B + 13U, 0x01U);                                         /* damage the newest copy */
    Boot();
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_OK);
    CHECK(strcmp(DeviceIdentity_Serial(), "WB-0004") == 0 && strcmp(DeviceIdentity_Label(), "") == 0);   /* fell back to A */
    FakeNv_Poke(A + 40U, 0x80U);                                         /* and the other one */
    Boot();
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_CORRUPT && strcmp(DeviceIdentity_Serial(), "UNSET") == 0);
    CHECK(strcmp(DeviceIdentity_StateName(), "corrupt") == 0);
    CHECK(DeviceIdentity_SetSerial("WB-0004") == DEVID_OK);              /* re-provisioning is the recovery */
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_OK);
}

static void TestVerified(void)
{
    FreshBoard();
    (void)DeviceIdentity_SetSerial("WB-0005");
    CHECK(DeviceIdentity_Verified() == 1U);
    f_uid[1] ^= 1U;                                                      /* the image on a different MCU */
    CHECK(DeviceIdentity_Verified() == 0U);
    CHECK(strcmp(DeviceIdentity_Serial(), "WB-0005") == 0);              /* still reported, but unverified */
    f_uid[1] ^= 1U;
    CHECK(DeviceIdentity_Verified() == 1U);
}

static void TestFlashGuard(void)
{
    uint8_t x = 0x00U;

    FreshBoard();
    CHECK(NvStore_EraseSector(A) == NVSTORE_ERR_RESERVED);               /* nobody else may touch the identity sectors */
    CHECK(NvStore_Program(B + 4U, &x, 1U) == NVSTORE_ERR_RESERVED);
    CHECK(NvStore_Program(A - 1U, &x, 2U) == NVSTORE_ERR_RESERVED);      /* straddling the edge */
    CHECK(NvStore_EraseSector(A - NVSTORE_SECTOR_BYTES) == NVSTORE_OK);  /* the sector below is free */
    CHECK(NvStore_EraseSector(NVSTORE_SCRATCH_ADDR) == NVSTORE_OK);      /* the scratch sector is free */
    CHECK(DeviceIdentity_SetSerial("WB-0006") == DEVID_OK);              /* the module itself may ... */
    CHECK(NvStore_EraseSector(A) == NVSTORE_ERR_RESERVED);               /* ... and the guard is closed again after it */
    CHECK(NVSTORE_IDENTITY_ADDR + NVSTORE_IDENTITY_BYTES <= NVSTORE_SCRATCH_ADDR);
}

static void TestChipMissingAtBoot(void)
{
    FreshBoard();
    (void)DeviceIdentity_SetSerial("WB-0010");
    FakeNv_CutPowerAfter(0U);
    (void)NvStore_EraseSector(NVSTORE_SCRATCH_ADDR);                     /* kills the fake chip */
    Boot();
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_NOCHIP && strcmp(DeviceIdentity_Serial(), "UNSET") == 0);
    CHECK(strcmp(DeviceIdentity_StateName(), "nochip") == 0);
    CHECK(DeviceIdentity_SetSerial("WB-0010") == DEVID_ERR_FLASH);
    FakeNv_RestorePower();
    Boot();
    CHECK(DeviceIdentity_State() == DEVICE_IDENTITY_OK);
}

static void TestFlashIdKeptOnceRead(void)
{
    device_hw_id_t hw;
    FreshBoard();
    DeviceIdentity_ReadHardware(&hw);
    FakeNv_SetUniqueId(kChipB, NVSTORE_ERR_IO);        /* a later failure does not lose the ID already read */
    DeviceIdentity_ReadHardware(&hw);
    CHECK(hw.flashUidValid == 1U && memcmp(hw.flashUid, kChipA, 8) == 0);
}

int main(void)
{
    TestChipAbsentOrUnplausible();
    TestFormats();
    TestTruncation();
    TestValidation();
    TestProvisionAndReboot();
    TestCopiesAlternate();
    TestPowerCutDuringUpdate();
    TestPowerCutDuringFirstWrite();
    TestCorruption();
    TestVerified();
    TestFlashGuard();
    TestChipMissingAtBoot();
    TestFlashIdKeptOnceRead();
    return test_report("test_device_identity");
}
