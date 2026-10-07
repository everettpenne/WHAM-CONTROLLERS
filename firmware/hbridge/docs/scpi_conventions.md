# The serial command protocol: conventions shared by WHAM-XREX-PFMG474 and WHAM-HBRIDGE-PFMG474

Both controllers speak the same SCPI-style protocol over USART2. This file is the single description of the
parts they have in common -- framing, reply grammar, error codes, mnemonic matching and argument rules -- and
of the helper code that implements them. The identical file is in both repos; each product's own
`docs/command_reference.md` lists its commands and links here. (What the two command SETS share and where the
same name means something different: the last section.)

## Shared code

Byte-identical in both repos; `python/check_shared.py` (the same script in both) compares them with the
sibling checkout:

| File | What it is |
|---|---|
| `src/middleware/scpi/scpi_parser.c/.h` | mnemonic matching and dispatch (`scpi_match()`, `scpi_dispatch()`) |
| `src/middleware/scpi/scpi_err.h` | the numeric error codes |
| `src/middleware/scpi/scpi_util.c/.h` | building replies, strict argument parsing, number-to-text, the `SCPI_FLOAT_SETTING` generator |
| `tests/test_scpi_parser.c`, `tests/test_scpi_util.c`, `tests/test_command_table.c`, `tests/fake_uart.c/.h` | their host tests |

Per product: `src/app/commands/command_table.c` (the table of patterns and handlers), the `cmd_*.c` handlers,
`cmd_common.c/.h` (this product's fault and interlock preconditions), and `tests/known_short_collisions.inc`.

## Link and framing

USART2, **115200 8N1**. A command is one line ended by `\r`, `\n` or `\r\n`, at most 127 characters
(`UART_RX_BUF_SIZE`, 128 with the terminator). Characters beyond that are silently dropped and the truncated
line is then executed, so keep commands short (long data goes through the paged and chunked commands). One command per line: compound
(`;`-separated) commands are not supported. The controller answers each line with exactly one reply line,
except where a command's own documentation says otherwise (paged replies, firmware-update data).

## Replies

| Form | Meaning |
|---|---|
| `OK\r\n` | accepted, no data |
| `OK <field> <field> ...\r\n` | accepted, with values: space-separated fields in the order the command documents |
| `ERR <code> <message>\r\n` | rejected; the code is stable across firmware versions, the message is for humans |

Numbers in a reply are plain decimal. How many decimals a value has is per command (a fixed count such as
`OK 1.250`, or the shortest `%g` form such as `OK 0.001`), and is documented with the command. Booleans are `0`/`1`.

**Unsolicited lines** start with `!` so a host can never mistake them for a reply: `!BOOT ...` (three lines sent
once per boot, after a reset or firmware swap), and in XREX `!EVT ...` telemetry events. Everything a host sends
gets an `OK`/`ERR` line; everything beginning `!` was not asked for.

## Error codes

Codes are **never renumbered or reused**. A code either means the same thing in both products or is used by only
one; a product simply never sends a code it has no use for. `src/middleware/scpi/scpi_err.h` is the definition.

| Code | Name | Meaning | XREX | HBRIDGE |
|---|---|---|---|---|
| 1 | `ERR_UNKNOWN_COMMAND` | no pattern matched | yes | yes |
| 2 | `ERR_TABLE_NOT_UPLOADING` | a `*:STEP`/`*:END` without its `*:BEGin` (`TABle:`, and in HBRIDGE `SHOT:`, `TARGet:`, `CURVe:`) | yes | yes |
| 3 | `ERR_TABLE_FULL` | an upload table or time series is full | yes | yes |
| 4 | `ERR_TABLE_STEP_INVALID` | an uploaded step is malformed (value count, range, ordering) | yes | yes |
| 5 | `ERR_TABLE_EMPTY` | nothing uploaded to play (`FIRE`, closed-loop start) | yes | yes |
| 6 | `ERR_FAULT_LATCHED` | a fault is latched; `FAULT:CLEar` first | yes | yes |
| 7 | `ERR_QSPI_FAILED` | QUADSPI / external flash command failed or timed out | yes | yes |
| 8 | `ERR_PFMIN_BAD_CHANNEL` | PFM_Input channel not 1-6 | yes | yes |
| 9 | `ERR_PFMIN_BAD_COUNT` | `PFMIN:CAPTURE` count out of range | yes | yes |
| 10 | `ERR_OUT_OF_MODE_LIMITS` (XREX: `ERR_CARRIER_TOO_HIGH`) | a step's carrier or duty is outside the limits: HBRIDGE the selected supply mode's, XREX the maximum carrier | yes | yes |
| 11 | `ERR_INVALID_CHANNEL` | XREX: channel not 1-N. HBRIDGE: control loop not 1 (the only loop; 0 = all where accepted) | yes | yes |
| 12 | `ERR_INVALID_ARGS` | wrong argument count, not a number, or out of range | yes | yes |
| 13 | `ERR_INVALID_STATE` | not allowed in the current state (`ARM` when not IDLE, `FIRE` when not ARMED, settings changed while running, ...) | yes | yes |
| 14 | `ERR_INVALID_NICKNAME` | channel nickname rules | yes | -- |
| 15 | `ERR_EXT_ENABLE_LOW` | `EXTernal:ENAble` is on and the PF13 interlock reads LOW | yes | -- |
| 16 | (retired) | an old XREX external-trigger coupling; do not reuse | -- | -- |
| 17 | `ERR_FWUPDATE` | `FWUPdate:*` flash/CRC/image/option-byte failure | yes | yes |
| 18 | `ERR_PROFILE` | saved profiles: empty or corrupt slot, or the flash failed | yes | yes |

## Mnemonic matching (SCPI-style)

Commands are matched by `scpi_match()` against a flat table of patterns; `scpi_parser.c`'s header comment is the
full specification. Summary:

- **Hierarchical**: levels are separated by `:` (`CONFig:CLOSEDloop:DUTY`). A leading `:` on the input is
  tolerated, never required; every command is matched from the root.
- **Short/long form, per level**: in a pattern the leading UPPERCASE run is the mandatory short form; lowercase
  letters after it are an optional long-form suffix that must be matched in full if used at all. `VERSion` accepts
  `VERS`, `VERSION` and `version`, not `VERSI`.
- **Case-insensitive** throughout.
- **The query suffix `?` must match exactly** (a pattern ending in `?` matches only input ending in `?`).
- **Common commands** such as `*IDN?` are ordinary zero-colon patterns.

A new command must be reachable by its short and its long form: `tests/test_command_table.c` checks every pattern
in the real table, and a collision with an earlier row fails the build (a product lists collisions it deliberately
keeps in `tests/known_short_collisions.inc`; never add one to hide a new collision).

## Arguments (strict)

Since 2026-10-05 both products parse numeric arguments **strictly**: the whole token must be the number. Where
the old code turned garbage into 0 (`atol("abc")` is 0, `atol("12x")` is 12), the command now answers an error and
changes nothing.

| Kind | Accepted | Refused |
|---|---|---|
| integer | `[+-]` then decimal digits that fit a `long` (`12`, `-7`, `007`) | `""`, `12x`, `1.5`, `1e3`, `0x10`, `" 1"`, overflow |
| flag | `0` or `1` | anything else |
| decimal (float) | `[+-]digits[.digits][e[+-]digits]`, finite, within float range, up to 18 digits (`620.69`, `.5`, `5.`, `2.5E-2`) | `nan`, `inf`, `0x10`, `1.2.3`, `1e400`, trailing text |
| unsigned fixed point | digits with at most N decimals, as the command documents (`47.5` with 2 decimals) | a sign, more decimals than the unit has, exponents |

The refusal is `ERR 12 not a number: <token>`, or the command's own documented code where it has one: `ERR 8`
for a PFM_Input channel, `ERR 9` for a capture count, `ERR 4` inside a table step, `ERR 11` for a channel
argument (XREX). A missing argument gets the command's own usage text (`ERR 12`). A value that parses but is out
of range keeps its existing error.

## Writing a handler

`cmd_common.h` pulls in `scpi_util.h`. A handler takes `(uart_instance_t *inst, char *args)`, splits `args` with
`strtok(args, " \r\n")` and answers exactly once:

```c
void cmd_foo(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    long ch;

    if (tok == NULL) { Scpi_SendErr(inst, ERR_INVALID_ARGS, "FOO needs <channel>"); return; }
    SCPI_NEED_LONG_ERR(inst, tok, ch, ERR_INVALID_CHANNEL);     /* "ERR 11 not a number: ..." and return */
    ...
    Scpi_ReplyOk(inst);                                          /* or Scpi_ReplyFixed / Scpi_ReplyFloat3 */
}
```

`Scpi_GetLong`/`Scpi_GetFloat` are the explicit forms for helper functions that return a value (the `SCPI_NEED_*`
macros `return;`). A float setting with its query is one line, `SCPI_FLOAT_SETTING(...)`. Then add the row to
`command_table.c`, declare the handler in `commands.h`, and document the command in the product's
`command_reference.md`. Nothing here uses printf's float support (newlib-nano leaves it out unless the link adds
`-u _printf_float`): decimals are formatted with integer arithmetic (`Scpi_FormatFixed`) and `%g`-style text with
`Scpi_FormatFloat`.

## Where the two command sets overlap

53 mnemonics exist in both products, with the same handler names. **A shared name does not guarantee the same
meaning**; a host tool must select the product from `*IDN?` before assuming units or reply shapes:

| Command | XREX | HBRIDGE |
|---|---|---|
| `SOURce:SETpoint <ch> <x>` | target output frequency, Hz | target voltage, volts (the loop is `1`) |
| `SOURce:STATus?` | `OK run setpointHz measuredHz outputHz` | same four fields, in volts and % duty |
| `PID:GAINS` | per Transrex channel | per supply type (applied to the selected mode) |
| `PID:LOOPMODE` | closed loop (1) is the default | open loop (0) is the default, and it selects what `FIRE` plays |
| `LOG:DATA?` | one streamed reply | paged (`<ch> <start> <count>`, up to 40 samples) with a check sum |
| `PFMIN:DATA?` | `<ch>` | `<ch> [start [count]]`, paged with a check sum |
| `SHOT:*` | `CURRent`, `TIMing`, `STARt` (a current ramp) | `BEGin`/`STEP`/`END`/`CLEar` (a time series of frequency and duty) |
| `STATE?` | `IDLE`, `ARMED`, `FIRING`, `FAULT <type> [n]` (several fault types) | `FAULT GENERAL` only |

The remaining mnemonics are product-specific (XREX: `XREX:CHANnel:*`, `EXTernal:*`, `OCP:*`, `SYS:*`, `!EVT`
telemetry, the Transrex `SIM:*`; HBRIDGE: `ADC:*`, `DAC:*`, `PHASe:*`, `TARGet:*`, the closed-loop `CONFig:` tree,
`SIM:PLANT:*`). The vocabularies are kept separate on purpose: they describe different physics.
