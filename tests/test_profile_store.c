/*
 * test_profile_store.c -- slot storage (src/app/settings/profile_store.c)
 * against the RAM fake flash, including a power cut at every possible
 * point of a save and of a clear.
 */
#include "profile_store.h"
#include "fake_nvstore.h"
#include "test_util.h"
#include <string.h>

static void Payload(uint8_t *p, uint16_t n, uint8_t seed)
{
    for (uint16_t i = 0U; i < n; i++)
    {
        p[i] = (uint8_t)(seed + i * 3U);
    }
}

static int ReadIs(uint8_t slot, const uint8_t *want, uint16_t n)
{
    uint8_t buf[PROFILE_PAYLOAD_MAX];
    uint16_t len = 0U;

    if (ProfileStore_Read(slot, buf, sizeof(buf), &len, NULL) != PROFILE_OK)
    {
        return 0;
    }
    return len == n && memcmp(buf, want, n) == 0;
}

static void TestBasic(void)
{
    uint8_t a[300], b[120], buf[PROFILE_PAYLOAD_MAX];
    uint16_t len;
    profile_info_t info;

    FakeNv_Reset();
    Payload(a, sizeof(a), 1U);
    Payload(b, sizeof(b), 77U);

    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_EMPTY);
    CHECK(ProfileStore_Read(0U, buf, sizeof(buf), &len, NULL) == PROFILE_ERR_EMPTY);

    CHECK(ProfileStore_Write(0U, "first", "v0.8i", a, sizeof(a)) == PROFILE_OK);
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK);
    CHECK(info.state == PROFILE_SLOT_VALID && info.seq == 1U && info.payloadLen == sizeof(a));
    CHECK_STR(info.label, "first");
    CHECK_STR(info.fw, "v0.8i");
    CHECK(ReadIs(0U, a, sizeof(a)));

    CHECK(ProfileStore_Write(0U, "second", "v0.8j", b, sizeof(b)) == PROFILE_OK);
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.seq == 2U);
    CHECK_STR(info.label, "second");
    CHECK(ReadIs(0U, b, sizeof(b)));

    /* other slots are independent */
    CHECK(ProfileStore_Info(3U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_EMPTY);
    CHECK(ProfileStore_Write(3U, NULL, NULL, a, sizeof(a)) == PROFILE_OK);
    CHECK(ProfileStore_Clear(0U) == PROFILE_OK);
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_EMPTY);
    CHECK(ReadIs(3U, a, sizeof(a)));
    CHECK(ProfileStore_Info(3U, &info) == PROFILE_OK && info.label[0] == '\0');

    /* a cleared slot starts over at sequence 1 */
    CHECK(ProfileStore_Write(0U, "again", "v", b, sizeof(b)) == PROFILE_OK);
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.seq == 1U);
}

static void TestLimits(void)
{
    uint8_t big[PROFILE_PAYLOAD_MAX + 1U], buf[8];
    uint16_t len;
    profile_info_t info;
    uint8_t p[100];

    FakeNv_Reset();
    memset(big, 1, sizeof(big));
    Payload(p, sizeof(p), 5U);
    CHECK(ProfileStore_Write(PROFILE_SLOT_COUNT, "x", "y", p, 10U) == PROFILE_ERR_SLOT);
    CHECK(ProfileStore_Info(PROFILE_SLOT_COUNT, &info) == PROFILE_ERR_SLOT);
    CHECK(ProfileStore_Clear(PROFILE_SLOT_COUNT) == PROFILE_ERR_SLOT);
    CHECK(ProfileStore_Read(PROFILE_SLOT_COUNT, buf, sizeof(buf), &len, NULL) == PROFILE_ERR_SLOT);
    CHECK(ProfileStore_Write(0U, "x", "y", big, sizeof(big)) == PROFILE_ERR_TOO_BIG);
    CHECK(ProfileStore_Write(0U, "x", "y", big, PROFILE_PAYLOAD_MAX) == PROFILE_OK);
    CHECK(ProfileStore_Read(0U, buf, sizeof(buf), &len, NULL) == PROFILE_ERR_TOO_BIG);   /* caller buffer too small */
    /* over-long strings are truncated, not overrun */
    CHECK(ProfileStore_Write(1U, "0123456789ABCDEFGHIJ", "fwfwfwfwfwfwfwfwfwfw", p, 10U) == PROFILE_OK);
    CHECK(ProfileStore_Info(1U, &info) == PROFILE_OK);
    CHECK_STR(info.label, "0123456789ABCDE");
    CHECK_STR(info.fw, "fwfwfwfwfwfwfwf");
    /* an empty payload is legal */
    CHECK(ProfileStore_Write(2U, "e", "y", NULL, 0U) == PROFILE_OK);
    CHECK(ProfileStore_Info(2U, &info) == PROFILE_OK && info.payloadLen == 0U);
}

static void TestCorruption(void)
{
    uint8_t a[64], b[64];
    profile_info_t info;
    uint32_t newestPayloadAddr;

    FakeNv_Reset();
    Payload(a, sizeof(a), 9U);
    Payload(b, sizeof(b), 40U);

    /* one copy only, damaged: corrupt, not empty */
    CHECK(ProfileStore_Write(0U, "a", "v", a, sizeof(a)) == PROFILE_OK);
    FakeNv_Poke(0U * 2U * NVSTORE_SECTOR_BYTES + 48U + 5U, 0x01U);   /* copy 0 payload */
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_CORRUPT);
    CHECK(ProfileStore_Read(0U, a, sizeof(a), &(uint16_t){0}, NULL) == PROFILE_ERR_CORRUPT);
    /* ...and it can be overwritten or cleared */
    CHECK(ProfileStore_Write(0U, "b", "v", b, sizeof(b)) == PROFILE_OK);
    CHECK(ReadIs(0U, b, sizeof(b)));

    /* two copies, the newest damaged: the older one is used */
    CHECK(ProfileStore_Write(0U, "c", "v", a, sizeof(a)) == PROFILE_OK);   /* seq 2, in copy 1; b (seq 1) stays in copy 0 */
    newestPayloadAddr = (0U * 2U + 1U) * NVSTORE_SECTOR_BYTES + 48U + 10U;
    FakeNv_Poke(newestPayloadAddr, 0x80U);
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_VALID && info.seq == 1U);
    CHECK(ReadIs(0U, b, sizeof(b)));

    /* a header from another version is not trusted */
    FakeNv_Reset();
    CHECK(ProfileStore_Write(0U, "a", "v", a, sizeof(a)) == PROFILE_OK);
    FakeNv_Poke(4U, 0x02U);   /* format version */
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_CORRUPT);
    CHECK(ProfileStore_Clear(0U) == PROFILE_OK);
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_EMPTY);
}

/* Power cut at every unit of a save: afterwards the slot holds exactly the
   old payload or exactly the new one, and if the save reported success it
   holds the new one. */
static void SaveCutSweep(uint8_t priorWrites)
{
    uint8_t oldP[200], newP[350];
    uint32_t total;

    Payload(oldP, sizeof(oldP), 3U);
    Payload(newP, sizeof(newP), 99U);

    /* dry run to count the units a save consumes in this state */
    FakeNv_Reset();
    for (uint8_t i = 0U; i < priorWrites; i++)
    {
        CHECK(ProfileStore_Write(2U, "old", "v", oldP, sizeof(oldP)) == PROFILE_OK);
    }
    FakeNv_CutPowerAfter(0xFFFFFFFFUL);
    CHECK(ProfileStore_Write(2U, "new", "v", newP, sizeof(newP)) == PROFILE_OK);
    total = FakeNv_UnitsUsed();
    CHECK(total > 0U);

    for (uint32_t cut = 0U; cut <= total; cut++)
    {
        profile_status_t rc;
        int isOld, isNew;

        FakeNv_Reset();
        for (uint8_t i = 0U; i < priorWrites; i++)
        {
            (void)ProfileStore_Write(2U, "old", "v", oldP, sizeof(oldP));
        }
        FakeNv_CutPowerAfter(cut);
        rc = ProfileStore_Write(2U, "new", "v", newP, sizeof(newP));
        FakeNv_RestorePower();

        isOld = (priorWrites != 0U) && ReadIs(2U, oldP, sizeof(oldP));
        isNew = ReadIs(2U, newP, sizeof(newP));
        CHECK(isOld || isNew || (priorWrites == 0U));
        if (rc == PROFILE_OK)
        {
            CHECK(isNew);
        }
        if (priorWrites != 0U)
        {
            CHECK(isOld != isNew);   /* never both, never neither */
        }
        else
        {
            profile_info_t info;
            /* nothing before: empty/corrupt (torn first save) or the new data, never other data */
            CHECK(ProfileStore_Info(2U, &info) == PROFILE_OK);
            CHECK(isNew || info.state != PROFILE_SLOT_VALID);
        }
        /* and the slot is always usable again */
        CHECK(ProfileStore_Write(2U, "after", "v", oldP, sizeof(oldP)) == PROFILE_OK);
        CHECK(ReadIs(2U, oldP, sizeof(oldP)));
    }
}

static void ClearCutSweep(uint8_t priorWrites)
{
    uint8_t p[150];
    uint32_t total;

    Payload(p, sizeof(p), 21U);
    FakeNv_Reset();
    for (uint8_t i = 0U; i < priorWrites; i++)
    {
        (void)ProfileStore_Write(5U, "x", "v", p, sizeof(p));
    }
    FakeNv_CutPowerAfter(0xFFFFFFFFUL);
    CHECK(ProfileStore_Clear(5U) == PROFILE_OK);
    total = FakeNv_UnitsUsed();

    for (uint32_t cut = 0U; cut <= total; cut++)
    {
        profile_info_t info;

        FakeNv_Reset();
        for (uint8_t i = 0U; i < priorWrites; i++)
        {
            (void)ProfileStore_Write(5U, "x", "v", p, sizeof(p));
        }
        FakeNv_CutPowerAfter(cut);
        (void)ProfileStore_Clear(5U);
        FakeNv_RestorePower();

        /* The slot is either still fully intact, or not valid; it never
           shows different data (in particular a cut must not resurrect an
           older save when the newest one is still there). */
        CHECK(ProfileStore_Info(5U, &info) == PROFILE_OK);
        if (info.state == PROFILE_SLOT_VALID)
        {
            CHECK(ReadIs(5U, p, sizeof(p)));
            CHECK(info.seq == priorWrites);
        }
    }
}

int main(void)
{
    TestBasic();
    TestLimits();
    TestCorruption();
    SaveCutSweep(0U);
    SaveCutSweep(1U);
    SaveCutSweep(2U);
    ClearCutSweep(1U);
    ClearCutSweep(2U);
    return test_report("test_profile_store");
}
