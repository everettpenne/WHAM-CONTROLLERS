/*
 * test_phase_mask.c -- the phase list parser and formatter (src/app/control/phase_mask.c).
 */
#include "phase_mask.h"
#include "test_util.h"
#include <string.h>

static uint8_t P(const char *text, uint8_t phases, uint8_t none, phase_parse_t *rc)
{
    uint8_t m = 0xAAU;

    *rc = PhaseMask_Parse(text, phases, none, &m);
    return m;
}

static void TestParse(void)
{
    phase_parse_t rc;

    /* letters, any order and case, repeats harmless */
    CHECK(P("U", 3U, 0U, &rc) == 0x1U && rc == PHASE_PARSE_OK);
    CHECK(P("v", 3U, 0U, &rc) == 0x2U && rc == PHASE_PARSE_OK);
    CHECK(P("W", 3U, 0U, &rc) == 0x4U && rc == PHASE_PARSE_OK);
    CHECK(P("UW", 3U, 0U, &rc) == 0x5U && rc == PHASE_PARSE_OK);
    CHECK(P("wu", 3U, 0U, &rc) == 0x5U && rc == PHASE_PARSE_OK);
    CHECK(P("uvw", 3U, 0U, &rc) == 0x7U && rc == PHASE_PARSE_OK);
    CHECK(P("U,W", 3U, 0U, &rc) == 0x5U && rc == PHASE_PARSE_OK);
    CHECK(P("UUV", 3U, 0U, &rc) == 0x3U && rc == PHASE_PARSE_OK);

    /* channel numbers, 1-based like XREX's */
    CHECK(P("1", 3U, 0U, &rc) == 0x1U && rc == PHASE_PARSE_OK);
    CHECK(P("13", 3U, 0U, &rc) == 0x5U && rc == PHASE_PARSE_OK);
    CHECK(P("1,2,3", 3U, 0U, &rc) == 0x7U && rc == PHASE_PARSE_OK);

    /* the words */
    CHECK(P("ALL", 3U, 0U, &rc) == 0x7U && rc == PHASE_PARSE_OK);
    CHECK(P("all", 3U, 0U, &rc) == 0x7U && rc == PHASE_PARSE_OK);
    CHECK(P("NONE", 3U, 1U, &rc) == 0x0U && rc == PHASE_PARSE_OK);
    (void)P("NONE", 3U, 0U, &rc);
    CHECK(rc == PHASE_PARSE_BAD);                                   /* NONE only where an empty set makes sense */

    /* refused: not a phase of this build, mixed naming, junk, nothing */
    (void)P("X", 3U, 0U, &rc);  CHECK(rc == PHASE_PARSE_BAD);       /* a 3-phase build has no X */
    (void)P("4", 3U, 0U, &rc);  CHECK(rc == PHASE_PARSE_BAD);
    (void)P("0", 3U, 0U, &rc);  CHECK(rc == PHASE_PARSE_BAD);
    (void)P("U2", 3U, 0U, &rc); CHECK(rc == PHASE_PARSE_BAD);
    (void)P("UQ", 3U, 0U, &rc); CHECK(rc == PHASE_PARSE_BAD);
    (void)P("U W", 3U, 0U, &rc); CHECK(rc == PHASE_PARSE_BAD);      /* a space would have split the argument already */
    (void)P("ALLU", 3U, 0U, &rc); CHECK(rc == PHASE_PARSE_BAD);
    (void)P("", 3U, 0U, &rc);   CHECK(rc == PHASE_PARSE_EMPTY);
    (void)P(",", 3U, 0U, &rc);  CHECK(rc == PHASE_PARSE_EMPTY);
    CHECK(PhaseMask_Parse(NULL, 3U, 0U, &(uint8_t){0}) == PHASE_PARSE_EMPTY);

    /* a bigger build knows more phases */
    CHECK(P("X", 4U, 0U, &rc) == 0x8U && rc == PHASE_PARSE_OK);
    CHECK(P("ALL", 5U, 0U, &rc) == 0x1FU && rc == PHASE_PARSE_OK);
    (void)P("Z", 5U, 0U, &rc);  CHECK(rc == PHASE_PARSE_BAD);
    CHECK(P("1", 1U, 0U, &rc) == 0x1U && rc == PHASE_PARSE_OK);
    (void)P("V", 1U, 0U, &rc);  CHECK(rc == PHASE_PARSE_BAD);
}

static void TestFormat(void)
{
    char out[16];

    PhaseMask_Format(0x7U, 3U, out, sizeof(out));  CHECK_STR(out, "UVW");
    PhaseMask_Format(0x5U, 3U, out, sizeof(out));  CHECK_STR(out, "UW");
    PhaseMask_Format(0x4U, 3U, out, sizeof(out));  CHECK_STR(out, "W");
    PhaseMask_Format(0x2U, 3U, out, sizeof(out));  CHECK_STR(out, "V");
    PhaseMask_Format(0x0U, 3U, out, sizeof(out));  CHECK_STR(out, "NONE");
    PhaseMask_Format(0x8U, 3U, out, sizeof(out));  CHECK_STR(out, "NONE");   /* a bit beyond the build is not shown */
    PhaseMask_Format(0x1FU, 5U, out, sizeof(out)); CHECK_STR(out, "UVWXY");
    PhaseMask_Format(0x7U, 3U, out, 3U);           CHECK_STR(out, "UV");     /* a short buffer is cut, never overrun */
    PhaseMask_Format(0x0U, 3U, out, 3U);           CHECK_STR(out, "NO");
    CHECK(PhaseMask_Letter(0U) == 'U' && PhaseMask_Letter(2U) == 'W' && PhaseMask_Letter(5U) == 'Z' && PhaseMask_Letter(6U) == '?');
    CHECK(PhaseMask_All(3U) == 0x7U && PhaseMask_All(1U) == 0x1U && PhaseMask_All(8U) == 0xFFU);
}

/* format and parse agree for every mask of a 3-phase build */
static void TestRoundTrip(void)
{
    for (uint8_t m = 1U; m < 8U; m++)
    {
        char text[8];
        uint8_t back = 0U;

        PhaseMask_Format(m, 3U, text, sizeof(text));
        CHECK(PhaseMask_Parse(text, 3U, 0U, &back) == PHASE_PARSE_OK && back == m);
    }
}

int main(void)
{
    TestParse();
    TestFormat();
    TestRoundTrip();
    return test_report("test_phase_mask");
}
