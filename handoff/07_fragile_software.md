# 7. Fragile software: handle with care

Snapshot: 2026-11-16, `main` after `666fd35`. Each item: what can break, how you would notice, and what to do instead. The product's
`AGENTS.md` "Hard-won invariants" section is the fuller record.

## 7.1 STM32CubeMX and the generated files

| Trap | What happens | Rule |
|---|---|---|
| Opening **HRTIM1's Mode/Configuration panel** in CubeIDE's Pinout view | Regenerates code, silently rewrites the clock tree, and causes duplicate-symbol link errors against the hand-written `hrtim.c` (it happened). | Never open it. Other peripherals can be configured in the GUI. |
| "Generate Code" | Anything outside `USER CODE BEGIN/END` blocks is overwritten. The USART2 baud-rate line is outside a block (it happens to match HAL's default today). USART2's interrupt enable is inside `USER CODE BEGIN USART2_Init 2`; if it moves or is lost, the board never answers. | After any regeneration, check the baud rate, that the board answers, and `git diff Core/`. |
| The link flag `-u _printf_float` | Without it, `printf("%f")` prints nothing (newlib-nano). Some replies (`PID:GAINS?`) still use `%f`. A CubeIDE regeneration of `Debug/makefile` can drop the hand-added flag. | After regenerating the build files, check that `PID:GAINS?` shows numbers. New code should use the printf-free `Scpi_FormatFloat`. |
| `BootJump_CheckAndEnter()` | Must be the literal first statement of `main()`, before `HAL_Init()`. It also restores `SCB->VTOR` and `SYSCFG->MEMRMP`, without which an image started by the ROM bootloader is deaf to every interrupt. | Never move it. |

## 7.2 The build

* **`wham_build.py` only** for anything that will be flashed (4.2). A bare `make` leaves `build/` stale; it has also run the clean goal.
* **New or moved source files need `python3 python/sync_build_sources.py`.** CubeIDE does not refresh `Debug/**/subdir.mk` on a plain `make`.
  `Debug/objects.list` is not tracked; a stale one once linked a 14 KB image with no startup code (look for "cannot find entry symbol
  Reset_Handler").
* Files added under `Drivers/` (vendor HAL) still need their makefile entries added by hand.
* The image reports `-dirty` if anything in the monorepo is uncommitted.

## 7.3 Timing, interrupts and the HRTIM

* **Interrupt priorities were chosen deliberately, and differ between the products** (for example, USART2 moved from priority 3 to 1 on
  HBRIDGE and to 2 on XREX on 2026-11-09, after an overrun could switch serial receive off). Changing a priority can delay fault handling,
  starve the per-period update, or lose serial input. Read `AGENTS.md` and the 2026-11-09 changelog entries before touching them.
* **The EXTI 15-10 vector is shared** by the HBRIDGE trigger (PF15) and gate-driver fault inputs. Work done there delays fault handling. That is why
  the shot start runs from a pended software interrupt (the unused CORDIC vector).
* **HRTIM output SET must stay on CMP3**, not on the period event. When SET and RESET land within about 3 HRTIM clocks of each other, RESET
  wins and the pulse is lost; this dropped real pulses on fast ramps until 2026-09-09.
* **Cold start of the phases** (`HRTIM1_PWM_Start()`): each phase is connected separately at its own reset event, with interrupts masked and a
  spin-count limit (not `HAL_GetTick()`, which cannot advance while masked). This fixed a phase glitch on every start (2026-09-04).
* **Output-off gaps (HBRIDGE, 2026-11-16):** on entry the outputs are disabled before the off step is written; on exit the step is written before
  the outputs reconnect. Each phase is cut at the end of its own period, in channel order, and the pending Master period flag is cleared. Change
  this order and you get cut pulses or a phase restarting out of step. Read the 2026-11-16 changelog entry first.
* **`PFM_ForceStop()` stops the shared Master counter.** XREX's fault ramp-down needs that counter running; it uses `PFM_ForceStopSoft()`. For
  months the "ramp-down" was really a freeze because of this (found 2026-09-15). Do not swap these calls.
* **CPU budget.** HBRIDGE with three closed loops at 125 kHz: the control tick peaks at about 80 % of its time slot and the per-period interrupt
  uses about 31 % of the CPU. `PID:DIAG?` reports the cost. Adding work to either needs measuring.
* **Dead-time register.** `CONFig:DEADtime` rewrites the HRTIM dead-time registers in place, so it is refused while `FIRING`. The gap logic's
  margin follows the dead time (`HRTIM1_GapEnter`).

## 7.4 State machine and faults

* **Only `SetState()` may change the state.** It also drives the LEDs; assigning the state variable directly makes the LEDs disagree with `STATE?`.
* `FAULT:CLEar` must call the fault-latch clears and then re-check the inputs. It once did neither, so a real fault could never be cleared.
* `EnterFault()` / the ramp-down / the main-loop fault poll run under masked interrupts to avoid a race that could turn a ramp into a cut-off.
* XREX: `ArmConditionsMet()` started as a stub that always allowed `ARM`. Later work added the ENA_OUT/CONTACT_OUT check. Check the current code
  and the 2026-09-23 changelog entry before assuming what it checks.
* **Simulator build (XREX):** runs the full controller fault stack. Four functions in `xrex_io.c` are compiled out for the simulator because they
  read pins that mean something else there. `XrexIo_EnableOutputsReadyToArm()` was not guarded. Any new fault path must get the same guard, or
  the simulator will fault itself randomly.

## 7.5 Saved profiles

* **Field ids and error codes are frozen.** A reused id silently loads the wrong value from an old profile.
* `SET_PRESENT_BITS` (HBRIDGE, 160 bits) must be at least fields x instances; a compile-time check enforces it.
* The HBRIDGE payload is near its limit (worst case 15,704 of 16,336 bytes): a big new table will not fit without a design change.
* Tables are streamed to and from flash because free RAM is about 14 KB. Do not "simplify" this into a RAM buffer.
* XREX and HBRIDGE use separate flash regions and magics; never move a region.
* A recall applies values through the normal setters, in a fixed order (supply mode first, because tables are checked against it).

## 7.6 Shared code between the products

`scpi_parser`, `scpi_util`, `scpi_err.h`, `nvstore`, `qspi_test`, `profile_store`, `device_identity`, `led_indicators`, and their tests are
byte-identical in both firmwares. Change both, run both products' `make -C tests`, then `python3 python/check_shared.py`. **Known exception:**
`uart.c`/`uart.h` differ since 2026-11-09 (XREX gained a refused-line capture); reconcile or record it as intentional.

## 7.7 Host software

* The hub has **no authentication**. Anyone who can reach its port can `ARM` and `FIRE`. Use `allow_from` or a loopback listen address where possible.
* The hub's dangerous-command list does not flag `*RCL`, but a recall can now switch phases on, change the GDS polarity and switch inputs off.
* `test_wham_hub.SlowClientTests` fail on macOS (pre-existing).
* The headless DSLogic capture tool (kept outside the repo, `~/dslogic-tool`) ignores its trigger setting: every capture is free-running.

## 7.8 Smaller traps

* `TABLE:STEP`'s error message once printed `"(1U + (4U))"` because the preprocessor stringified an expression; use `snprintf` for computed values.
* A `/*` inside a block comment breaks the build under `-Werror`.
* `_Min_Stack_Size` is the CubeMX default (1 KB), never sized for the real call depth.
* `PFM_TABLE_SIZE` and the stack share the RAM budget: revisit both together.
