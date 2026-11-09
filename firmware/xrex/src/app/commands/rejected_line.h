/*
 * rejected_line.h / rejected_line.c -- the last command line the controller refused as "Unknown command", exactly as
 * it was received (length, hex bytes, text), and how many have been refused. Added 2026-11-09 to find out what a
 * garbled reply on the bench really was: a valid poll (SOURce:STATus? 3) came back "ERR 1 Unknown command" with every
 * UART error counter at zero, so either a byte was changed on the way (bridge, wiring) or the controller mangled the
 * line itself; the bytes it held say which. Hardware-independent, host-tested (tests/test_rejected_line.c).
 */
#ifndef REJECTED_LINE_H
#define REJECTED_LINE_H

#include <stddef.h>
#include <stdint.h>

#define REJECTED_LINE_KEEP      (48U)    /* bytes of the line kept; the true length is reported separately */
#define REJECTED_LINE_TEXT_MAX  (320U)   /* enough for RejectedLine_Format() of a full line */

/* Remember `len` bytes at `raw` as the last refused line and count it. */
void RejectedLine_Record(const uint8_t *raw, size_t len);

/* How many lines have been refused since boot. */
uint32_t RejectedLine_Count(void);

/* "COUNT=<n> LEN=<true length> HEX=<hex of the kept bytes, none if empty> TEXT=<the bytes, printable ASCII as is,
   backslash as \\, anything else as \xHH>" -- TEXT last because it may contain spaces. NUL-terminated; returns the
   length. A cut-off line is marked by LEN being greater than the bytes in HEX. */
size_t RejectedLine_Format(char *buf, size_t cap);

#endif /* REJECTED_LINE_H */
