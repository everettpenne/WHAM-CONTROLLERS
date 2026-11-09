/*
 * test_fault_mask.c -- src/app/protection/fault_mask.c on the host: which individual WATER/TMP/ENERPRO
 * faults a raw GateDriverStatus read represents (polarity, per-channel gating), and the names STATE?
 * lists for a mask.
 */
#include "fault_mask.h"
#include "test_util.h"
#include <string.h>

#define LOW   FAULT_POLARITY_NORMALLY_LOW
#define HIGH  FAULT_POLARITY_NORMALLY_HIGH

static void TestFromGateDriver(void)
{
    /* All pins HIGH = healthy for a NORMALLY_HIGH board; XR1 water (PE0) pulled LOW. */
    CHECK(FaultMask_FromGateDriver(0x0FFF, 0x0F, HIGH, HIGH, HIGH) == 0UL);
    CHECK(FaultMask_FromGateDriver(0x0FFE, 0x0F, HIGH, HIGH, HIGH) == FM_WATER(0));

    /* The bench case that started this: PE0 low, but XR1 not enabled -> not a fault. */
    CHECK(FaultMask_FromGateDriver(0x0FFE, 0x00, HIGH, HIGH, HIGH) == 0UL);
    CHECK(FaultMask_FromGateDriver(0x0FFE, 0x0E, HIGH, HIGH, HIGH) == 0UL);

    /* Several at once, each category in its own group: XR3 water (PE2), XR2 temp (PE5), XR4 enerpro (PE11). */
    {
        const uint16_t raw = (uint16_t)(0x0FFF & ~((1U << 2) | (1U << 5) | (1U << 11)));
        const uint32_t want = FM_WATER(2) | FM_TMP(1) | FM_ENERPRO(3);
        CHECK(FaultMask_FromGateDriver(raw, 0x0F, HIGH, HIGH, HIGH) == want);
        CHECK((want & FM_WATER_TMP_MASK) == (FM_WATER(2) | FM_TMP(1)));
        CHECK((want & FM_ENERPRO_MASK) == FM_ENERPRO(3));
    }

    /* Polarity is per category: water NORMALLY_LOW (HIGH is the fault), the others NORMALLY_HIGH. */
    CHECK(FaultMask_FromGateDriver(0x0FFF, 0x01, LOW, HIGH, HIGH) == FM_WATER(0));
    CHECK(FaultMask_FromGateDriver(0x0FF0, 0x01, LOW, HIGH, HIGH) == 0UL);
    /* ... and the other two categories keep their own. */
    CHECK(FaultMask_FromGateDriver(0x0000, 0x01, HIGH, LOW, LOW) == FM_WATER(0));
    CHECK(FaultMask_FromGateDriver(0x0FFF, 0x01, HIGH, LOW, LOW) == (FM_TMP(0) | FM_ENERPRO(0)));
}

static void TestBitLayout(void)
{
    /* The groups must not overlap, and every bit fits the 23 used. */
    uint32_t seen = 0UL;
    for (unsigned ch = 0; ch < FM_CHANNELS; ch++)
    {
        const uint32_t bits[] = { FM_WATER(ch), FM_TMP(ch), FM_ENERPRO(ch), FM_OCP(ch), FM_ENABLE_OUT(ch) };
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
        {
            CHECK((seen & bits[i]) == 0UL);
            seen |= bits[i];
        }
    }
    CHECK((seen & (FM_EXT_ENABLE | FM_PC10 | FM_TEST)) == 0UL);
    CHECK((FM_EXT_ENABLE & FM_PC10) == 0UL);
    CHECK((FM_PC10 & FM_TEST) == 0UL);
    CHECK(FM_WATER_TMP_MASK == (FM_WATER(0) | FM_WATER(1) | FM_WATER(2) | FM_WATER(3) |
                                FM_TMP(0) | FM_TMP(1) | FM_TMP(2) | FM_TMP(3)));
    CHECK(FM_ENERPRO_MASK == (FM_ENERPRO(0) | FM_ENERPRO(1) | FM_ENERPRO(2) | FM_ENERPRO(3)));
}

static void TestFormat(void)
{
    char buf[FAULT_MASK_TEXT_MAX];

    CHECK(FaultMask_Format(0UL, buf, sizeof(buf)) == 4U);
    CHECK_STR(buf, "NONE");

    FaultMask_Format(FM_WATER(0), buf, sizeof(buf));
    CHECK_STR(buf, "WATER1");

    /* Order: category groups first, channel ascending within; system bits last. */
    FaultMask_Format(FM_PC10 | FM_OCP(2) | FM_TMP(1) | FM_WATER(3) | FM_WATER(0) | FM_EXT_ENABLE | FM_ENABLE_OUT(0) |
                         FM_ENERPRO(1) | FM_TEST, buf, sizeof(buf));
    CHECK_STR(buf, "WATER1,WATER4,TMP2,ENERPRO2,OCP3,ENABLE_OUTPUT1,EXTERNAL_ENABLE,PC10,TEST");

    /* The return value is the length written. */
    {
        const size_t n = FaultMask_Format(FM_TMP(0) | FM_TMP(3), buf, sizeof(buf));
        CHECK(n == strlen(buf));
        CHECK_STR(buf, "TMP1,TMP4");
    }

    /* Every bit set must fit the documented maximum, whole. */
    {
        uint32_t all = FM_EXT_ENABLE | FM_PC10 | FM_TEST;
        for (unsigned ch = 0; ch < FM_CHANNELS; ch++)
        {
            all |= FM_WATER(ch) | FM_TMP(ch) | FM_ENERPRO(ch) | FM_OCP(ch) | FM_ENABLE_OUT(ch);
        }
        const size_t n = FaultMask_Format(all, buf, sizeof(buf));
        CHECK(n == strlen(buf));
        CHECK(n < FAULT_MASK_TEXT_MAX);
        CHECK(strstr(buf, "TEST") != NULL);
        CHECK(strstr(buf, "ENABLE_OUTPUT4") != NULL);
    }
}

static void TestFormatTruncation(void)
{
    char buf[16];

    /* "WATER1,WATER2" is 13 characters; a third name would not fit in 16 -> cut at a whole name. */
    const size_t n = FaultMask_Format(FM_WATER(0) | FM_WATER(1) | FM_WATER(2), buf, sizeof(buf));
    CHECK(n == strlen(buf));
    CHECK_STR(buf, "WATER1,WATER2");

    /* Zero-size buffer: nothing written, nothing crashes. */
    CHECK(FaultMask_Format(FM_WATER(0), buf, 0U) == 0U);

    /* A buffer too small for even one name leaves an empty string, not garbage. */
    {
        char tiny[4];
        memset(tiny, 'x', sizeof(tiny));
        CHECK(FaultMask_Format(FM_WATER(0), tiny, sizeof(tiny)) == 0U);
        CHECK_STR(tiny, "");
    }
}

int main(void)
{
    TestFromGateDriver();
    TestBitLayout();
    TestFormat();
    TestFormatTruncation();
    return test_report("test_fault_mask");
}
