/*
 * test_scpi_util.c -- reply building and strict argument parsing (src/middleware/scpi/scpi_util.c).
 * SHARED: byte-identical in WHAM-XREX-PFMG474 and WHAM-HBRIDGE-PFMG474.
 */
#include "scpi_util.h"
#include "fake_uart.h"
#include "test_util.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uart_instance_t uart2;                                  /* fake_uart.c's */
static uart_instance_t *const inst = &uart2;

static void TestReplies(void)
{
    fake_uart_clear(); Scpi_ReplyOk(inst);                    CHECK_STR(fake_uart_out, "OK\r\n");
    fake_uart_clear(); Scpi_SendErr(inst, 12, "bad");         CHECK_STR(fake_uart_out, "ERR 12 bad\r\n");
    fake_uart_clear(); Scpi_ReplyFixed(inst, 1.5f, 3U);       CHECK_STR(fake_uart_out, "OK 1.500\r\n");
    fake_uart_clear(); Scpi_ReplyFixed(inst, -0.25f, 2U);     CHECK_STR(fake_uart_out, "OK -0.25\r\n");
    fake_uart_clear(); Scpi_ReplyFixed(inst, 7.0f, 0U);       CHECK_STR(fake_uart_out, "OK 7\r\n");
    fake_uart_clear(); Scpi_ReplyFloat(inst, 0.25f);          CHECK_STR(fake_uart_out, "OK 0.25\r\n");
    fake_uart_clear(); Scpi_ReplyFloat(inst, 0.0f);           CHECK_STR(fake_uart_out, "OK 0\r\n");
    fake_uart_clear(); Scpi_ReplyFloat3(inst, 4000.0f, 200000.0f, 0.001f); CHECK_STR(fake_uart_out, "OK 4000 200000 0.001\r\n");
    fake_uart_clear(); Scpi_ReplyFloat3(inst, -1.5f, 1e-5f, 1e7f);          CHECK_STR(fake_uart_out, "OK -1.5 1e-05 1e+07\r\n");

    fake_uart_clear(); Scpi_ReplySet(inst, SCPI_SET_OK, "range", "busy");    CHECK_STR(fake_uart_out, "OK\r\n");
    fake_uart_clear(); Scpi_ReplySet(inst, SCPI_SET_RANGE, "range", "busy"); CHECK_STR(fake_uart_out, "ERR 12 range\r\n");
    fake_uart_clear(); Scpi_ReplySet(inst, SCPI_SET_BUSY, "range", "busy");  CHECK_STR(fake_uart_out, "ERR 13 busy\r\n");

    fake_uart_clear(); Scpi_ReplyBadNumber(inst, "12x");      CHECK_STR(fake_uart_out, "ERR 12 not a number: 12x\r\n");
    fake_uart_clear(); Scpi_ReplyBadNumberErr(inst, ERR_PFMIN_BAD_CHANNEL, "q");
    CHECK_STR(fake_uart_out, "ERR 8 not a number: q\r\n");
    fake_uart_clear(); Scpi_ReplyBadNumber(inst, "a\x01\x7f" "b");                       /* nothing unprintable goes back out */
    CHECK_STR(fake_uart_out, "ERR 12 not a number: a??b\r\n");
    fake_uart_clear(); Scpi_ReplyBadNumber(inst, "123456789012345678901234567890123456");  /* a long token is cut */
    CHECK(strlen(fake_uart_out) < 80U && strstr(fake_uart_out, "not a number: 1234567890123456789012345") != NULL);
    fake_uart_clear(); Scpi_ReplyBadNumber(inst, NULL);       CHECK_STR(fake_uart_out, "ERR 12 not a number: \r\n");
}

static void TestFormatFixed(void)
{
    char out[24];

    Scpi_FormatFixed(out, sizeof(out), 1.25f, 3U);   CHECK_STR(out, "1.250");
    Scpi_FormatFixed(out, sizeof(out), -1.25f, 3U);  CHECK_STR(out, "-1.250");
    Scpi_FormatFixed(out, sizeof(out), 0.0f, 2U);    CHECK_STR(out, "0.00");
    Scpi_FormatFixed(out, sizeof(out), 0.005f, 2U);  CHECK_STR(out, "0.01");      /* rounds half away from zero */
    Scpi_FormatFixed(out, sizeof(out), -0.004f, 2U); CHECK_STR(out, "0.00");
    Scpi_FormatFixed(out, sizeof(out), 12345.0f, 0U); CHECK_STR(out, "12345");
    Scpi_FormatFixed(out, sizeof(out), 620.69f, 4U); CHECK_STR(out, "620.6900");
    Scpi_FormatFixed(out, sizeof(out), 1.0f, 3U);    CHECK_STR(out, "1.000");
}

static void TestParseLong(void)
{
    long v = 99;

    CHECK(Scpi_ParseLong("0", &v) == 1U && v == 0);
    CHECK(Scpi_ParseLong("12", &v) == 1U && v == 12);
    CHECK(Scpi_ParseLong("-7", &v) == 1U && v == -7);
    CHECK(Scpi_ParseLong("+3", &v) == 1U && v == 3);
    CHECK(Scpi_ParseLong("007", &v) == 1U && v == 7);
    {
        char buf[32];

        snprintf(buf, sizeof(buf), "%ld", LONG_MAX);  CHECK(Scpi_ParseLong(buf, &v) == 1U && v == LONG_MAX);
        snprintf(buf, sizeof(buf), "%ld", LONG_MIN);  CHECK(Scpi_ParseLong(buf, &v) == 1U && v == LONG_MIN);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)LONG_MAX + 1UL);  v = 5; CHECK(Scpi_ParseLong(buf, &v) == 0U && v == 5);
        snprintf(buf, sizeof(buf), "-%lu", (unsigned long)LONG_MAX + 2UL); v = 5; CHECK(Scpi_ParseLong(buf, &v) == 0U && v == 5);
    }
    v = 5;
    CHECK(Scpi_ParseLong("", &v) == 0U);
    CHECK(Scpi_ParseLong("+", &v) == 0U);
    CHECK(Scpi_ParseLong("-", &v) == 0U);
    CHECK(Scpi_ParseLong("12x", &v) == 0U);
    CHECK(Scpi_ParseLong("x12", &v) == 0U);
    CHECK(Scpi_ParseLong("1.5", &v) == 0U);
    CHECK(Scpi_ParseLong("1e3", &v) == 0U);
    CHECK(Scpi_ParseLong("0x10", &v) == 0U);
    CHECK(Scpi_ParseLong(" 1", &v) == 0U);
    CHECK(Scpi_ParseLong("1 ", &v) == 0U);
    CHECK(Scpi_ParseLong("--1", &v) == 0U);
    CHECK(Scpi_ParseLong(NULL, &v) == 0U);
    CHECK(v == 5);                                               /* a refusal leaves the value alone */
}

static void TestParseFloat(void)
{
    float v = 99.0f;

    CHECK(Scpi_ParseFloat("0", &v) == 1U && v == 0.0f);
    CHECK(Scpi_ParseFloat("12", &v) == 1U && v == 12.0f);
    CHECK(Scpi_ParseFloat("1.5", &v) == 1U && v == 1.5f);
    CHECK(Scpi_ParseFloat("-0.25", &v) == 1U && v == -0.25f);
    CHECK(Scpi_ParseFloat("+2", &v) == 1U && v == 2.0f);
    CHECK(Scpi_ParseFloat(".5", &v) == 1U && v == 0.5f);
    CHECK(Scpi_ParseFloat("5.", &v) == 1U && v == 5.0f);
    CHECK(Scpi_ParseFloat("1e3", &v) == 1U && v == 1000.0f);
    CHECK(Scpi_ParseFloat("2.5E-2", &v) == 1U && v == 0.025f);
    CHECK(Scpi_ParseFloat("1e+2", &v) == 1U && v == 100.0f);
    CHECK(Scpi_ParseFloat("0.001", &v) == 1U && v == 0.001f);
    CHECK(Scpi_ParseFloat("620.69", &v) == 1U && v == 620.69f);
    CHECK(Scpi_ParseFloat("0.167", &v) == 1U && v == 0.167f);
    CHECK(Scpi_ParseFloat("200000", &v) == 1U && v == 200000.0f);

    /* the same value atof gives, for ordinary text */
    {
        static const char *const ordinary[] = { "0.1", "3.14159", "1234.5678", "0.000123", "98765.4321", "7.77e5", "1e-5", "2.9", "47.5", "0.3" };

        for (size_t i = 0U; i < sizeof(ordinary) / sizeof(ordinary[0]); i++)
        {
            CHECK(Scpi_ParseFloat(ordinary[i], &v) == 1U);
            CHECK(v == (float)atof(ordinary[i]));
        }
    }
    v = 5.0f;
    CHECK(Scpi_ParseFloat("", &v) == 0U);
    CHECK(Scpi_ParseFloat(".", &v) == 0U);
    CHECK(Scpi_ParseFloat("-", &v) == 0U);
    CHECK(Scpi_ParseFloat("e3", &v) == 0U);
    CHECK(Scpi_ParseFloat("1e", &v) == 0U);
    CHECK(Scpi_ParseFloat("1e+", &v) == 0U);
    CHECK(Scpi_ParseFloat("1.2.3", &v) == 0U);
    CHECK(Scpi_ParseFloat("1,5", &v) == 0U);
    CHECK(Scpi_ParseFloat("12x", &v) == 0U);
    CHECK(Scpi_ParseFloat("nan", &v) == 0U);
    CHECK(Scpi_ParseFloat("inf", &v) == 0U);
    CHECK(Scpi_ParseFloat("-inf", &v) == 0U);
    CHECK(Scpi_ParseFloat("0x10", &v) == 0U);
    CHECK(Scpi_ParseFloat(" 1", &v) == 0U);
    CHECK(Scpi_ParseFloat("1 ", &v) == 0U);
    CHECK(Scpi_ParseFloat("1e400", &v) == 0U);                   /* beyond a float */
    CHECK(Scpi_ParseFloat("1e39", &v) == 0U);
    CHECK(Scpi_ParseFloat("12345678901234567890123", &v) == 0U); /* more digits than are held exactly */
    CHECK(Scpi_ParseFloat(NULL, &v) == 0U);
    CHECK(v == 5.0f);
}

static void TestParseBoolAndScaled(void)
{
    uint8_t b = 9U;
    uint32_t u = 77U;
    float f = 0.0f;

    CHECK(Scpi_ParseBool("0", &b) == 1U && b == 0U);
    CHECK(Scpi_ParseBool("1", &b) == 1U && b == 1U);
    b = 9U;
    CHECK(Scpi_ParseBool("2", &b) == 0U && Scpi_ParseBool("01", &b) == 0U && Scpi_ParseBool("", &b) == 0U);
    CHECK(Scpi_ParseBool("-1", &b) == 0U && Scpi_ParseBool("1 ", &b) == 0U && Scpi_ParseBool(NULL, &b) == 0U && b == 9U);

    CHECK(Scpi_ParseScaled("47.5", 2U, &u) == 1U && u == 4750U);
    CHECK(Scpi_ParseScaled("47", 2U, &u) == 1U && u == 4700U);
    CHECK(Scpi_ParseScaled("0.001", 3U, &u) == 1U && u == 1U);
    CHECK(Scpi_ParseScaled("12", 0U, &u) == 1U && u == 12U);
    CHECK(Scpi_ParseScaled("4294967295", 0U, &u) == 1U && u == 4294967295UL);
    u = 77U;
    CHECK(Scpi_ParseScaled("4294967296", 0U, &u) == 0U);              /* too big */
    CHECK(Scpi_ParseScaled("42949672.96", 2U, &u) == 0U);
    CHECK(Scpi_ParseScaled("1.234", 2U, &u) == 0U);                   /* more decimals than the unit */
    CHECK(Scpi_ParseScaled("1.5", 0U, &u) == 0U);
    CHECK(Scpi_ParseScaled("-1", 0U, &u) == 0U);
    CHECK(Scpi_ParseScaled("", 2U, &u) == 0U && Scpi_ParseScaled(".", 2U, &u) == 0U && Scpi_ParseScaled("1.2.3", 3U, &u) == 0U);
    CHECK(Scpi_ParseScaled("12x", 2U, &u) == 0U && Scpi_ParseScaled(NULL, 2U, &u) == 0U && u == 77U);

    CHECK(Scpi_ParseUnsigned("47.5", 2U, &f) == 1U && f == 47.5f);
    CHECK(Scpi_ParseUnsigned("0.167", 3U, &f) == 1U && f == 0.167f);
    CHECK(Scpi_ParseUnsigned("-1", 3U, &f) == 0U && Scpi_ParseUnsigned("abc", 3U, &f) == 0U);
}


/* the checks the shot profile's parser had before it moved here */
static void TestParseScaledLegacy(void)
{
    uint32_t v = 0U;

    CHECK(Scpi_ParseScaled("20000", 3U, &v) == 1U && v == 20000000U);
    CHECK(Scpi_ParseScaled("20000.5", 3U, &v) == 1U && v == 20000500U);
    CHECK(Scpi_ParseScaled("0.125", 3U, &v) == 1U && v == 125U);
    CHECK(Scpi_ParseScaled("50", 2U, &v) == 1U && v == 5000U);
    CHECK(Scpi_ParseScaled("100.", 2U, &v) == 1U && v == 10000U);
    CHECK(Scpi_ParseScaled("1500", 0U, &v) == 1U && v == 1500U);
    CHECK(Scpi_ParseScaled(".5", 2U, &v) == 1U && v == 50U);
    CHECK(Scpi_ParseScaled("47.555", 2U, &v) == 0U);
    CHECK(Scpi_ParseScaled("1e3", 2U, &v) == 0U);
    CHECK(Scpi_ParseScaled("5 ", 2U, &v) == 0U);
    CHECK(Scpi_ParseScaled("99999999999", 3U, &v) == 0U);
}

/* the handler shorthands */
static long g_seenL;
static float g_seenF;

static void HandlerLong(uart_instance_t *i, char *args)
{
    long v = -1;

    SCPI_NEED_LONG(i, args, v);
    g_seenL = v;
    Scpi_ReplyOk(i);
}

static void HandlerLongCode(uart_instance_t *i, char *args)
{
    int v = -1;                                   /* a narrower variable: the macro casts to its type */

    SCPI_NEED_LONG_ERR(i, args, v, ERR_PFMIN_BAD_COUNT);
    g_seenL = v;
    Scpi_ReplyOk(i);
}

static void HandlerFloat(uart_instance_t *i, char *args)
{
    float v = -1.0f;

    SCPI_NEED_FLOAT(i, args, v);
    g_seenF = v;
    Scpi_ReplyOk(i);
}

static void HandlerDouble(uart_instance_t *i, char *args)
{
    double v = -1.0;

    SCPI_NEED_FLOAT(i, args, v);
    g_seenF = (float)v;
    Scpi_ReplyOk(i);
}

static void TestShorthands(void)
{
    char a[16];

    strcpy(a, "42");  fake_uart_clear(); HandlerLong(inst, a);     CHECK_STR(fake_uart_out, "OK\r\n"); CHECK(g_seenL == 42);
    strcpy(a, "4x");  fake_uart_clear(); g_seenL = 0; HandlerLong(inst, a);
    CHECK_STR(fake_uart_out, "ERR 12 not a number: 4x\r\n"); CHECK(g_seenL == 0);    /* returned before using it */
    strcpy(a, "7");   fake_uart_clear(); HandlerLongCode(inst, a); CHECK_STR(fake_uart_out, "OK\r\n"); CHECK(g_seenL == 7);
    strcpy(a, "q");   fake_uart_clear(); HandlerLongCode(inst, a); CHECK_STR(fake_uart_out, "ERR 9 not a number: q\r\n");
    strcpy(a, "-2.5"); fake_uart_clear(); HandlerFloat(inst, a);   CHECK_STR(fake_uart_out, "OK\r\n"); CHECK(g_seenF == -2.5f);
    strcpy(a, "x");   fake_uart_clear(); HandlerFloat(inst, a);    CHECK_STR(fake_uart_out, "ERR 12 not a number: x\r\n");
    strcpy(a, "0.5"); fake_uart_clear(); HandlerDouble(inst, a);   CHECK(g_seenF == 0.5f);
}

static void TestGetters(void)
{
    long l = 5L;
    float f = 5.0f;

    fake_uart_clear(); CHECK(Scpi_GetLong(inst, ERR_INVALID_ARGS, "42", &l) == 1U && l == 42L);   CHECK_STR(fake_uart_out, "");
    fake_uart_clear(); CHECK(Scpi_GetLong(inst, ERR_PFMIN_BAD_CHANNEL, "x", &l) == 0U && l == 42L);
    CHECK_STR(fake_uart_out, "ERR 8 not a number: x\r\n");
    fake_uart_clear(); CHECK(Scpi_GetFloat(inst, ERR_INVALID_ARGS, "0.5", &f) == 1U && f == 0.5f);  CHECK_STR(fake_uart_out, "");
    fake_uart_clear(); CHECK(Scpi_GetFloat(inst, ERR_INVALID_ARGS, "z", &f) == 0U && f == 0.5f);
    CHECK_STR(fake_uart_out, "ERR 12 not a number: z\r\n");
}

/* a generated setting pair */
static float g_setting = 1.0f;
static int   g_busy = 0;

static int SetFoo(float v)
{
    if (g_busy != 0) { return SCPI_SET_BUSY; }
    if (v > 10.0f) { return SCPI_SET_RANGE; }
    g_setting = v;
    return SCPI_SET_OK;
}
static float GetFoo(void) { return g_setting; }

SCPI_FLOAT_SETTING(cmd_foo, SetFoo, GetFoo, 3U, "FOO <volts>", "foo must be 0-10", "not while running")

static void TestFloatSetting(void)
{
    char a[32];

    strcpy(a, "2.5");  fake_uart_clear(); cmd_foo(inst, a);          CHECK_STR(fake_uart_out, "OK\r\n");
    fake_uart_clear(); cmd_foo_query(inst, NULL);                    CHECK_STR(fake_uart_out, "OK 2.500\r\n");
    strcpy(a, "11");   fake_uart_clear(); cmd_foo(inst, a);          CHECK_STR(fake_uart_out, "ERR 12 foo must be 0-10\r\n");
    g_busy = 1;
    strcpy(a, "1");    fake_uart_clear(); cmd_foo(inst, a);          CHECK_STR(fake_uart_out, "ERR 13 not while running\r\n");
    g_busy = 0;
    strcpy(a, "abc");  fake_uart_clear(); cmd_foo(inst, a);          CHECK_STR(fake_uart_out, "ERR 12 FOO <volts>\r\n");
    strcpy(a, "-1");   fake_uart_clear(); cmd_foo(inst, a);          CHECK_STR(fake_uart_out, "ERR 12 FOO <volts>\r\n");   /* unsigned only */
    strcpy(a, "1 2");  fake_uart_clear(); cmd_foo(inst, a);          CHECK_STR(fake_uart_out, "ERR 12 FOO <volts>\r\n");   /* one argument */
    fake_uart_clear(); cmd_foo(inst, NULL);                          CHECK_STR(fake_uart_out, "ERR 12 FOO <volts>\r\n");
    strcpy(a, "1.2345"); fake_uart_clear(); cmd_foo(inst, a);        CHECK_STR(fake_uart_out, "ERR 12 FOO <volts>\r\n");   /* too many decimals */
    CHECK(g_setting == 2.5f);                                        /* none of the refusals changed it */
}

/* the preview text of a float matches printf's %g (which the target's newlib-nano cannot print) */
static void TestFormatFloat(void)
{
    static const float typical[] = { 0.167f, 6.667f, 4000.0f, 200000.0f, 0.001f, 12.5f, 620.69f, 1.45f, 250.0f, 47.5f,
        90000.0f, 19250.0f, 1.5f, 900.0f, 1e-5f, 1e7f, 123456.7f, 0.000123f, 0.0f, -1.5f, -0.001f, 1000000.0f, 999999.0f,
        999999.5f, 0.0001f, 0.00009f, 100.0f, 2.9f, 1e10f, 1.5e-8f, 3.14159265f, 10.0f, 0.5f, 123456789.0f, 5e-5f, 62069.0f };
    char mine[40], ref[40];

    for (size_t i = 0U; i < sizeof(typical) / sizeof(typical[0]); i++)
    {
        Scpi_FormatFloat(typical[i], mine, sizeof(mine));
        snprintf(ref, sizeof(ref), "%g", (double)typical[i]);
        CHECK_STR(mine, ref);
    }
    Scpi_FormatFloat(NAN, mine, sizeof(mine));
    CHECK_STR(mine, "nan");
    Scpi_FormatFloat(INFINITY, mine, sizeof(mine));
    CHECK_STR(mine, "inf");
    Scpi_FormatFloat(-INFINITY, mine, sizeof(mine));
    CHECK_STR(mine, "-inf");
    Scpi_FormatFloat(1.0f, mine, 2U);                         /* a short buffer is cut, never overrun */
    CHECK(mine[0] == '1' && mine[1] == '\0');
    Scpi_FormatFloat(123.456f, mine, 3U);
    CHECK_STR(mine, "12");

    /* arbitrary values: 6 significant digits means up to 5e-6 relative error, no more */
    srand(1);
    for (int i = 0; i < 20000; i++)
    {
        float v = (float)(((double)rand() / RAND_MAX) * pow(10.0, (rand() % 24) - 10));
        double back;

        if ((rand() & 1) != 0) { v = -v; }
        Scpi_FormatFloat(v, mine, sizeof(mine));
        back = strtod(mine, NULL);
        CHECK(fabs(back - (double)v) <= fabs((double)v) * 6.0e-6);
    }
}

int main(void)
{
    TestReplies();
    TestFormatFixed();
    TestParseLong();
    TestParseFloat();
    TestParseBoolAndScaled();
    TestParseScaledLegacy();
    TestShorthands();
    TestGetters();
    TestFloatSetting();
    TestFormatFloat();
    return test_report("test_scpi_util");
}
