/*
 * scpi_util.h / scpi_util.c -- what every command handler needs and nothing product-specific: building the
 * "OK ..." / "ERR <code> <message>" reply lines, and STRICT parsing of numeric arguments.
 *
 * SHARED, byte-identical in WHAM-XREX-PFMG474 and WHAM-HBRIDGE-PFMG474 (python/check_shared.py);
 * host-tested (tests/test_scpi_util.c). The conventions these implement are in docs/scpi_conventions.md.
 *
 * Strict means the WHOLE argument token must be the number: "12" is 12, "12x", "", "1.5" for an integer
 * argument, "nan" or "0x10" are refused (the handler answers ERR 12), where atol()/atof() quietly turn them
 * into 0 or a prefix. Nothing here uses printf's float support (newlib-nano needs a link flag for it): decimals
 * are formatted with integer arithmetic.
 */
#ifndef SCPI_UTIL_H
#define SCPI_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "uart.h"
#include "scpi_err.h"

/* ---- replies ---- */

/* "ERR <code> <msg>\r\n". */
void Scpi_SendErr(uart_instance_t *inst, int code, const char *msg);

/* "OK\r\n". */
void Scpi_ReplyOk(uart_instance_t *inst);

/* "OK <v>\r\n" with a fixed number of decimals ("OK -1.250"). */
void Scpi_ReplyFixed(uart_instance_t *inst, float v, uint8_t decimals);

/* "OK <a>\r\n" and "OK <a> <b> <c>\r\n", floats as Scpi_FormatFloat (printf's "%g") -- the compact form XREX's
   float replies have always used. */
void Scpi_ReplyFloat(uart_instance_t *inst, float a);
void Scpi_ReplyFloat3(uart_instance_t *inst, float a, float b, float c);

/* "OK <count> <v0> ... <v(count-1)>\r\n", one line of any length, sent in pieces from a small buffer. */
void Scpi_ReplyU32List(uart_instance_t *inst, const uint32_t *values, uint16_t count);

/* The outcome of a setter, as plain integers so any product's own result enum maps onto it. */
#define SCPI_SET_OK     (0)
#define SCPI_SET_RANGE  (1)     /* value outside the allowed range */
#define SCPI_SET_BUSY   (2)     /* not allowed right now (a run in progress) */

/* OK for SCPI_SET_OK; ERR 13 busyMsg for SCPI_SET_BUSY; ERR 12 rangeMsg otherwise. */
void Scpi_ReplySet(uart_instance_t *inst, int result, const char *rangeMsg, const char *busyMsg);

/* ERR 12 "not a number: <token>" (the token is cut short if long). */
void Scpi_ReplyBadNumber(uart_instance_t *inst, const char *tok);

/* ---- number to text ---- */

/* v with `decimals` decimals, rounded: "-1.250" (no printf float support needed). */
void Scpi_FormatFixed(char *buf, size_t n, float v, uint8_t decimals);

/* A float as text like printf's "%g": 6 significant digits from a single scaling of the value (so the sixth
   digit can differ from printf's in about 2 % of arbitrary values -- a float has only ~7), trailing zeros
   dropped, fixed notation for exponents -4..5 and "d.ddde+XX" outside. */
void Scpi_FormatFloat(float v, char *out, size_t n);

/* ---- strict text to number; each returns 1 on success, 0 (out untouched) otherwise ---- */

uint8_t Scpi_ParseLong(const char *tok, long *out);               /* [+-]digits, fits a long */
uint8_t Scpi_ParseFloat(const char *tok, float *out);             /* [+-]digits[.digits][e[+-]digits], finite */
uint8_t Scpi_ParseBool(const char *tok, uint8_t *out);            /* "0" or "1" */

/* Unsigned decimal with at most `decimals` places as an integer of units of 10^-decimals ("47.5", 2 -> 4750). */
uint8_t Scpi_ParseScaled(const char *tok, uint8_t decimals, uint32_t *out);

/* The same, as a float ("47.5", 2 -> 47.5f). */
uint8_t Scpi_ParseUnsigned(const char *tok, uint8_t decimals, float *out);

/* Parse an argument token and, if it is not a number, answer "ERR <code> not a number: <tok>" and return 0
   (1 and *out set on success). The explicit form, for helper functions that return a value: */
uint8_t Scpi_GetLong(uart_instance_t *inst, int code, const char *tok, long *out);
uint8_t Scpi_GetFloat(uart_instance_t *inst, int code, const char *tok, float *out);

/* ---- handler shorthands (for void handlers only: they `return;` on a bad token) ---- */

#define SCPI_NEED_LONG_ERR(inst, tok, var, code) \
    do { long scpiL_; \
         if (Scpi_ParseLong((tok), &scpiL_) == 0U) { Scpi_ReplyBadNumberErr((inst), (code), (tok)); return; } \
         (var) = (__typeof__(var))scpiL_; } while (0)

#define SCPI_NEED_FLOAT_ERR(inst, tok, var, code) \
    do { float scpiF_; \
         if (Scpi_ParseFloat((tok), &scpiF_) == 0U) { Scpi_ReplyBadNumberErr((inst), (code), (tok)); return; } \
         (var) = (__typeof__(var))scpiF_; } while (0)

#define SCPI_NEED_LONG(inst, tok, var)   SCPI_NEED_LONG_ERR((inst), (tok), (var), ERR_INVALID_ARGS)
#define SCPI_NEED_FLOAT(inst, tok, var)  SCPI_NEED_FLOAT_ERR((inst), (tok), (var), ERR_INVALID_ARGS)

/* The same "not a number" reply with a chosen error code (for commands whose documented error for a bad value
   is not ERR 12, e.g. ERR 8 for a PFM_Input channel). */
void Scpi_ReplyBadNumberErr(uart_instance_t *inst, int code, const char *tok);

/* ---- a float setting as a generated pair of commands ----
 *
 *   SCPI_FLOAT_SETTING(cmd_foo, SetFoo, GetFoo, 3U, "FOO <volts>", "foo must be 0-10", "not while running")
 *
 * defines cmd_foo (one non-negative argument with up to 3 decimals, handed to SetFoo(float), which returns a
 * SCPI_SET_* value) and cmd_foo_query (replies "OK <GetFoo()>" with 3 decimals). Needs <string.h> (strtok).
 */
#define SCPI_FLOAT_SETTING(FN, SETTER, GETTER, DEC, USAGE, RANGEMSG, BUSYMSG) \
void FN(uart_instance_t *inst, char *args) \
{ \
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL; \
    float v; \
    if ((tok == NULL) || (strtok(NULL, " \r\n") != NULL) || (Scpi_ParseUnsigned(tok, (DEC), &v) == 0U)) \
    { \
        Scpi_SendErr(inst, ERR_INVALID_ARGS, USAGE); \
        return; \
    } \
    Scpi_ReplySet(inst, (int)SETTER(v), RANGEMSG, BUSYMSG); \
} \
void FN##_query(uart_instance_t *inst, char *args) \
{ \
    (void)args; \
    Scpi_ReplyFixed(inst, GETTER(), (DEC)); \
}

#endif /* SCPI_UTIL_H */
