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
    FakeNv_Poke(PROFILE_STORE_BASE + 0U * 2U * PROFILE_COPY_BYTES + 48U + 5U, 0x01U);   /* copy 0 payload */
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_CORRUPT);
    CHECK(ProfileStore_Read(0U, a, sizeof(a), &(uint16_t){0}, NULL) == PROFILE_ERR_CORRUPT);
    /* ...and it can be overwritten or cleared */
    CHECK(ProfileStore_Write(0U, "b", "v", b, sizeof(b)) == PROFILE_OK);
    CHECK(ReadIs(0U, b, sizeof(b)));

    /* two copies, the newest damaged: the older one is used */
    CHECK(ProfileStore_Write(0U, "c", "v", a, sizeof(a)) == PROFILE_OK);   /* seq 2, in copy 1; b (seq 1) stays in copy 0 */
    newestPayloadAddr = PROFILE_STORE_BASE + (0U * 2U + 1U) * PROFILE_COPY_BYTES + 48U + 10U;
    FakeNv_Poke(newestPayloadAddr, 0x80U);
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_VALID && info.seq == 1U);
    CHECK(ReadIs(0U, b, sizeof(b)));

    /* a header from another version is not trusted */
    FakeNv_Reset();
    CHECK(ProfileStore_Write(0U, "a", "v", a, sizeof(a)) == PROFILE_OK);
    FakeNv_Poke(PROFILE_STORE_BASE + 4U, 0x02U);   /* format version */
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

/* ---- what is new in this version: multi-sector copies, streaming write and read, the product magic ---- */

static void BigPayload(uint8_t *p, uint16_t n, uint8_t seed)
{
    for (uint16_t i = 0U; i < n; i++)
    {
        p[i] = (uint8_t)(seed + (i * 7U) + (i >> 8));
    }
}

static void TestStreaming(void)
{
    static uint8_t big[10000];
    static uint8_t back[10000];
    profile_writer_t w;
    profile_reader_t r;
    profile_info_t info;
    uint16_t pos = 0U;

    FakeNv_Reset();
    BigPayload(big, sizeof(big), 5U);

    /* written in uneven pieces that cross sector and page boundaries */
    CHECK(ProfileWriter_Begin(&w, 4U, "big", "v0.9", sizeof(big)) == PROFILE_OK);
    while (pos < sizeof(big))
    {
        uint16_t n = (uint16_t)(((pos % 7U) * 211U) + 1U);

        if (n > (uint16_t)(sizeof(big) - pos)) { n = (uint16_t)(sizeof(big) - pos); }
        CHECK(ProfileWriter_Write(&w, &big[pos], n) == PROFILE_OK);
        pos = (uint16_t)(pos + n);
    }
    /* nothing is visible until the header goes in last */
    CHECK(ProfileStore_Info(4U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_EMPTY);
    CHECK(ProfileWriter_End(&w) == PROFILE_OK);
    CHECK(ProfileStore_Info(4U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_VALID && info.payloadLen == sizeof(big));
    CHECK_STR(info.label, "big");

    /* read back whole and in pieces */
    CHECK(ProfileReader_Open(4U, &r, &info) == PROFILE_OK && r.len == sizeof(big));
    CHECK(ProfileReader_Read(&r, 0U, back, sizeof(big)) == PROFILE_OK && memcmp(back, big, sizeof(big)) == 0);
    memset(back, 0, sizeof(back));
    CHECK(ProfileReader_Read(&r, 4090U, back, 20U) == PROFILE_OK && memcmp(back, &big[4090], 20U) == 0);   /* across a sector edge */
    CHECK(ProfileReader_Read(&r, 9990U, back, 10U) == PROFILE_OK && memcmp(back, &big[9990], 10U) == 0);
    CHECK(ProfileReader_Read(&r, 9990U, back, 11U) == PROFILE_ERR_TOO_BIG);                                 /* past the end */
    CHECK(ProfileReader_Open(PROFILE_SLOT_COUNT, &r, NULL) == PROFILE_ERR_SLOT);
    CHECK(ProfileReader_Open(5U, &r, NULL) == PROFILE_ERR_EMPTY);

    /* the buffered API sees the same data */
    CHECK(ReadIs(4U, big, sizeof(big)));

    /* a save is told its length up front: too few or too many bytes is refused, and nothing becomes visible */
    CHECK(ProfileWriter_Begin(&w, 6U, "x", "v", 100U) == PROFILE_OK);
    CHECK(ProfileWriter_Write(&w, big, 99U) == PROFILE_OK);
    CHECK(ProfileWriter_End(&w) == PROFILE_ERR_USAGE);
    CHECK(ProfileWriter_Write(&w, big, 2U) == PROFILE_ERR_USAGE);
    CHECK(ProfileStore_Info(6U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_EMPTY);
    CHECK(ProfileWriter_Begin(&w, 6U, "x", "v", (uint16_t)(PROFILE_PAYLOAD_MAX + 1U)) == PROFILE_ERR_TOO_BIG);

    /* a payload using every sector of the copy */
    {
        static uint8_t full[PROFILE_PAYLOAD_MAX];

        BigPayload(full, PROFILE_PAYLOAD_MAX, 77U);
        CHECK(ProfileStore_Write(7U, "full", "v", full, PROFILE_PAYLOAD_MAX) == PROFILE_OK);
        CHECK(ReadIs(7U, full, PROFILE_PAYLOAD_MAX));
    }

    /* clear a big slot, then a small save into it: the stale bytes beyond the new payload are unreachable */
    CHECK(ProfileStore_Clear(4U) == PROFILE_OK);
    CHECK(ProfileStore_Info(4U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_EMPTY);
    CHECK(ProfileStore_Write(4U, "small", "v", big, 100U) == PROFILE_OK);
    CHECK(ReadIs(4U, big, 100U));
    CHECK(ProfileStore_Write(4U, "small2", "v", &big[1], 100U) == PROFILE_OK);
    CHECK(ReadIs(4U, &big[1], 100U));
}

/* the magic tags the product: another product's data (same header layout, different magic) is never read */
static void TestProductMagic(void)
{
    uint8_t a[64];
    profile_info_t info;

    FakeNv_Reset();
    Payload(a, sizeof(a), 1U);
    CHECK(ProfileStore_Write(0U, "mine", "v", a, sizeof(a)) == PROFILE_OK);
    FakeNv_Poke(PROFILE_STORE_BASE + 0U, 'H' ^ 'W');                                      /* "HSPF" -> "WSPF": XREX's magic */
    CHECK(ProfileStore_Info(0U, &info) == PROFILE_OK && info.state == PROFILE_SLOT_CORRUPT);
    CHECK(ProfileStore_Read(0U, a, sizeof(a), &(uint16_t){0}, NULL) == PROFILE_ERR_CORRUPT);
}

/* Power cut during a LARGE multi-sector save, at a spread of points (every unit would be 10,000 runs of a
   10 KB write; a stride plus the boundaries covers every phase: erase of each sector, payload, header). */
static void BigSaveCutSweep(void)
{
    static uint8_t oldP[3000], newP[9000];
    uint32_t total;
    uint32_t cut;

    BigPayload(oldP, sizeof(oldP), 11U);
    BigPayload(newP, sizeof(newP), 222U);

    FakeNv_Reset();
    CHECK(ProfileStore_Write(1U, "old", "v", oldP, sizeof(oldP)) == PROFILE_OK);
    FakeNv_CutPowerAfter(0xFFFFFFFFUL);
    CHECK(ProfileStore_Write(1U, "new", "v", newP, sizeof(newP)) == PROFILE_OK);
    total = FakeNv_UnitsUsed();
    CHECK(total > sizeof(newP));

    for (cut = 0U; cut <= total; cut += (cut < 8U || cut + 8U > total) ? 1U : 41U)
    {
        int isOld, isNew;
        profile_status_t rc;

        FakeNv_Reset();
        (void)ProfileStore_Write(1U, "old", "v", oldP, sizeof(oldP));
        FakeNv_CutPowerAfter(cut);
        rc = ProfileStore_Write(1U, "new", "v", newP, sizeof(newP));
        FakeNv_RestorePower();

        isOld = ReadIs(1U, oldP, sizeof(oldP));
        isNew = ReadIs(1U, newP, sizeof(newP));
        CHECK(isOld != isNew);                 /* exactly one of them, never neither, never a mixture */
        if (rc == PROFILE_OK)
        {
            CHECK(isNew);
        }
    }
}

int main(void)
{
    TestStreaming();
    TestProductMagic();
    BigSaveCutSweep();
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
