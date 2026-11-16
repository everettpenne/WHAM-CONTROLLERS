# 3. Settled design decisions

Snapshot: 2026-11-16, `main` after `666fd35`.

These were decided on purpose, mostly by the project owner, often after a real problem. Each says what was decided and why. If
one now looks wrong, raise it with the owner; do not quietly reverse it. Dates are when it was decided (details in the changelogs).

## 3.1 Repository and process

| Decision | Why |
|---|---|
| **One repository for everything** (`WHAM-CONTROLLERS`, 2026-10-08): both firmwares, hardware, host software, docs. The old per-product repos are frozen, not deleted. | Shared code, a shared host service and cross-product docs were growing. |
| Each product stays a **self-contained CubeIDE project** with its own history, docs and build. Shared code is **copied byte-for-byte** and checked by `check_shared.py`, not linked from a common folder. | Each product must build on its own; a shared folder would tie their release cycles. |
| **Layered source layout** enforced by `check_layout.py` (2026-09-26). `Core/` holds only CubeMX output. | The owner wants a professional, hand-over-ready codebase; layering also makes the logic host-testable. |
| **Changelog and memory footprint updated with every change**, without being asked. | Standing instruction: the changelog is the project's real design history. |
| **AGPL-3.0-only for the software** (2026-10-08). Docs and hardware not licensed yet. | Owner's choice after discussing that no open-source licence forbids selling. |
| **No AI attribution lines in commit messages.** Commit and push only when asked. | Owner's instruction. |
| The repository is **public**. | Owner's choice. Keep secrets and personal details out. |

## 3.2 Both products

| Decision | Why |
|---|---|
| **115200 baud** everywhere. | Tested working; 921600 failed on this hardware (garbled replies). Raised from 9600 on 2026-09-04. |
| **Strict command arguments**: a number must be the whole token; `12x`, `1.5` for an integer, `abc`, `nan` are refused (`ERR 12`), never truncated. | A mistyped setpoint must not become a different valid setpoint on a power supply. |
| **Error codes and saved-profile field ids are frozen.** Never renumber or reuse. | Host software, LabVIEW front ends and old saved profiles depend on them. |
| **Every shot needs its own `ARM`.** Faults latch until `FAULT:CLEar`, which re-checks the inputs. | Deliberate two-step start; a fault must be acknowledged. |
| **Profiles are never loaded at boot**, only by `*RCL`. | A reset must always come up in a known default state. |
| **Firmware update over the serial link, dual bank, needs `IDLE`.** | Boards must be updatable remotely without risk of bricking; the old image stays for rollback. The network bridge is fixed at 8N1, so the ST ROM bootloader (8E1) cannot be used over the network. |
| **Identity is reported, never enforced.** Nothing in the firmware depends on the serial number. | A blank or corrupted identity must never stop a controller from working. |
| **One hub per board, no write leases, every client sees every command, dangerous commands announced.** No authentication (yet). | Several operators may share a board; in-person safety rules apply. Lease and authentication were discussed and deferred. |
| **Front-panel LEDs are cosmetic, removable, never blocking** (`LED_INDICATORS_ENABLED`). | They must never affect timing or safety. |

## 3.3 XREX

| Decision | Why |
|---|---|
| **Fixed-rate PID** on a Master-timer heartbeat (default 1 kHz), not self-clocked per channel. | A fixed control sample time keeps the PID math valid whatever each channel's carrier is. Read the design notes before changing. |
| **Each channel's HRTIM unit updates on its own roll-over** (`ResetTrigger=NONE`, `UpdateTrigger=NONE`, `ResetUpdate=ENABLED`). | Fully independent channel frequencies. |
| **Output floor 3 kHz.** | HRTIM 16-bit period register limit at this clock. |
| **Amps-to-hertz conversion isolated in one function** (`AmpsToHz()` in `pid.c`), currently linear. | Real Transrex behavior may be nonlinear; calibration must touch only one place. |
| **Fault response: the faulted channel stops, the others ramp down** (General: ramp to turn-on frequency over `CONFig:FaultRampTime`; overcurrent: survivors step down by `100/N` %, bypassing the slew clamp, then ramp). | A magnet supply should be ramped, not cut, wherever possible. |
| **External enable (PF13) and trigger (PF15) are separate pins** (2026-09-17). The trigger no longer needs the enable switched on, but every fire re-checks the enable interlock. | Two independent physical signals; safety kept by the re-check. |
| **`ARM` requires ENA_OUT and CONTACT_OUT high for every enabled channel**, re-checked continuously while armed. | Do not arm a channel whose supply is not enabled and contacted. |
| **Fault polarities settable per category** (`CONFig:FaultPolarity:WATER|TEMP|ENERPRO|OCP`). | Real Transrex signal polarities were not known when written. |
| **Duty-cycle measurement on PFM inputs dropped**; period only. | Both edge-capture designs read one period late for an unexplained reason; not chased further. |
| **Emergency stop on PG10 abandoned**: PG10 is the MCU's NRST pin. | Hardware fact, not a firmware choice. |

## 3.4 HBRIDGE

| Decision | Why |
|---|---|
| **One fault type (`GENERAL`), immediate force-stop**, no ramp-down. | An H-bridge gate-driver fault must stop switching at once. |
| **GDS polarity `NORMALLY_HIGH`** (a LOW pin is a fault; an unwired input is therefore a fault) since 2026-11-13. Runtime flip for all 12 at once with `GDS:POLarity` (2026-11-16). No per-pin polarity. | Fail-safe for a broken wire. Per-pin polarity was offered and not requested. |
| **GDS inputs can be switched off individually** (`GDS:ENAble`), only in `IDLE`/`FAULT`; switching on is allowed in any state and re-checks at once. | Supplies with fewer than 12 wired signals, and bench boards. |
| **Supply mode must be chosen before a shot can be uploaded** (boot mode `NONE`); a mode change discards the shot. | Prevents playing a shot checked against the other supply's limits. |
| **Shot interpolation is linear in time, per switching period**; a 0 Hz point is an output-off gap (first point cannot be off, last two must be on). | Matches how operators describe shots; gaps added 2026-11-16 at the owner's request. |
| **Phases keep fixed positions (U 0, V 1/3, W 2/3 of a period) whichever are enabled.** | Disabling V must not move W. |
| **Dead time: one value for every phase, runtime, default 100 ns, 50-3000 ns, not while firing.** Saved in profiles since 2026-11-16. | Owner's request; 50 ns floor chosen to avoid shoot-through margins; the 3000 ns top is the register limit. |
| **Phase choice is saved in profiles** (2026-11-16). Before that it was deliberately left out so a recall could never change which legs are driven. | Owner's request. A recall can now switch legs on; check `PHASe:ENAble?` after `*RCL`. |
| **External trigger is ON at every boot** (`SHOT_TRIGGER_DEFAULT_ENABLED`), handled by interrupt, not saved in profiles. | Matches XREX behavior; the start runs in a lower-priority software interrupt so fault handling is never delayed. |
| **CHARGE output stays on through faults**, off at boot, not in profiles; a non-zero frequency is "dangerous" in the hub. | Owner's decision for the capacitor-bank charger. |
| **The XREX PF13 external-enable interlock is NOT ported to HBRIDGE** (yet), nor XREX's per-channel fault types or PID ramps. | Not requested; listed in open items. |
| **HBRIDGE logs (`LOG:ARM`) record closed-loop runs only.** | That is what the logger was built for; open-loop shots are checked with the PFM input capture instead. |
