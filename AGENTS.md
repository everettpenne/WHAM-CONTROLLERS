# AGENTS.md — WHAM-HBRIDGE-PFMG474 Firmware

Orientation file for any AI agent or LLM working in this repository.
Read this before touching code.

## What this project is

Firmware for the **H-bridge power supplies** of the WHAM PFM/HRTIM
controller family, on the STM32G474QET6 (Cortex-M4F, 170 MHz, LQFP128):
the **Limiter** (driven by PWM) and the **HVPS** (driven by PFM). Both
are 3-channel outputs -- Phase U+UN, Phase V+VN, Phase W+WN, complements
with dead time inserted by the HRTIM -- and both read back 12 gate-drive
signals as fault inputs. The firmware generates phase-locked,
table-driven multi-channel PWM/PFM on the HRTIM and takes commands over
a serial (SCPI-style) link.

**Naming and history**: this repo started as `WHAM-PFMG474-V4` (the
rewrite of the older PFM-STM32G474 firmware for the respun PCB), was
copied on 2026-09-09 as `WHAM-SWITCH-PFMG474` ("switching-supply
application"), and was renamed `WHAM-HBRIDGE-PFMG474` on 2026-09-29
because that name says what the hardware is. It is the direct
continuation of that firmware (`FW_VERSION_STRING` was not reset). The
sibling `WHAM-XREX-PFMG474` (closed-loop PID controller for the Transrex
magnet supplies, same PCB) was forked from it first and has since grown
platform features -- layered source layout, in-application firmware
update, boot diagnostics, an operating-state machine, saved settings
profiles, telemetry -- that are being ported back here in phases (the
plan and its status: `docs/changelog.txt`). Everything below the
"Current functionality" heading describes this firmware as it was at the
rename; the sections are updated as each phase lands.

**Provenance**: reseeded (2026-09-09) from
[`WHAM-PFMG474-V4`](https://github.com/everettpenne/WHAM-PFMG474-V4) at
its commit `de83324`, as a genuinely **independent git repository, not a
GitHub fork** -- no shared git history, matching this project's own
established convention for sibling projects. **WHAM-PFMG474-V4 itself
was not modified to create this repo**; it continues to exist,
unchanged, as the original name for the same codebase (whether it stays
as an archive is the maintainer's call). Historical entries in
`docs/changelog.txt` predate the renames and are kept verbatim -- they
are still this codebase's own history.

**Sibling projects**: `../../PFM-STM32G474` (referred to below as "V3")
and `../../PFM-STM32G474-V4` ("V4") are separate git repositories for
the earlier hardware revision(s) this codebase's own architecture was
originally ported from -- further along and much larger in scope (PWM
generation, closed-loop feedback, fault handling, a GUI).
`WHAM-XREX-PFMG474` is a further sibling, reseeded from this
project's own `de83324` for a different application on the same board.
None of these share git history with each other or with this repo. Code
ported over is called out explicitly below and in each file's own
comments -- check there before assuming a mechanism is unique to this
project or before "fixing" something that was a deliberate port.

## Current functionality (as of 2026-09-08, unchanged by the rename)

Configurable-channel-count PWM generation (HRTIM1, 1-5 channels
phase-locked to the Master timer, set by `HRTIM_NUM_CHANNELS` in
`Core/Inc/ctrlr_config.h` -- 3 by default, matching this board's actual
current wiring; channels beyond N stay pin/dead-time-reserved but
unlocked, up through Timer F) plus the serial command layer. A PFM
table can be uploaded, its length confirmed, and fired: `FIRE` starts
playback from step 0 via the HRTIM master-repetition interrupt, and it
auto-stops when the table is exhausted. Since 2026-09-29 (Phase 3) an
operating-state machine gates it: `FIRE` needs `ARM`, every shot needs its
own `ARM`, and every fault latches `FAULT` -- see the bullet below.

- **Supply mode** (added 2026-09-29; `src/app/control/supply_mode.c`,
  limits in `ctrlr_config.h`): the operator selects `CONFig:MODE
  LIMiter|HVPS` and every `TABle:STEP` is checked against the mode's
  carrier and duty range -- LIMITER (PWM) 75-125 kHz / 5-95 % per-phase duty,
  HVPS (PFM) 10-30 kHz / 40-60 % duty (the older PFM-STM32G474 project's
  values). Boot mode is NONE: no table can be uploaded until one is chosen.
  A mode change is IDLE-only and discards the table. This replaced the old
  single 100 kHz carrier ceiling (`ERR 10` now covers both frequency and
  duty limits). **100-125 kHz (LIMITER) is inside the range but has not been
  scoped on real hardware** -- the SET/RESET-collision fix was only
  confirmed up to 100 kHz. Host-tested (`tests/test_supply_mode.c`).
- **Operating-state machine** (added 2026-09-29, Phase 3 of the port;
  `src/app/control/state_machine.c`, `task_faults.c`, `cmd_state.c`):
  IDLE -> `ARM` -> ARMED -> `FIRE` -> FIRING -> table ends -> IDLE;
  `DISARM`; `STATE?`. **One fault type by design** (`SM_FAULT_GENERAL`):
  PC10/HRTIM1_FLT6, any of the 12 GateDriverStatus inputs, or
  `GENERAL:TEST:FAULT` puts it in FAULT from any state with an immediate
  `PFM_ForceStop()` (no ramp-down -- unlike WHAM-XREX-PFMG474, whose
  PID-driven ramps and per-channel fault types were deliberately not
  ported). The outputs are made safe before the state machine even sees the
  fault (PC10 in silicon, GateDriverStatus from its EXTI handler); the state
  machine keeps it latched. `FAULT:CLEAR` -> IDLE only if both inputs
  re-check healthy. Shot completion and fault latching are both detected in
  `SM_PollFaults()` (main loop, interrupts masked) -- no state-machine call
  from any ISR. Gates: `FIRE` (ERR 13 unless ARMED), `TABLE:BEGIN`/`STEP`
  (ERR 13 while FIRING), `BOOT` (ERR 13 while ARMED/FIRING), `FWUPdate:*`
  (ERR 13 unless IDLE). Host-tested (`tests/test_state_machine.c`, fakes
  for the PFM engine, HRTIM fault flag, gate-driver latch and IRQ masking).
  Not ported yet: external enable/trigger (PF13/PF15), DEBUG:FAULT:BYPASS,
  telemetry events (Phase 5).
- **Firmware update, boot diagnostics, `!BOOT` banner** (added
  2026-09-29, Phase 2 of the port from WHAM-XREX-PFMG474; `flash_bank.c`,
  `boot_diag.c`, `cmd_fwupdate.c`, `cmd_system.c`): `FWUPdate:*`
  dual-bank in-application update over the serial link (works through the
  ethernet bridge), reset-surviving `.noinit` boot record, an unsolicited
  three-line `!BOOT` banner every boot, `DIAGnostic:OPTBytes?`/`RSTCause?`,
  and the git commit in `*IDN?`. See `docs/command_reference.md`. Host tool:
  `python/fw_update.py` (plus `net_broker.py`/`net_client.py`/
  `net_terminal.py` for the ethernet bridge). Verified on the WHAM-XREX
  simulator board -- see `docs/changelog.txt`.
- **Serial command architecture** (`uart.c`, `cmd_parser.c`) -- ported
  from the sibling project, ported *architecture-only* (no command
  handlers came with it). Interrupt-driven single-byte USART2 RX with
  line-buffer accumulation (`uart.c`), polled from the main loop.
  Dispatch is a **flat, SCPI-style hierarchical command table**
  (`cmd_parser.c`) -- see `docs/command_reference.md` for the full
  mnemonic-matching rules (short/long form, `:`-separated levels, `?`
  query suffix). This is a genuine departure from the sibling project's
  case-sensitive exact-match dispatch; see that file's header comment
  for the reasoning.
- **`*IDN?`** (`commands.c`, constants in `ctrlr_config.h`) -- reports
  board name, hardware revision, and firmware version. `HW_BOARD_REV`
  in `ctrlr_config.h` is a placeholder; keep it in sync with the
  actual PCB silkscreen. (`version.h` no longer exists -- condensed
  into `ctrlr_config.h`, 2026-09-08; see `docs/changelog.txt`.)
- **`BOOT`** / serial-bootloader entry (`boot_jump.c`) -- software jump
  into the STM32 ROM bootloader over USART2, no physical BOOT0/NRST
  access needed. Ported from the sibling project's `boot_jump.c`, with
  two additions made here:
  1. **Removable by design**: `BOOT_JUMP_FEATURE_ENABLED` in
     `boot_jump.h` is the single point of control -- see that header's
     doc comment for exactly what disabling it does and does not remove.
  2. **A real bugfix, found and confirmed on hardware in this project**
     (not present in the sibling project's copy as of this writing --
     see "Known gaps" below): the ROM bootloader's `Go` command (what
     `stm32flash -g` issues after writing a new image) does **not**
     perform a real chip reset, so `SCB->VTOR` and `SYSCFG->MEMRMP` can
     be left pointing at the bootloader's own vector table. Without a
     fix, the newly-flashed app runs but is deaf to every
     interrupt-driven peripheral (including the USART2 RX interrupt
     everything here depends on) until something -- a manual reset --
     clears it. `BootJump_CheckAndEnter()` now restores both
     unconditionally on the normal-boot path. See that function's doc
     comment for the full mechanism, and `docs/changelog.txt` for the
     debugging history (this was chased down empirically, on real
     hardware, over several flash/reset cycles).
- **Host-side tooling** (`python/`):
  - `wham_build.py` -- **the canonical way to build this project**
    (added 2026-09-29, ported from WHAM-XREX-PFMG474): git-version
    header, build-list and layout checks, unit tests, `make`, the `.bin`
    regeneration, and publishing to `build/` -- see "Build & verify".
  - `sync_build_sources.py` -- regenerates `Debug/`'s makefile lists and
    `.cproject`'s include paths/source folders from the source tree.
    Run it after adding, moving or renaming any source file or folder.
  - `check_layout.py` -- enforces "Source layout"; run by `wham_build.py`.
  - `gen_git_version.py` -- regenerates `build/generated/git_version.h`
    (commit hash + dirty flag). Gitignored output; not yet used by any
    firmware source (the `!BOOT` banner and `*IDN?` will report it in a
    later phase).
  - `fw_update.py` -- network/serial firmware update using the firmware's
    own `FWUPdate:*` commands (added 2026-09-29): loads `build/` image into
    the inactive bank, verifies, swaps, checks the new banner; `--rollback`,
    `--status`, `--no-swap`. Works through the ethernet serial bridge (the
    ROM bootloader's 8E1 cannot). `net_broker.py` / `net_client.py` /
    `net_terminal.py` share the bridge's single client slot between people
    and scripts. The IP addresses and jump host in their headers are
    placeholders -- fill in your own tunnel.
  - `pfm_shot_test.py` -- fires a programmed frequency/duty ramp with the
    PFM_Input capture armed (PFMIN:*), reads it back (re-reading any reply
    damaged by the ethernet bridge), and plots programmed vs measured
    frequency with the phase-lock model residual (added 2026-09-30). Wiring
    assumed: PFM_Input_1..4 = U, V, W, UN over fiber (`--map` changes it).
    `--selftest` needs no hardware; `--from-json` replays a saved shot. When
    a phase and its complement are both wired it also reconstructs how the
    compare value (duty) MOVED from (UN period - U period) -- the capture
    still measures period only, so the duty LEVEL is not available (see
    docs/changelog.txt).
  - `wham_serial_flash.py` -- one-command serial reflash (`BOOT` +
    `stm32flash`). Ported from the sibling project's
    `pfm_serial_flash.py`, corrected for this project's actual baud and
    build artifact name. **Verified working end-to-end on real hardware**,
    including the VTOR/MEMRMP fix above (confirmed: flash a new version,
    query `*IDN?` immediately, no manual reset, get the new version back
    -- done twice, back to back).
  - `scpi.py` -- an interactive serial terminal (not written by an
    agent; predates this documentation pass). Useful for manual
    poking at the command set.
  - `pfm_table_upload.py` -- builds one of 5 fixed PFM shot profiles
    (see below) and uploads it via `TABLE:*`.
  - `dsl_viewer.py` -- interactive viewer for saved DSLogic/DSView
    `.dsl` capture files (not this project's firmware/protocol tooling
    -- a bench-instrumentation aid, generalized from several one-off
    hand-decode scripts used during the cold-start phase-lock
    investigation, 2026-09-04). Stacked digital-trace plot
    (matplotlib), scroll-to-zoom on the time axis, and a two-cursor
    click-to-measure readout (time delta + implied frequency) --
    requires `pip install matplotlib numpy`. See its own header
    comment for the `.dsl` format details (reverse-engineered from real
    captures, not from any DSView spec) and full usage.
  - `pfm_input_plot.py` -- pulls whatever `PFMIN:DATA?` capture is
    currently sitting in the controller and plots it per channel
    (period + frequency, outlier-robust y-scaling). Read-only: doesn't
    arm a capture or `FIRE` -- run those yourself first.
  - `memory_report.py` -- flash/RAM footprint snapshot and lifetime
    history, added 2026-09-09. **Standing instruction: run this after
    any commit that changes flash/RAM usage** (same "update proactively,
    not just when asked" convention as `docs/changelog.txt`) --
    `python3 memory_report.py history` (appends one row to
    `docs/memory_history.csv` for the current `HEAD`) then
    `python3 memory_report.py report` (regenerates
    `docs/memory_report.html` from that CSV), and commit both files
    alongside the code change. `python3 memory_report.py history
    --backfill` rebuilds the whole CSV from every past commit that has
    a `Debug/WHAM-PFMG474-V4.elf` checked in -- only needed once, or if
    the CSV is ever lost/corrupted. Per-module attribution is read off
    the **linked ELF's own retained symbol table**
    (`arm-none-eabi-nm --print-size -l`), not summed `.o` files -- with
    `-ffunction-sections`/`-fdata-sections`/`--gc-sections` (this
    project's Makefile), a raw per-`.o` `size` counts code the linker
    later discards as unreachable, wildly overcounting HAL driver files
    where only a handful of functions per file actually ship. See the
    script's own header comment for the full method (including the
    Default_Handler weak-alias dedup, and why `size`'s own totals --
    not the module sum -- are always the authoritative numbers, with
    any gap rolled into an explicit "Padding / unattributed" bucket).
- **PWM/HRTIM engine** (`hrtim.c`, `pfm.c`) -- originally ported from
  the sibling project's hardcoded 3-channel design and wired into
  `main.c` (see `docs/changelog.txt`, 2026-09-04 entries, for the full
  CubeMX-vs-hand-code saga this took to get right -- read that before
  touching `hrtim.c`'s `HAL_HRTIM_MspInit()` or the `.ioc`), then
  generalized (2026-09-08) to a compile-time channel count,
  `HRTIM_NUM_CHANNELS` in `ctrlr_config.h`, 1-5. Channels 0..N-1 are
  phase-locked to the Master timer (channel 0 via `MASTER_PER`,
  channels 1..N-1 via `MASTER_CMP1..MASTER_CMP(N-1)`, evenly spaced --
  only 4 Master compare units exist, capping this scheme at 5 synced
  channels, not 6); channels N..5 (up through Timer F) stay
  pin/dead-time-reserved but unlocked, exactly as D/E/F behaved before
  this generalization, and are never started by `HRTIM1_PWM_Start()`.
  See `ctrlr_config.h`'s own comment for why a 6th phase-locked channel
  isn't a simple extension (real HAL support exists for it --
  cross-timer `HRTIM_TIMRESETTRIGGER_OTHERx_CMPy` reset triggers -- but
  needs new phase math with no existing pattern here, deliberately left
  as a follow-up).
- **`TABLE:BEGIN`/`STEP`/`END`/`?`, `CONFig:CHANnels?`** (`commands.c`,
  primitives in `pfm.c`) -- uploads a complete PFM table (`PFM_Step_t`
  entries, each now `per` + one compare value per channel) built
  entirely off-controller. Firmware does zero construction/validation
  beyond "does it fit" -- see `pfm.h`'s "ADDED" header note.
  `CONFig:CHANnels?` (`OK <N>`) reports `HRTIM_NUM_CHANNELS` so host
  tooling can confirm a board's actual channel count rather than
  assuming it. `python/pfm_table_upload.py` is the reference builder: 5
  fixed profiles today (constant 50% duty; 25%->75% ramp; 90%->10%
  ramp; 10%->90% ramp; 90%->25% ramp), selected by editing a constant
  before running -- it queries `CONFig:CHANnels?` itself at startup and
  refuses to upload on a mismatch against its own `HRTIM_NUM_CHANNELS`
  constant.
  `FIRE` plays an uploaded table back -- see "Current functionality"
  above.
- **`FAULT?`/`FAULT:CLEAR`, two independent fault sources unified at
  the command layer** (`hrtim.c`/`hrtim.h`, `gate_driver.c`/
  `gate_driver.h`, gated into `pfm.c`/`commands.c`) -- `FAULT?`/
  `FAULT:CLEAR`/`FIRE`'s `ERR 6` check ALWAYS mean "either source,"
  via `commands.c`'s `AnyFaultLatched()`. The two sources themselves
  stay structurally separate, two different mechanisms:
  1. **PC10/HRTIM1_FLT6, native HRTIM hardware fault input** --
     PC10 is wired directly to HRTIM1's own FLT6 fault input on this
     board (`docs/pin_mapping_v4.csv`), active-low. Configured as a
     genuine silicon-level protection
     (`HAL_HRTIM_FaultConfig()`/`FaultModeCtl()`,
     `pOutputCfg.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE`) --
     fault-enabled outputs are forced safe autonomously, in hardware,
     the instant the pin trips, no CPU/interrupt/polling involved.
     Deliberately **not** the sibling PFM-STM32G474 project's
     software-polled `HRTIM1_EmergencyStop()` approach. **Verified on
     hardware so far**: idle reads as healthy (PC10 idle-high isn't a
     spurious trip) and normal `FIRE` still works. **Not yet
     verified**: an actual PC10-low trip on scope/DSLogic, or its
     specific contribution to the recovery sequence -- needs the
     bench, see `docs/changelog.txt`.
  2. **GateDriverStatus_01..12 (PE0-PE11), software/EXTI-driven fault
     interrupt** -- added 2026-09-08 while investigating why a fault
     wasn't being registered (these pins weren't even configured as
     GPIO inputs before that day). `MX_GPIO_Init()` configures
     PE0-PE11 `GPIO_MODE_IT_RISING_FALLING`/`GPIO_NOPULL`, backing 7
     EXTI IRQ handlers (`stm32g4xx_it.c`) that all call
     `GateDriver_CheckFault()` (`gate_driver.c`): re-reads all 12 pins
     bitwise and evaluates them against the compile-time
     `GDS_FAULT_POLARITY` (`ctrlr_config.h`, `NORMALLY_HIGH` or
     `NORMALLY_LOW`) -- if any pin is in its fault state, immediately
     `PFM_ForceStop()`s HRTIM output and latches. One boot-time
     explicit call (`main.c`) also runs this check once, closing a
     real gap: EXTI is edge-triggered, so a pin already in its fault
     state before the interrupt is even configured produces no edge
     and would otherwise go undetected until something happened to
     toggle it. `GateDriver_FaultClear()` re-validates immediately
     after clearing (calls `GateDriver_CheckFault()` again) for the
     same reason -- unlike PC10's hardware latch, which re-trips on
     its own if still physically tripped, a still-bad GateDriverStatus
     pin needs this explicit recheck or a clear would silently "fix"
     a fault that's still there. **Verified on real hardware,
     end-to-end**: `GDS_FAULT_POLARITY = GDS_NORMALLY_LOW` was set
     after confirming this board's actual gate driver wiring; the
     board's GateDriverStatus_03 (PE2) was already HIGH (a fault under
     that polarity) at boot -- `FAULT?` correctly read `OK 1`
     immediately after flashing, `FIRE` was correctly rejected with
     `ERR 6`, and `FAULT:CLEAR` correctly re-latched (`FAULT?` back to
     `OK 1`) rather than clearing a fault that was still physically
     present.

  Both sources: latched-until-cleared by design (matching the sibling
  project's Lockout/`CLEARFAULTS` convention); `FAULT:CLEAR` never
  itself reconnects outputs -- that's the next explicit `FIRE`'s job.
  `docs/command_reference.md` documents `GDS?` and `FAULT?`/
  `FAULT:CLEAR`/`ERR 6` -- the GateDriverStatus source's real-hardware
  verification (immediately above) was thorough enough on its own that
  the writeup no longer waits on the PC10-low bench trip, which stays
  flagged as the one still-open item there instead.
- **`GDS?`** (`gate_driver.c`/`gate_driver.h`, `commands.c`) -- raw
  HIGH/LOW snapshot of the same 12 GateDriverStatus_01..12 pins.
  Direct `GPIOE->IDR` read, no debounce, no polarity interpretation,
  no fault-latching of its own -- unaffected by and independent of the
  fault interrupt described above (same pins, separate mechanism). One
  `OK 01=HIGH 02=LOW ...` line, PE0 first. **Verified on hardware**:
  stable, repeatable across back-to-back queries.
- **`QSPI:ID?`** (`qspi_test.c`/`qspi_test.h`, `commands.c`) -- QUADSPI
  connectivity test against a W25Q128JVS NOR flash on
  PE12-PE15/PB10-PB11 (all AF10, `docs/pin_mapping_v4.csv`), added
  2026-09-08. Deliberately narrow: the chip's standard JEDEC Read ID
  instruction (`0x9F`, plain 1-line mode) and nothing else -- no
  program/erase, no memory-mapped access. **Easily removable by
  design**, following `boot_jump.h`'s exact pattern:
  `QSPI_TEST_FEATURE_ENABLED` (`qspi_test.h`) is the single point of
  control -- confirmed genuinely removable, not just structured to
  look that way: building with it forced to 0 compiles clean and drops
  2308 bytes of `.text`, with `QSPI:ID?` disappearing from the command
  table entirely (falls through to `ERR 1` like any unknown mnemonic).
  Required copying `stm32g4xx_hal_qspi.c`/`.h` into `Drivers/` (never
  present in this project before -- QUADSPI was never enabled via
  CubeMX here) from `STM32Cube_FW_G4_V1.6.3`, chosen specifically
  because it bundles HAL Driver V1.2.7, an exact version match against
  every other driver file already in this project (confirmed via each
  file's own version banner, not assumed).
  **Verified on real hardware, with a real bug found and fixed in the
  process**: the first attempt returned a stable "DE 80 30" instead of
  Winbond's real "EF 40 18" -- confirmed this was a precise **1-bit
  sample-timing error**, not noise (`EF4018 << 1 == DE8030` exactly, as
  one continuous 24-bit stream). Root cause: `SampleShifting` was
  `QSPI_SAMPLE_SHIFTING_HALFCYCLE` (the usually-recommended default);
  switching to `QSPI_SAMPLE_SHIFTING_NONE` fixed it immediately and
  repeatably (`OK EF 40 18`, correct Winbond manufacturer ID + W25Q128
  memory type/capacity, 3x confirmed) -- see `qspi_test.c`'s own
  bugfix comment and `docs/changelog.txt` for the fuller writeup. This
  confirms the new QUADSPI hardware itself (pins, traces, the
  W25Q128JVS) is correctly wired and responding, not just that the
  firmware compiles.
- **Shot = time series of setpoints (2026-10-01; -O2 build and 100-entry ring, 2026-10-02)**: `SHOT:BEGin/STEP/END`
  (`cmd_shot.c`), `shot_profile.c` holds them and the generator that walks the curve
  one switching period at a time into the per-period RING table in `pfm.c`
  (`PFM_TABLE_SIZE`, refilled by a priority-2 software interrupt; an empty ring before
  the shot ends is a latched fault, `FAULT:SOURces?` TABLE=1). First setpoint must be
  t=0, linear ramps between setpoints, limits checked per setpoint. Sizes are
  `ctrlr_config.h` constants and the default ring (1,300) is sized from measurements,
  not taste: see the comment there and docs/changelog.txt 2026-10-01 before shrinking
  it. The project builds at -O2 (changed 2026-10-02; it was -O0, and the refill could
  not keep up above ~80 kHz with a step every period). The generator is
  ported exactly in `pfm_shot_test.py generate_profile()`; change both together.
- **PFM_Input capture design (2026-09-30)**: bounded NORMAL-mode DMA straight into
  the per-channel result array (`pfm_input.c`), started just BEFORE the outputs;
  no per-edge or per-chunk CPU work. `PFMIN:EDGes <ch> RISING|BOTH` selects raw
  both-edge capture (duty from high time); `PFMIN:RAW? <ch> [<start> [<count>]]`
  returns the stream in checksummed pages -- the ethernet serial bridge loses a
  character at a fixed place in replies over ~1.7 kB, so read pages, not the whole
  stream. Do not go back to a circular buffer read by the DMA interrupt: it
  starved the last channel at 100 kHz (docs/changelog.txt, 2026-09-30). Tools:
  `pfm_edge_test.py`, `pfm_edge_plot.py`. Known, separate: the table interrupt
  saturates the CPU above ~93 kHz on ramps.
- **`PFMIN:CAPTURE`/`STATus?`/`DATA?`** (`pfm_input.c`/`pfm_input.h`,
  hooked into `pfm.c`, `commands.c`) -- per-period **period-only**
  measurement (rising-edge-to-rising-edge; duty/time-high was dropped,
  see below) on the 6 PFM_Input_01..06 pins
  (PA15/PD4/PB2/PC12/PB4/PD12, TIM2/TIM3/TIM4/TIM5), added 2026-09-08.
  **Capture is synchronized to PFM shot lifetime, not a standalone
  command** -- `PFMIN:CAPTURE <M>` only arms a target period count per
  channel (`PfmInput_Arm()`); the actual `HAL_TIM_IC_Start_IT()` calls
  happen inside `PFM_Restart()` (`pfm.c`), the same function that
  starts `HRTIM1_PWM_Start()`, via a `PfmInput_OnShotStart()` call
  right alongside it -- so capture begins at the literal same call as
  the shot, not a separately-timed command. `PfmInput_OnShotEnd()`,
  called from everywhere `pfm.c` stops a shot, bounds a capture's
  runtime to the shot that started it -- no separate, arbitrarily-
  chosen timeout exists or was needed. Each of the 6 channels captures
  fully independently, plain `TIM_ICPOLARITY_RISING`, armed once at
  init and never changed again. **Easily removable by design**
  (`PFM_INPUT_FEATURE_ENABLED`, `pfm_input.h`), same pattern as
  `boot_jump.h`/`qspi_test.h` -- confirmed genuinely removable, and
  this check caught a real linkage gap before it shipped: `extern
  TIM_HandleTypeDef htim2..htim5` and `stm32g4xx_it.c`'s 4
  `IRQHandler()`s referencing them must be guarded identically to
  `pfm_input.c`'s own definitions, or disabling the feature fails to
  link -- confirmed by actually building the disabled configuration,
  not just re-reading the fix. `stm32g4xx_hal_tim.c`/`.h`/`_ex` copied
  into `Drivers/` from `STM32Cube_FW_G4_V1.6.3` (same HAL-V1.2.7
  exact-version-match reasoning as the QSPI driver copy).
  **Command-layer and shot-synchronized start/stop plumbing verified
  on real hardware.**
  **Duty-cycle measurement was attempted and dropped, NOT
  root-caused.** Two real-hardware designs were tried for measuring
  time-high (both rise and fall on every channel):
  `TIM_ICPOLARITY_BOTHEDGE` + live-GPIO edge disambiguation (a genuine
  race -- HAL clears `CCxIF` before the callback runs, so a later edge
  can silently overwrite `CCR` without tripping the overcapture flag),
  then an alternating-single-polarity redesign (flip
  `TIM_ICPOLARITY_RISING`/`_FALLING` directly via `CCER` inside the
  ISR -- the standard, architecturally-correct STM32 technique). Both
  produced real signals wired to `PFM_Input_01` reading back
  *consistently one full real period late* (measured 20/20 periods:
  reported high == real period + real high; reported period == 2x real
  period) -- see `docs/changelog.txt` for the full diagnostic history,
  including a disassembly-confirmed CCxE-toggle fix (the documented
  STM32 requirement for a clean polarity change) that compiled exactly
  as intended and still made zero measured difference. A follow-up
  isolation test (STM32's native PWM Input Mode, cross-connecting one
  timer's CH1+CH2) caused two real hangs requiring physical power-
  cycles and was abandoned before yielding data. **Decision**: rather
  than continue chasing an unexplained bug whose one common factor was
  an in-ISR polarity change, this module now measures period only,
  armed `RISING` once and never touched again -- no polarity flip
  exists anywhere in this file any more. **Confirmed fixed on real
  hardware, same day**: repeated the phase-U-into-`PFM_Input_01` test
  (identical 100-entry/100 kHz/40%-duty table) -- `PFMIN:DATA?`
  reported the real period (1699 ticks) 20/20 times, `OVERCAP=0`,
  cross-checked against a fresh DSLogic capture of the same shot
  (48 real periods averaging 1697.1 ticks at 170 MHz, within ordinary
  cross-clock quantization noise of 1699 -- nothing like the old bug's
  exact 2x signature). Root cause of the original bug is still not
  explained -- sidestepped, not fixed -- but the measured data is now
  correct. `docs/command_reference.md` does not document these
  commands yet. Restricted to `PFM_Input_01` only, via
  `PFM_INPUT_ACTIVE_CHANNEL_MASK` (`pfm_input.h`) -- the other 5
  channels aren't wired to anything on this bench, and leaving them
  fully initialized was adding CPU/interrupt load for no benefit while
  the investigation below was open. A timer with no active channel is
  left completely untouched (no clock, no NVIC, no GPIO), not merely
  armed with `M=0`.
- **RESOLVED 2026-09-09: real, DSLogic-confirmed missing PWM pulses on
  V/W during a fast frequency ramp -- an HRTIM SET/RESET-event
  collision, not a `pfm_input.c` bug.** A frequency-ramp test (`python/
  pfm_table_upload.py` `PROFILE 6`/`7`, added this session specifically
  to verify period capture tracks a *changing* signal) found real,
  DSLogic-confirmed missing output pulses -- not a capture artifact --
  on V and W starting as low as ~68-69 kHz, well below where U alone
  had seemed safe. Root cause, found via ST's own community
  engineering forum (search "HRTIM PWM transients greater than a
  period"): when an HRTIM timer's output SET and RESET events fall
  within ~3 HRTIM clocks of each other, RESET always wins and SET is
  silently dropped -- V/W's SET (`TIMPER`, meaning their own
  `MASTER_CMPk` reset-trigger event, which itself shifts every table
  entry) could land within a few clocks of the *previous* cycle's own
  duty compare (`CMP1`) during a fast ramp. **Channel 0 (U) was NOT
  actually immune** -- a first fix (V/W only) confirmed clean on V/W
  but then unmasked the SAME failure on U at the same per-value
  transitions, just rarer. **Fix** (`HRTIM1_FullInit()`, `hrtim.c`):
  every active channel's output SET moved off the un-prioritized
  `TIMPER` onto a dedicated `CMP3` compare (HRTIM favors the
  higher-numbered compare on a collision, so SET now wins instead of
  losing) -- required also updating `HRTIM1_PWM_Start()`'s cold-start
  settling logic, since channel 0's SET source no longer coincides
  with its own reset event. **Confirmed fixed on real hardware, all 6
  channels** (U/UN/V/VN/W/WN), 60-100 kHz ramp, DSLogic: zero anomalies
  anywhere, cold start unaffected. Generalizes automatically to any
  future `HRTIM_NUM_CHANNELS` (1-5, `ctrlr_config.h`) -- every place
  this fix lives is keyed off that constant, not a hardcoded count;
  re-verify on real hardware when raising it, the same way this fix
  itself was verified, not assumed. Also added, same session: a hard
  `PFM_MAX_CARRIER_FREQ_HZ` (100 kHz, `ctrlr_config.h`) ceiling on
  every `TABLE:STEP`, rejected with `ERR 10` -- predates and is
  independent of the SET/RESET fix, kept as a documented, tested
  boundary rather than removed now that the fix holds. Full diagnostic
  history (DSLogic captures, exact failure signatures, dead ends) in
  `docs/changelog.txt`'s 2026-09-09 entries.

## Closed-loop control (added 2026-10-02)

The supply's output voltage can be held to a time-series target (`TARGet:*`)
by reading it back over an ADC input: ADC1 scans six inputs (PA0 PA1 PC0 PB0
PC1 PB1, default feedback PA1) plus VREFINT on a timer trigger into circular
DMA; each DMA block is one control tick (`CONFig:PIDRate`, default 10 kHz).
Feedforward from a plant model plus PID, output through the PFM engine's
live source. HVPS: frequency (duty fixed), one side of the ~19 kHz peak
(`CONFig:CLOSEDloop:BRANCH`); Limiter: duty (carrier fixed). Every voltage is
in the loop's own units (process volts): `CONFig:FEEDback:SCALE/OFFSet` or
`RANGE <Vmax>` (the Limiter's divider puts -Vmax at 0 V and +Vmax at full
scale, zero at mid-scale) convert the ADC reading; `CONFig:MAXVolts` (the
immediate-stop overvoltage limit) is in the same units, so it must be raised
when the range is. Faults: overvoltage, ADC at full scale, and (only when the
offset is non-zero) ADC at code 0 -- a dead feedback line on a mid-scale-zero
range. `PID:LOOPMODE 1 1` makes `FIRE` play the target instead of the open-loop
`SHOT:*`; leave it 0 for open-loop shots. `CONFig:FEEDback:SOURce SIM` runs the
loop against a firmware plant model with nothing wired. Commands:
`docs/command_reference.md`; history: `docs/changelog.txt` 2026-10-02.

## Source layout (standing rule -- keep it this way)

Adopted 2026-09-29 (ported from WHAM-XREX-PFMG474, where it was
established 2026-09-26 at the user's direction) as a **long-term
instruction for anyone, human or agent, working in this repo: every
change must keep this structure.** `python/check_layout.py` enforces the
mechanical parts and runs in every `python/wham_build.py` build, so a
violation fails the build just like a compile error.

```
Core/               CubeMX-generated ONLY: Src/main.c, stm32g4xx_it.c,
                    stm32g4xx_hal_msp.c, system_stm32g4xx.c, syscalls.c,
                    sysmem.c; Inc/main.h, stm32g4xx_hal_conf.h,
                    stm32g4xx_it.h; Startup/. The composition root: main()
                    and the IRQ handlers wire the layers together.
Drivers/            ST HAL + CMSIS (vendor code, untouched)
src/
  config/           ctrlr_config.h -- compile-time configuration; any layer
  drivers/          hardware-independent interfaces, headers only:
                    uart.h, board_io.h, mcu.h, dac.h, adc.h, vref.h, hrtim.h, pfm_input.h,
                    qspi_test.h, boot_jump.h
  bsp/stm32g4/      the only code that touches the MCU (HAL, registers,
                    interrupts): implements drivers/; *_hw.h = BSP-private
  middleware/scpi/  scpi_parser.c -- protocol logic, no hardware, no
                    product knowledge
  app/              the product: app.c (start-up, main loop), task_*.c
                    (main-loop tasks, tasks.h)
    control/        pfm (the table playback engine), shot_profile (SHOT:*),
                    closed loop: pid_core, feedback, target_profile,
                    closed_loop (pure logic, host-tested), control_runtime
                    (the ADC-tick glue); sim/sim_plant (simulated supply)
    protection/     gate_driver (fault evaluation)
    commands/       command_table.c, commands.h, cmd_common.c/.h,
                    cmd_<subsystem>.c
tests/              off-target unit tests, run on the PC (make -C tests)
build/              wham_build.py output: WHAM-HBRIDGE-PFMG474.{elf,bin}
                    (tracked), .map, generated/ (git_version.h) and tests/
                    (ignored)
Debug/              CubeIDE's build folder; makefiles kept in step with the
                    tree by python/sync_build_sources.py
docs/, python/      documentation, host tools
```

Rules:

1. **Dependencies point one way.** app -> middleware -> drivers <- bsp;
   config (and build/generated) from anywhere. `src/app`,
   `src/middleware` and `src/drivers` never include `main.h`, `stm32*.h`
   or a BSP header, and never use HAL calls or types, register access,
   GPIO port/pin names, IRQ numbers or interrupt intrinsics. Need
   hardware from app code? Add to (or create) a `src/drivers` interface
   and implement it in `src/bsp/stm32g4`.
2. **The BSP never includes app or middleware headers.** When a hardware
   event must reach the application (an interrupt), `Core/`'s handler
   calls the app function, or the app registers a callback.
3. **`Core/` stays generated.** Hand-written code there is limited to
   one-line calls inside USER CODE blocks; no new files in `Core/`.
   (Invariants 2 and 3 below are the two deliberate exceptions to
   "one-line": the bootloader jump first in `main()` and USART2's NVIC
   setup.)
4. **A module is a `.c` and its `.h` side by side** in the same folder.
   BSP-private declarations (HAL handles, HAL-valued settings) go in a
   `<module>_hw.h` next to it. Header names are unique project-wide --
   includes are by bare name.
5. **Commands:** the handler goes in the matching
   `src/app/commands/cmd_<subsystem>.c` (file map in `commands.h`), is
   declared in `commands.h`, and gets a row in `command_table.c`; errors
   use `SendErr()` with a `cmd_err_t` code (`cmd_common.h`). A new
   subsystem gets a new `cmd_<subsystem>.c` and a line in the map.
6. **Main-loop work is a task:** `TaskX_Poll()` in `src/app/task_x.c`,
   declared in `tasks.h`, called from `App_Poll()`. Tasks never block.
7. **Hardware-independent logic gets a host test** in `tests/`
   (`make -C tests`, also run by `wham_build.py`). `test_command_table`
   checks every command pattern is reachable by its short and long form
   -- a new command that collides with an existing one fails the build.
8. **After adding, moving or renaming a source file or folder**, run
   `python3 python/sync_build_sources.py` (updates `Debug/`'s makefiles
   and `.cproject` for the IDE); `wham_build.py` refuses to build while
   they are stale.
9. **Images come from `wham_build.py` and live in `build/`**; the
   flashing and update tools read them from there.
10. **If a rule genuinely doesn't fit, change it deliberately**: this
    section and `check_layout.py` together, with a changelog entry.
    Don't work around the checker.

Known limits, left as they are: interface names still carry this chip's
vocabulary (`HRTIM1_*`, `PfmInput_*`, the `uart2` instance).

## Tech stack & layout

- C11, STM32Cube HAL (G4), bare metal -- no RTOS, no heap.
- Built with STM32CubeIDE's generated makefile; toolchain
  arm-none-eabi-gcc 13.3.
- Where everything lives: "Source layout" above. `main.c` is
  CubeMX-generated init plus one call per USER CODE block; start-up and
  the main loop are `src/app/app.c`. Serial commands:
  `src/middleware/scpi/scpi_parser.c` (matching),
  `src/app/commands/command_table.c` (the table), `commands.h` (every
  handler, plus which file holds which subsystem), `cmd_common.c/.h`
  (`cmd_err_t`, `SendErr()`, shared preconditions), one
  `cmd_<subsystem>.c` per subsystem (split out of the old single
  `commands.c`, 2026-09-29; older changelog entries still say
  `commands.c`, `cmd_parser.c` or `Core/Src/...`).
- `python/` -- host-side tooling (see above).
- `tests/` -- off-target unit tests (see "Source layout").
- `docs/` -- this documentation set.

## Build & verify

**Canonical way to build, as of 2026-09-29:**

```bash
python3 python/wham_build.py
```

It runs, in order: (1) `python/gen_git_version.py`, regenerating
`build/generated/git_version.h`; (2) `python/sync_build_sources.py
--check` -- fails if `Debug/`'s makefiles or `.cproject` no longer match
the source tree; (3) `python/check_layout.py` -- the "Source layout"
rules; (4) `make -C tests` -- the off-target unit tests
(`--skip-tests` to skip); (5) `make -j4 all` in `Debug/`; (6)
`arm-none-eabi-objcopy -O binary` to regenerate the `.bin` -- this
project's `.cproject` does **not** have CubeIDE's "Convert to binary file
(.bin)" post-build step enabled, so a bare `make` alone never touches the
`.bin`, and flashing a stale one "succeeds" while silently not containing
your latest changes; (7) publish the `.elf`/`.bin`/`.map` to
`build/WHAM-HBRIDGE-PFMG474.*` -- where the flashing tools read them, and
what git tracks (`Debug/`'s own copies are ignored).

**Adding, moving or renaming a source file or folder from outside
CubeIDE (e.g. an agent writing a `.c`/`.h` pair directly)**: run
`python3 python/sync_build_sources.py`. CubeIDE's managed build does not
refresh its generated per-directory `Debug/**/subdir.mk`,
`Debug/sources.mk`, `Debug/makefile` or `Debug/objects.list` on a plain
`make`; the script regenerates all of them, plus `.cproject`'s include
paths and source folders, from the tree (`--check` only reports). In
CubeIDE itself, `F5` (Refresh) + a normal Build also regenerates them.
Files added under `Drivers/` (vendor HAL sources) still need the
hand-patch. `Debug/objects.list` is not tracked in git, so a fresh clone
must build once from the IDE or run the script before a command-line
`make` links.

- Verification = clean build (zero warnings) + the off-target unit tests
  (SCPI matching, command-table reachability) + real hardware round trips
  over the serial link (see `docs/changelog.txt` for what's actually been
  confirmed on hardware vs. only compiled/linked).
- Serial console: **115200 8N1** (raised from 9600 on 2026-09-04, see
  "Hard-won invariants" below and `docs/changelog.txt`), SCPI-style
  mnemonics (case-insensitive, short/long form), `OK ...` /
  `ERR <code> <msg>` responses. Full protocol: `docs/command_reference.md`.

## Coding conventions

Same conventions as the sibling project (module-prefixed public
functions, `stdint.h` exact-width types, minimal non-blocking ISRs,
`volatile` on ISR-shared state, bugfix comments that state root cause +
symptom + why the fix works) -- there's no separate style doc here yet
since the codebase is still small; read `boot_jump.c` and `cmd_parser.c`
for the current standard to match.

## Hard-won invariants

1. **Baud rate is 115200 everywhere on purpose** (raised from 9600 on
   2026-09-04, real-hardware experiment -- see `docs/changelog.txt`;
   `python/wham_serial_flash.py`, `python/pfm_table_upload.py`, and
   `python/scpi.py` were all updated to match). `main.c`'s
   `huart2.Init.BaudRate = 115200` line lives in CubeMX-generated code
   *outside* any `USER CODE` marker. A `.ioc` "Generate Code" would
   reset it back to HAL's own default -- which, as it happens, IS
   115200 right now, so a regen wouldn't silently break this today.
   That's a coincidence, not a guarantee: if this value ever needs to
   change again, re-apply it explicitly rather than trusting a regen to
   land on the right number by chance. This is a WHAM-PFMG474-V4-only
   divergence -- the sibling PFM-STM32G474 project still uses 9600, and
   nothing here implies porting this change there.
   - **921600 does not work on this hardware** -- tried directly after
     115200 (skipping the standard steps in between) as part of the
     same experiment: consistently garbled, same-length-but-wrong-content
     replies (a real baud mismatch/reliability failure, not a fluke --
     retried 3x). Left unexplored which of 230400/460800 is the actual
     practical ceiling; 115200 was chosen as "clearly safe and already a
     large win," not as "the fastest verified-working rate."
   - **If a bad baud change ever strands the board** (as 921600 did,
     mid-experiment): the serial `BOOT` command needs a working link to
     even request the ROM bootloader, so it can't recover a board stuck
     at a wrong baud. An ST-Link + `st-flash write <bin> 0x08000000`
     (or `st-info --probe` first to confirm it's detected) recovers
     over SWD, completely bypassing the broken serial link -- confirmed
     working for exactly this scenario the same day.
2. **`BootJump_CheckAndEnter()` must stay the literal first statement in
   `main()`**, before `HAL_Init()`. This is what lets the ROM bootloader
   jump-in path (entering, not the `Go` command discussed above) work
   reliably -- see `boot_jump.c`'s header comment.
3. **USART2's NVIC priority/enable call lives inside `MX_USART2_UART_Init()`'s
   `USER CODE BEGIN USART2_Init 2` block**, not in `USER CODE BEGIN 2`
   with everything else -- without it, `HAL_UART_Receive_IT()` arms but
   the interrupt never actually reaches the NVIC, and nothing over
   serial ever gets a reply. Easy to lose track of on a CubeMX regen if
   you're not looking in the right `USER CODE` block.

4. **Every board needs a firm pull-up on the `NRST` line (a resistor to
   +3V3; 5.1 kOhm verified on the WHAM-XREX boards, which use this same
   PCB) -- a hardware requirement, not a firmware setting.** Found
   2026-09-29 on WHAM-XREX-PFMG474 after two days of chasing a ghost. The
   symptom: after `FWUPdate:SWAP`, or after a cold power-on, the board is
   silent (no `!BOOT` banner, no `*IDN?` reply) until an ST-Link is
   connected. The cause: the STM32G474 has NO pull-down on NRST, only a
   weak (~40 kOhm) internal pull-up that the reference manual says is
   disabled during every internal reset; something else on the board holds
   the net partway down (measured 1.6 V, between the ~1.0 V low and ~2.3 V
   high thresholds -- datasheet values from memory, verify), so the chip
   stays in reset. With the resistor fitted: 30 clean operations (swaps, a
   full reflash, a 20-swap soak, a true cold start) on one board and 16 of
   16 swaps on the other. **Before investigating a "silent after
   swap/power-on" report, check NRST with a meter (healthy is near
   3.3 V).** It is NOT the source layout, NOT the option bytes
   `nSWBOOT0`/`IRHEN`, and NOT the update code -- all three were ruled out
   there; don't re-chase them. Do not hard-tie NRST to 3.3 V (it defeats the
   chip's internal-reset holder); use a resistor. The source of the
   pull-down path is still unidentified. Full record: WHAM-XREX-PFMG474's
   `docs/changelog.txt` (2026-09-28/29 entries) and SOP Section 3.3.
   **ST-Link:** with the pull-up fitted, attach with SWDIO, SWCLK, GND and
   3V3 only and leave the ST-Link's NRST wire OFF; use `openocd` with
   `reset_config none` and halt through the debug port. (2026-09-29, on
   the simulator board: with the pull-up lifted and the NRST wire not
   driving, the core could not be halted at all -- "timed out while
   waiting for target halted" -- and a flash attempt died mid-erase.)
5. **Know your board's option bytes.** A floating `BOOT0` pin (`PB8`, only
   a capacitor on some boards) can boot the ROM bootloader instead of the
   application, which is silent to the serial link. Setting `nSWBOOT0 = 0`
   makes the chip ignore the pin. Read them with `DIAGnostic:OPTBytes?`;
   write them only with an ST-Link (`openocd ... stm32l4x option_write 0
   0x20 <value> <mask>`, masked so only the intended bit changes) after
   recording the current values. The WHAM-XREX simulator board runs
   `FLASH_OPTR = 0xBBEFF8AA` (`nSWBOOT0 = 0`, `IRHEN = 0`).
6. **A firmware update needs `STATE IDLE`.** `FWUPdate:BEGin` and `SWAP`
   refuse (`ERR 13`) when ARMED, FIRING or in FAULT. A bench board with
   nothing wired to the gate-drive inputs may boot in `FAULT GENERAL`;
   `FAULT:CLEAR` then `STATE?` -- if it stays in FAULT, the inputs really
   read as faulted (check `GDS?` against `GDS_FAULT_POLARITY`).

## Known gaps / in-flight work (as of 2026-08-31)

- **The VTOR/MEMRMP bugfix (see above) has not been ported to the
  sibling V3/V4 projects yet**, which carry the identical unfixed
  `boot_jump.c` this one was forked from. Deliberately deferred, not
  forgotten -- ask before assuming it should happen automatically.
- **RESOLVED 2026-09-04: `FIRE` now exists.** `HRTIM1_EnableMasterInterrupt()`
  (`hrtim.c`) and `HRTIM1_Master_IRQHandler()` (`stm32g4xx_it.c`) were
  ported from the sibling project (state-machine/fault-pin calls
  stripped, since neither exists here), enabled once at boot from
  `main.c` (after `FixSysTickPriority()`, also newly ported, has raised
  SysTick off the HAL default -- see that function's own doc comment
  for the priority scheme: SysTick=0, HRTIM1_Master=1, USART2=2).
  `cmd_fire()` (`commands.c`) wraps `PFM_Restart()`, rejecting with
  `ERR 5` against an empty table. No ARM/state-machine interlock was
  added -- `FIRE` always takes effect immediately; adding an interlock
  is future work, not implied by this change. See
  `docs/test_protocol.md`'s T3 for the (now largely superseded, but
  still useful for `hrtim.c`-only bring-up) developer-only direct-call
  path this bypassed. **Verified end-to-end on real hardware the same
  day** (flash -> upload a real 500-entry table -> `FIRE` -> `OK`) --
  command-layer round trip only, no scope available in that pass, so
  the actual waveform (T4.5-T4.8) is still unobserved. That same pass
  also caught and fixed an unrelated ~25-50x slowdown in
  `pfm_table_upload.py`'s own reply-reading loop -- see
  `docs/changelog.txt`, not a firmware issue.
- **RESOLVED 2026-09-04: cold-start phase-lock glitch on every `FIRE`,
  fully confirmed on real hardware.** The waveform T4.5-T4.8 left
  unobserved above WAS observed, via DSLogic captures -- and went
  through three iterations before it was actually right (full story in
  `docs/changelog.txt`'s several 2026-09-04 entries; this is the final
  state only). Root cause: `HRTIM1_PWM_Start()`'s force-ACTIVE step
  (needed to avoid an undefined complementary-pair state, per ST's own
  guidance) puts U/V/W's main outputs HIGH at the same instant with no
  regard for their intended 120°/240° stagger, and Timers B/C
  (`ResetTrigger = MASTER_CMP1`/`MASTER_CMP2`) don't become genuinely
  phase-locked until they've received that first reset -- which, unlike
  Timer A's (`MASTER_PER`, coincides with its own natural rollover), an
  external reset does NOT itself regenerate the output's SET state.
  Fixed in `HRTIM1_PWM_Start()`: each phase now waits for and connects
  to its own pins SEPARATELY, at its own reset-trigger event (V at
  `MASTER_CMP1`, W at `MASTER_CMP2`, U at `MASTER_PER`/`MREP`, via the
  new static helper `HRTIM1_WaitForPhaseAndConnect()`), with V/W's
  output re-forced ACTIVE at that exact moment (a direct `SETx1R`/
  `RSTx2R` write, same reasoning as the direct `OENR` write) so their
  first visible pulse is a genuine fresh start, not a snapshot of
  whatever their comparator had been doing since an earlier, still-
  hidden reset. Global interrupts are masked for the whole sequence
  (`HRTIM_MASTER_IT_MREP`/`MCMP1`/`MCMP2`, all armed at boot by
  `HRTIM1_EnableMasterInterrupt()`, would otherwise fire mid-sequence
  and let `PFM_CycleBoundaryHandler()` silently advance the table
  before step 0 was ever visible) -- bounded by a spin-count ceiling,
  not `HAL_GetTick()`, since that can't advance while masked.

  **Confirmed on real hardware**: a ~5 ms DSLogic capture spanning 499
  of profile 3's 500 periods shows phase spacing holding at
  ~3300 ns/~6600-6700 ns (target 3333/6667 ns) consistently from the
  cold-start edge through the last period, and U's duty tracking the
  90% -> 10% ramp cleanly throughout, landing exactly on 10.0% at the
  final period -- the fix holds for the entire shot, not just the
  startup instant.
- **RESOLVED 2026-09-08: channel count is a real, working, compile-time
  setting.** `version.h`'s `PWM_NUM_CHANNELS` was documentation-only
  ("NOT read by hrtim.c today") since it was first added -- generalized
  the whole PWM/HRTIM path to a real `HRTIM_NUM_CHANNELS` (1-5) in a
  new `ctrlr_config.h`, per a direct request to plan this out first
  (see the plan file this was implemented from, and the design
  discussion earlier in that session for the full "why 1-5, why
  compile-time, why not channel F" reasoning). Touched: `PFM_Step_t`
  (fixed `cmpA/cmpB/cmpC` -> `cmp[HRTIM_NUM_CHANNELS]`, sized exactly
  per build, no waste), `PFM_Phase120()/PFM_Phase240()` ->
  `PFM_PhaseForChannel()`, `PFM_Phase_t`/`PFM_SetPhaseEnabled()`/
  `PFM_GetPhaseEnabled()` -> a plain 0..N-1 channel index (durable
  per-channel enable array, same contract as before), every hardcoded
  A/B/C block in `HRTIM1_FullInit()`/`HRTIM1_PWM_Start()`/
  `HRTIM1_PWM_Stop()`/`HRTIM1_SoftwareUpdate()`/`HRTIM1_ApplyPfmStep()`
  -> a loop over 0..N-1 (the per-phase cold-start settling mechanism
  from the entry above it generalized directly, unchanged in mechanism
  -- channel k waits on `MASTER_CMPk`, ascending, channel 0 waits on
  `MASTER_PER`/`MREP` last), the per-channel register-address getters
  -> single indexed getters backed by lookup tables. `TABLE:STEP`'s
  wire format is now `per` + N compare values, not a fixed 4 -- new
  `CONFig:CHANnels?` (`OK <N>`) lets host tooling confirm N instead of
  assuming it; `python/pfm_table_upload.py` queries it at startup and
  refuses to upload on a mismatch.

  A real bug was found and fixed during this work, not just designed
  around: the first attempt stringified `TABLE:STEP`'s expected token
  count via the standard `#x` preprocessor idiom, which only works on
  plain literals -- `1 + HRTIM_NUM_CHANNELS` being an *expression*
  meant the `ERR 4` message printed the literal unevaluated text
  `"(1U + (4U))"` instead of `"5"`. Fixed with a runtime `snprintf()`
  instead of compile-time stringification.

  **Verified on real hardware**: full clean rebuild at the default N=3
  (zero warnings, `.bss` unchanged -- `PFM_Step_t` stays exactly 8
  bytes/entry at N=3, confirming no regression), reflashed, `*IDN?` and
  the new `CONFig:CHANnels?` both correct, a real table uploaded and
  `FIRE`d twice with the board staying responsive throughout. Then
  temporarily rebuilt at N=4 specifically to prove the generalization
  works at a *different* channel count, not just N=3 restated: `.bss`
  grew by exactly the predicted +10,000 B (2 B/entry x 5000 entries),
  `CONFig:CHANnels?` correctly reported 4, a 4-channel-wide table
  uploaded and fired cleanly, and the wrong-width-table rejection
  (`ERR 4`) was confirmed too. Also confirmed the `#error` compile-time
  guard actually fires for an out-of-range N=6. Reverted to the
  project's real N=3 default before the final commit -- N=4/5 are
  proven to work, not shipped as the active setting.

  **Not yet done**: a DSLogic capture confirming N=4/5's actual
  waveform (even spacing at the new angle, correct dead time on the
  added channel) -- everything above verifies the command layer and
  the build/RAM math, not the electrical output, and this project's
  physical probes have only ever been wired to 3 channels. The N=3
  regression pass also has no fresh DSLogic capture of its own in this
  entry (though extensive command-layer verification found nothing
  wrong, and the underlying settling mechanism is provably unchanged
  for channel 0..2 -- see the diff) -- a scope/DSLogic pass confirming
  N=3's waveform is still bit-for-bit the same as the baseline above
  would close this out completely.
- **`HRTIM1` *is* represented in the `.ioc` now** (as of 2026-09-04,
  hand-constructed, not GUI-generated -- see `docs/changelog.txt`).
  Do NOT reopen HRTIM1's Mode/Configuration panel in CubeIDE's Pinout &
  Configuration GUI -- doing so once already triggered a full
  regeneration that silently rewrote the clock tree and produced
  duplicate-symbol link errors against `hrtim.c`'s hand-written
  `HAL_HRTIM_MspInit()`/`hhrtim1`. Other peripherals' pinout remains
  safe to configure through the GUI normally.
- **`_Min_Stack_Size` in `STM32G474QETX_FLASH.ld` is still the CubeMX
  nominal minimum (0x400 = 1 KB)**, not a value chosen for this
  firmware's actual worst-case call depth. Deliberately not changed
  yet (2026-08-31) -- revisit once `PFM_TABLE_SIZE` is actually raised
  from its current default (5000); the two decisions are linked (a
  larger table leaves less RAM headroom for a larger stack reservation
  to mean anything). Don't raise `PFM_TABLE_SIZE` far past 5000 without
  also revisiting this.
- **`HW_BOARD_REV` in `ctrlr_config.h`** (`"REVA"`) **is a placeholder**
  -- not yet confirmed against the actual board silkscreen.

## Documentation map

| Doc | Contents |
|---|---|
| `docs/command_reference.md` | Every serial command, SCPI matching rules, error codes |
| `docs/serial_reflash_guide.md` | How to use `wham_serial_flash.py`, the BOOT mechanism, the VTOR/MEMRMP fix, troubleshooting |
| `docs/changelog.txt` | Running change log, dated entries, root causes for fixes |
| `docs/memory_history.csv` + `docs/memory_report.html` | Flash/RAM footprint, tracked per commit since project start -- see `python/memory_report.py`'s entry above for how/when to regenerate |
| `docs/test_protocol.md` | Numbered bench test protocol (T0-T3) -- markedly shorter than the sibling project's, since it only covers what actually exists here (serial identity, BOOT/reflash incl. the Go-jump regression check, and a developer-only HRTIM smoke test). Update it as functionality is added rather than assuming it tracks the sibling's group numbering. |
| `docs/sop/wham_pfmg474_v4_sop.tex` | Formal Standard Operating Procedure: build, bootstrap flash, serial reflash, verification, troubleshooting. Same LaTeX house style as the sibling project (`mathpazo`, 5in×8.25in `geometry`, plain `\hline` tables) -- compile with `pdflatex` (twice, for cross-references), then render to PNG and visually inspect before calling any edit done; this document's own `\tabcolsep` note explains a real overfull-table pitfall worth reading before adding another table. |
