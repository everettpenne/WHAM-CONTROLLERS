/*
 * cmd_system.c
 *
 * Identification, bootloader entry, SYS:* timekeeping/telemetry,
 * DIAGnostic:OPTBytes?/RSTCause, QSPI:ID?.
 * Handlers are declared in commands.h and registered in command_table.c.
 */

#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "boot_jump.h"
#include "qspi_test.h"
#include "nvstore.h"
#include "state_machine.h"
#include "git_version.h"
#include "device_identity.h"
#include "mcu.h"
#include "telemetry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Identification / system
 * -------------------------------------------------------------------------- */

void cmd_idn(uart_instance_t *inst, char *args)
{
    char buf[160];
    char idTag[64];
    (void)args;

    /* Board + firmware identity in one line, matching the sibling
       PFM-STM32G474 project's *IDN convention (OK <value>, space-
       separated fields) -- see ctrlr_config.h for HW_BOARD_NAME/
       HW_BOARD_REV/FW_VERSION_STRING.

       4th field, FW_GIT_COMMIT, added 2026-09-11 per direct request --
       exactly which git commit THIS firmware was actually built from,
       so a host can always tell what's really running on a board, not
       just what's supposed to be flashed. See python/gen_git_version.py
       (generates build/generated/git_version.h, gitignored -- see that file's
       own header comment) and python/wham_build.py (runs it
       automatically before every build; this is the canonical way to
       build this project now). A `-dirty` suffix (FW_GIT_DIRTY) flags
       a build made with uncommitted changes -- the commit hash alone
       would otherwise silently overstate how precisely this build
       matches that commit in git history. "unknown" if git_version.h
       was never generated (git unavailable, or built some other way
       entirely) -- gen_git_version.py always writes a valid, buildable
       header either way, never blocks compiling over this. */
    /* Last field (2026-10-07): which physical board this is -- the serial
       (SN=UNSET until one is provisioned) and the MCU's factory unique ID
       (device_identity.h). Appended at the end so every parser that reads the
       earlier fields by position is unaffected. */
    (void)DeviceIdentity_FormatTag(idTag, sizeof(idTag));
    snprintf(buf, sizeof(buf), "OK %s %s %s %s%s %s\r\n",
             HW_BOARD_NAME, HW_BOARD_REV, FW_VERSION_STRING,
             FW_GIT_COMMIT, (FW_GIT_DIRTY != 0U) ? "-dirty" : "", idTag);
    uart_send(inst, buf);
}

#if (BOOT_JUMP_FEATURE_ENABLED != 0)
void cmd_boot(uart_instance_t *inst, char *args)
{
    (void)args;

    /* No state-machine/Firing concept exists in this minimal firmware
       yet -- when one is added, gate this the same way the sibling
       PFM-STM32G474 project's cmd_boot() does (reject with ERR while
       Firing; resetting under load would drop outputs uncontrolled). */

    /* uart_send() is blocking (HAL_UART_Transmit with HAL_MAX_DELAY), so
       this ACK is guaranteed to be fully on the wire before
       BootJump_RequestBootloader() resets the MCU below -- the operator
       (or a flashing script) sees "OK ENTERING BOOTLOADER" before the
       link drops. */
    uart_send(inst, "OK ENTERING BOOTLOADER\r\n");

    BootJump_RequestBootloader();
    /* Never returns. */
}
#endif /* BOOT_JUMP_FEATURE_ENABLED */

/* --------------------------------------------------------------------------
 * SYS:TIME? / SYS:TELEM? / SYS:EVENT <0|1> / SYS:EVENT?
 *
 * Telemetry contract, Phase 1 of docs/telemetry.md. `SYS:` is the namespace
 * that owns the telemetry data-contract: SYS:TIME? exposes the monotonic
 * millisecond clock (HAL_GetTick, SysTick 1 ms), SYS:TELEM? the schema
 * version (TELEMETRY_SCHEMA_VERSION, telemetry.h -- bumped on any wire-format
 * change), and SYS:EVENT gates the unsolicited `!EVT` stream (default ON).
 * The `!EVT` lines themselves are emitted by telemetry.c's
 * Telemetry_PollEmit(), not by any of these handlers.
 * -------------------------------------------------------------------------- */
void cmd_sys_time(uart_instance_t *inst, char *args)
{
    char buf[24];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %lu\r\n", (unsigned long)Mcu_GetTickMs());
    uart_send(inst, buf);
}

void cmd_sys_telem(uart_instance_t *inst, char *args)
{
    char buf[24];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned int)TELEMETRY_SCHEMA_VERSION);
    uart_send(inst, buf);
}

void cmd_sys_event(uart_instance_t *inst, char *args)
{
    char *tok;
    long  val;

    tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    if (tok == NULL)
    {
        Scpi_SendErr(inst, ERR_INVALID_ARGS, "SYS:EVENT needs one argument: 0|1");
        return;
    }
    SCPI_NEED_LONG(inst, tok, val);
    if ((val != 0L) && (val != 1L))
    {
        Scpi_SendErr(inst, ERR_INVALID_CHANNEL, "Invalid value -- must be 0 or 1");
        return;
    }

    Telemetry_SetEventEnabled((uint8_t)val);
    Scpi_ReplyOk(inst);
}

void cmd_sys_event_query(uart_instance_t *inst, char *args)
{
    char buf[16];
    (void)args;

    snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned int)Telemetry_GetEventEnabled());
    uart_send(inst, buf);
}

/* SYS:EVLOG? -- flight recorder (docs/telemetry.md Phase 4). Replays the
   retained event history as "OK <n>" then n one-line !EVT packets, oldest
   first. Delegates entirely to telemetry.c's Telemetry_ReplayFlightLog() so
   the replay and the live stream share one renderer. Unlike the gated live
   stream, this history is always recorded -- SYS:EVENT 0 silences !EVT
   without losing the post-mortem log. */
void cmd_sys_evlog(uart_instance_t *inst, char *args)
{
    (void)args;
    Telemetry_ReplayFlightLog(inst);
}

/* --------------------------------------------------------------------------
 * DIAGnostic:OPTBytes?
 *
 * TEMPORARY diagnostic, added 2026-09-17 -- direct request: is PB8
 * (this board's BOOT0 net, docs/pin_mapping_v4.csv) actually available
 * as a plain GPIO after boot, or does it stay committed to boot-mode
 * sampling? Reads the LIVE FLASH_OPTR register (FLASH->OPTR, loaded
 * from the option bytes at reset -- stm32g474xx.h's own bit
 * definitions, not guessed) rather than relying on memory of what the
 * G4 family "usually" does. The three bits that actually decide this,
 * per the CMSIS header:
 *   nBOOT0     (bit 27) -- the option-byte-supplied boot0 VALUE, used
 *              only when nSWBOOT0 (below) is 0.
 *   nSWBOOT0   (bit 26) -- 1 = boot0 is read from the physical pin
 *              (BOOT0/PB8, whichever this package/remap uses); 0 =
 *              boot0 is taken ENTIRELY from the nBOOT0 option-byte
 *              value above, and the physical pin is NOT sampled at
 *              all for boot purposes -- meaning if nSWBOOT0=0, PB8 is
 *              free for GPIO use regardless of any BOOT0/PB8 remap
 *              question, since nothing ever reads it at boot either
 *              way.
 *   nBOOT1     (bit 23) -- combines with the effective boot0 value
 *              (whichever source) to select the final boot target
 *              (main flash / system memory / SRAM).
 * Does NOT itself answer whether PB8 is safe to repurpose -- that
 * still depends on nSWBOOT0's value once read back for real, and
 * separately, on what PG10 (the other pin asked about, this board's
 * own "NRST"-labeled net) is actually wired to on the schematic, which
 * no register on this chip can reveal -- only the schematic can.
 * Remove once the PB8/PG10 GPIO-reuse question is settled. */
void cmd_diag_optbytes_query(uart_instance_t *inst, char *args)
{
    char buf[96];
    (void)args;

    mcu_option_bytes_t ob;
    Mcu_ReadOptionBytes(&ob);

    snprintf(buf, sizeof(buf), "OK OPTR=%08lX nBOOT0=%u nSWBOOT0=%u nBOOT1=%u\r\n",
             (unsigned long)ob.raw, (unsigned)ob.nBoot0, (unsigned)ob.nSwBoot0,
             (unsigned)ob.nBoot1);
    uart_send(inst, buf);
}

/* --------------------------------------------------------------------------
 * DIAGnostic:RSTCause? / DIAGnostic:RSTCause:CLEar
 *
 * TEMPORARY diagnostic, added 2026-09-17 -- direct request: while
 * testing the new PD0->fiber TX->fiber->inverting RX->PG10 loop for the
 * emergency-stop feature, DIAGnostic:GPOut11 1 (PD0 HIGH) consistently
 * returned a single garbled 0x00 byte instead of "OK\r\n", and the pin
 * never actually reached HIGH on a follow-up query -- while the
 * unrelated DIAGnostic:GPOut12 (PD1, no fiber transmitter behind it)
 * kept working perfectly the whole time. User asked whether this could
 * be (a) some PG10-specific NRST-adjacent silicon behavior, or (b)
 * power-rail droop from the fiber TRANSMITTER's own LED current
 * actually resetting the MCU -- rather than guess, this reads the
 * REAL RCC->CSR reset-cause flags (stm32g474xx.h's own bit
 * definitions -- RCC_CSR_BORRSTF/PINRSTF/SFTRSTF/IWDGRSTF/WWDGRSTF/
 * LPWRRSTF/OBLRSTF, not guessed), which is the one thing that can
 * actually distinguish "a real reset happened, and here's why" from
 * "no reset at all, something else garbled the UART byte." Nothing
 * else in this codebase clears these flags (confirmed by grep before
 * adding this), so whatever the reset cause was persists until
 * explicitly cleared here -- no special early-boot capture needed.
 *
 * On pure silicon-fact grounds (separately answered, not guessed): NRST
 * is ALWAYS a dedicated, non-GPIO pin on every STM32 (confirmed via
 * this project's own device header -- RCC_CSR_PINRSTF is the ONLY flag
 * tied to that pin, and no flag here is tied to any GPIO port at all),
 * so PG10 cannot itself BE or trigger the chip's own NRST function at
 * the silicon level. If PINRSTF is what shows up after reproducing the
 * glitch, that would mean something is asserting the REAL NRST net
 * (a board-level path, not a PG10-is-secretly-NRST one); if BORRSTF
 * shows up instead, that directly confirms the power-rail-droop
 * hypothesis. Remove once the GPOut11/PG10 diagnostic investigation is
 * settled. */
void cmd_diag_rstcause_query(uart_instance_t *inst, char *args)
{
    char buf[160];
    int  len = 0;
    (void)args;

    mcu_reset_flags_t f;
    Mcu_ReadResetFlags(&f);

    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, "OK CSR=%08lX", (unsigned long)f.raw);
    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " BOR=%u",   (unsigned)f.bor);
    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " PIN=%u",   (unsigned)f.pin);
    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " SFT=%u",   (unsigned)f.sft);
    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " IWDG=%u",  (unsigned)f.iwdg);
    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " WWDG=%u",  (unsigned)f.wwdg);
    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " LPWR=%u",  (unsigned)f.lpwr);
    len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " OBL=%u",   (unsigned)f.obl);
    snprintf(&buf[len], sizeof(buf) - (size_t)len, "\r\n");

    uart_send(inst, buf);
}

void cmd_diag_rstcause_clear(uart_instance_t *inst, char *args)
{
    (void)args;
    Mcu_ClearResetFlags();   /* resets every flag above to 0 */
    Scpi_ReplyOk(inst);
}

#if (QSPI_TEST_FEATURE_ENABLED != 0)
/* --------------------------------------------------------------------------
 * QSPI:ID?
 *
 * QUADSPI connectivity test against the W25Q128JVS wired to
 * PE12-PE15/PB10-PB11 (docs/pin_mapping_v4.csv) -- issues the flash's
 * standard JEDEC Read ID instruction (0x9F, plain 1-line mode, no Quad
 * Enable required) via QspiTest_ReadId() (qspi_test.h) and reports the
 * 3 raw bytes (Manufacturer, Memory Type, Capacity) as space-separated
 * uppercase hex, e.g. "OK EF 40 18" for a healthy Winbond part --
 * compare against the W25Q128JVS datasheet's own JEDEC ID table rather
 * than trusting any specific expected value hardcoded here (none is;
 * this command reports what the chip actually says, nothing assumed).
 * ERR 7 on any HAL_QSPI command/receive failure or timeout (e.g. no
 * chip present, a wiring fault, or the bus wedged) -- see
 * QspiTest_ReadId()'s own doc comment for exactly what that call does
 * and does not verify.
 * -------------------------------------------------------------------------- */
void cmd_qspi_id(uart_instance_t *inst, char *args)
{
    uint8_t id[3];
    char buf[32];
    (void)args;

    if (QspiTest_ReadId(id) == 0U)
    {
        Scpi_SendErr(inst, ERR_QSPI_FAILED, "QUADSPI command failed or timed out");
        return;
    }

    snprintf(buf, sizeof(buf), "OK %02X %02X %02X\r\n",
             (unsigned int)id[0], (unsigned int)id[1], (unsigned int)id[2]);
    uart_send(inst, buf);
}
/* --------------------------------------------------------------------------
 * QSPI:SELFtest
 *
 * Bench proof of the external-flash storage driver (nvstore.h) against the
 * real W25Q128JVS, added 2026-09-29 before anything is built on top of it.
 * An action, not a query (it erases and programs), so no '?'. Works only on
 * the reserved scratch sector (NVSTORE_SCRATCH_ADDR, last sector of the
 * chip) and leaves it erased. IDLE only: a sector erase blocks the main loop
 * (serial link included) for tens of milliseconds.
 *
 * Steps: read the three status registers; erase the sector (the driver
 * checks it reads back blank); program a 300-byte pattern that starts
 * mid-page and crosses a 256-byte page boundary; read it back and compare;
 * read the WHOLE sector and check every byte outside the pattern is still
 * 0xFF (catches a page program that wrapped around inside its page and
 * clobbered earlier bytes); erase again.
 *
 * Reply: "OK PASS sr=<SR1>,<SR2>,<SR3> erase=<ms> prog=<ms> read=<ms>" (hex
 * status registers; times from the 1 ms tick, so +-1 ms), or
 * "ERR 7 selftest <step>: <reason>" naming the first step that failed.
 * ERR 13 if not IDLE.
 * -------------------------------------------------------------------------- */
#define SELFTEST_PATTERN_OFFSET   (200U)   /* 200..499: crosses the 256-byte page boundary */
#define SELFTEST_PATTERN_LEN      (300U)
#define SELFTEST_READ_CHUNK       (256U)

static uint8_t SelftestPatternByte(uint32_t i)
{
    return (uint8_t)((i * 7U) + 0x5AU);
}

static void SelftestFail(uart_instance_t *inst, const char *step, nvstore_status_t st)
{
    char msg[64];

    snprintf(msg, sizeof(msg), "selftest %s: %s", step, NvStore_StatusText(st));
    Scpi_SendErr(inst, ERR_QSPI_FAILED, msg);
}

void cmd_qspi_selftest(uart_instance_t *inst, char *args)
{
    uint8_t pattern[SELFTEST_PATTERN_LEN];
    uint8_t chunk[SELFTEST_READ_CHUNK];
    uint8_t sr[3];
    uint32_t t0;
    uint32_t eraseMs;
    uint32_t progMs;
    uint32_t readMs;
    char buf[96];
    nvstore_status_t st;
    (void)args;

    if (SM_GetState() != SM_STATE_IDLE)
    {
        Scpi_SendErr(inst, ERR_INVALID_STATE, "QSPI:SELFtest only in IDLE");
        return;
    }

    if ((st = NvStore_ReadStatusRegs(sr)) != NVSTORE_OK)
    {
        SelftestFail(inst, "status", st);
        return;
    }

    t0 = Mcu_GetTickMs();
    if ((st = NvStore_EraseSector(NVSTORE_SCRATCH_ADDR)) != NVSTORE_OK)
    {
        SelftestFail(inst, "erase", st);
        return;
    }
    eraseMs = Mcu_GetTickMs() - t0;

    for (uint32_t i = 0U; i < SELFTEST_PATTERN_LEN; i++)
    {
        pattern[i] = SelftestPatternByte(i);
    }
    t0 = Mcu_GetTickMs();
    if ((st = NvStore_Program(NVSTORE_SCRATCH_ADDR + SELFTEST_PATTERN_OFFSET, pattern,
                              SELFTEST_PATTERN_LEN)) != NVSTORE_OK)
    {
        SelftestFail(inst, "program", st);
        return;
    }
    progMs = Mcu_GetTickMs() - t0;

    /* Read the whole sector back, independently of the driver's own
       verify: the pattern where it belongs, 0xFF everywhere else. */
    t0 = Mcu_GetTickMs();
    for (uint32_t off = 0U; off < NVSTORE_SECTOR_BYTES; off += SELFTEST_READ_CHUNK)
    {
        if ((st = NvStore_Read(NVSTORE_SCRATCH_ADDR + off, chunk, SELFTEST_READ_CHUNK)) != NVSTORE_OK)
        {
            SelftestFail(inst, "read", st);
            return;
        }
        for (uint32_t i = 0U; i < SELFTEST_READ_CHUNK; i++)
        {
            uint32_t pos = off + i;
            uint8_t want = 0xFFU;

            if (pos >= SELFTEST_PATTERN_OFFSET && pos < (SELFTEST_PATTERN_OFFSET + SELFTEST_PATTERN_LEN))
            {
                want = SelftestPatternByte(pos - SELFTEST_PATTERN_OFFSET);
            }
            if (chunk[i] != want)
            {
                snprintf(buf, sizeof(buf), "selftest compare: offset %u is %02X, expected %02X",
                         (unsigned int)pos, (unsigned int)chunk[i], (unsigned int)want);
                Scpi_SendErr(inst, ERR_QSPI_FAILED, buf);
                return;
            }
        }
    }
    readMs = Mcu_GetTickMs() - t0;

    if ((st = NvStore_EraseSector(NVSTORE_SCRATCH_ADDR)) != NVSTORE_OK)
    {
        SelftestFail(inst, "final erase", st);
        return;
    }

    snprintf(buf, sizeof(buf), "OK PASS sr=%02X,%02X,%02X erase=%lums prog=%lums read=%lums\r\n",
             (unsigned int)sr[0], (unsigned int)sr[1], (unsigned int)sr[2],
             (unsigned long)eraseMs, (unsigned long)progMs, (unsigned long)readMs);
    uart_send(inst, buf);
}

#endif /* QSPI_TEST_FEATURE_ENABLED */

/* --------------------------------------------------------------------------
 * SYS:ID? -- OK serial=<sn> uid=<24 hex> flashuid=<16 hex> product=<name> hw=<rev> fw=<version> commit=<hash> label=<text> verified=<0|1>
 *
 * Which physical board this is (device_identity.h; design in
 * docs/device_identity_and_host_service.md at the top of the repository). One line of
 * key=value fields: a host reads fields by key, so new ones can be added at the end.
 * `serial` is UNSET and `verified` 0 until a serial is provisioned. `uid` is the
 * MCU's factory unique ID, `flashuid` the flash chip's (0000000000000000 if the chip
 * did not answer). Identical in both products.
 * -------------------------------------------------------------------------- */
void cmd_sys_id_query(uart_instance_t *inst, char *args)
{
    char buf[224];
    char body[200];
    (void)args;

    (void)DeviceIdentity_FormatId(body, sizeof(body), HW_BOARD_NAME, HW_BOARD_REV, FW_VERSION_STRING,
                                  (FW_GIT_DIRTY != 0U) ? FW_GIT_COMMIT "-dirty" : FW_GIT_COMMIT);
    snprintf(buf, sizeof(buf), "OK %s\r\n", body);
    uart_send(inst, buf);
}
