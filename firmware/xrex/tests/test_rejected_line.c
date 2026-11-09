/*
 * test_rejected_line.c -- src/app/commands/rejected_line.c on the host: what a refused line looks like in
 * DIAGnostic:UART:REJect?.
 */
#include "rejected_line.h"
#include "test_util.h"
#include <string.h>

static void Rec(const char *s) { RejectedLine_Record((const uint8_t *)s, strlen(s)); }

int main(void)
{
    char buf[REJECTED_LINE_TEXT_MAX];

    /* Nothing refused yet. */
    CHECK(RejectedLine_Count() == 0U);
    RejectedLine_Format(buf, sizeof(buf));
    CHECK_STR(buf, "COUNT=0 LEN=0 HEX= TEXT=");

    Rec("SOURce:STATus? 3");
    CHECK(RejectedLine_Count() == 1U);
    CHECK(RejectedLine_Format(buf, sizeof(buf)) == strlen(buf));
    CHECK_STR(buf, "COUNT=1 LEN=16 HEX=534F555263653A5354415475733F2033 TEXT=SOURce:STATus? 3");

    /* A garbled line: a changed byte and a dropped one are visible in both forms. */
    Rec("SOURce:STAUs? 3");
    RejectedLine_Format(buf, sizeof(buf));
    CHECK(strstr(buf, "COUNT=2 LEN=15 ") == buf);
    CHECK(strstr(buf, "TEXT=SOURce:STAUs? 3") != NULL);

    /* Non-printable bytes, a backslash and an embedded NUL (the true length survives, text is escaped). */
    {
        const uint8_t raw[] = { 'A', 0x00, 0x7F, '\\', 0xFF, 'z' };
        RejectedLine_Record(raw, sizeof(raw));
        RejectedLine_Format(buf, sizeof(buf));
        CHECK(strstr(buf, "COUNT=3 LEN=6 HEX=41007F5CFF7A ") == buf);
        CHECK_STR(strstr(buf, "TEXT="), "TEXT=A\\x00\\x7F\\\\\\xFFz");
    }

    /* A line longer than what is kept: LEN is the true length, HEX holds only the kept bytes. */
    {
        uint8_t big[100];
        memset(big, 'x', sizeof(big));
        RejectedLine_Record(big, sizeof(big));
        RejectedLine_Format(buf, sizeof(buf));
        CHECK(strstr(buf, "COUNT=4 LEN=100 HEX=") == buf);
        CHECK(strlen(buf) < REJECTED_LINE_TEXT_MAX);
        CHECK(strlen(strstr(buf, "HEX=") + 4) > 2 * REJECTED_LINE_KEEP);   /* hex then " TEXT=" and the text */
    }

    /* Every byte non-printable: the worst case for the text form still fits the documented maximum. */
    {
        uint8_t all[REJECTED_LINE_KEEP];
        memset(all, 0x01, sizeof(all));
        RejectedLine_Record(all, sizeof(all));
        const size_t n = RejectedLine_Format(buf, sizeof(buf));
        CHECK(n == strlen(buf));
        CHECK(n < REJECTED_LINE_TEXT_MAX);
        CHECK(strstr(buf, "\\x01\\x01") != NULL);
    }

    /* A tiny buffer is cut, never overrun, and stays terminated. */
    {
        char small[24];
        memset(small, 'Q', sizeof(small));
        RejectedLine_Format(small, sizeof(small));
        CHECK(memchr(small, '\0', sizeof(small)) != NULL);
        CHECK(RejectedLine_Format(small, 0U) == 0U);
    }
    return test_report("test_rejected_line");
}
