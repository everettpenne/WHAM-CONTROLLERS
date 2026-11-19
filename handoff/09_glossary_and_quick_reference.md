# 9. Glossary and quick reference

Snapshot: 2026-11-16, `main` after `666fd35`. The command lists here are the everyday subset; the full set with every argument and error is
in `firmware/<product>/docs/command_reference.md`.

## 9.1 Glossary (embedded terms, for an electrical engineer)

| Term | Meaning here |
|---|---|
| **MCU** | Microcontroller: the STM32G474 (CPU, flash, RAM and peripherals on one chip). |
| **Firmware / image** | The program in the MCU's flash. An image is the built `.bin`/`.elf` file in `build/`. |
| **Flash bank, BFB2** | The 512 KB flash is two 256 KB banks. The option bit BFB2 picks which bank boots. Updates write the idle bank and flip BFB2. |
| **Option bytes** | Non-volatile configuration bits of the MCU (boot bank, how BOOT0 is used). Read with `DIAGnostic:OPTBytes?`, change only with an ST-Link. |
| **ROM bootloader** | ST's built-in loader in system memory, entered by BOOT0 or the `BOOT` command. Talks 8E1, so not over our network bridges. |
| **SWD / ST-Link** | The two-wire debug interface and the USB probe that uses it: programs flash and halts the CPU, independent of the serial link. |
| **NRST** | The MCU's reset pin. Low = held in reset. See 6.1. |
| **HAL** | ST's Hardware Abstraction Layer library (`Drivers/`). |
| **CubeMX / CubeIDE** | ST's configuration code generator and its Eclipse-based IDE. Generates `Core/` and the `.ioc` file. |
| **BSP** | Board support package: our layer that touches the hardware (`src/bsp/stm32g4/`). |
| **Interrupt / ISR, priority** | A hardware event that pauses the main program to run a short handler (interrupt service routine). Lower priority number = more urgent; a more urgent one can interrupt a less urgent one. |
| **NVIC, EXTI** | The interrupt controller, and the external-interrupt unit that turns GPIO edges into interrupts (one port per line number). |
| **DMA** | A hardware engine that moves data (ADC samples, capture timestamps) into RAM without the CPU. |
| **`volatile`** | A C keyword that forces every read/write of a variable to really happen; needed for data shared with interrupts. |
| **HRTIM** | High-resolution timer: the peripheral that generates the drive signals. Master timer + units A-F, 170 MHz, compare registers, dead time, fault inputs. |
| **Compare (CMPx), period (PER)** | Counter values at which an output changes state, and the count at which the counter restarts (sets the switching frequency). |
| **Shadow register / update** | A buffered copy of a timer setting that takes effect at a defined moment (the roll-over), so a change never lands mid-cycle. |
| **Dead time** | A short gap inserted between one switch of a leg turning off and its complement turning on, to avoid shoot-through. |
| **PFM / PWM** | Pulse-frequency / pulse-width modulation. |
| **Carrier** | The switching frequency. |
| **Shot** | One timed output sequence (a pulse of the supply), from start to its programmed end. |
| **Setpoint, target** | HBRIDGE: a shot point (time, frequency, duty) for open loop; a target point (time, voltage) for closed loop. |
| **Open loop / closed loop** | Output follows the programmed table / output is corrected from measured feedback by a PID controller. |
| **PID; Kp, Ki, Kd** | Proportional-integral-derivative controller and its gains. |
| **Feedforward / plant model** | The expected output-to-response relation, used to start from a good guess before PID correction. |
| **Slew limit** | The largest change allowed per control tick. |
| **GDS** | Gate-driver status: the 12 fault inputs on HBRIDGE (PE0-PE11). |
| **Polarity, NORMALLY_HIGH** | Which pin level is healthy. NORMALLY_HIGH: HIGH = healthy, LOW = fault. |
| **Interlock** | An input that must be satisfied before output is allowed (XREX PF13 external enable). |
| **Latch** | A fault that stays set until explicitly cleared, even if the input recovers. |
| **SCPI** | A text command style from test instruments: `SUBSYSTEM:COMMAND args`, queries end in `?`, long/short forms (`SHOT:STARt` = `SHOT:STAR`). |
| **Hub** | `host/wham_hub.py`, the per-board network service that shares one board among users. |
| **WIZnet bridge** | The serial-to-Ethernet module on each board's UART. |
| **Profile, slot** | A saved set of settings in external flash, one of 8 numbered places. |
| **Host test** | A unit test of firmware logic compiled and run on the PC, with fakes for the hardware. |
| **`-dirty`** | Suffix on the version when the build tree had uncommitted changes. |
| **Changelog status tag** | The bracket at the end of a changelog title saying what was verified: host-tested, flashed, confirmed on hardware. |
| **Transrex ISR-2126** | The magnet power supplies XREX controls. |
| **ECH** | Electron cyclotron heating; the ECH-HVPS is the gyrotron's high-voltage supply. |
| **Enerpro** | The SCR firing board in a Transrex supply; one of its fault signals. |

## 9.2 Commands both products share

| Command | Purpose |
|---|---|
| `*IDN?` | Product, board revision, firmware version, commit, `SN=`, `UID=`. **Always first.** |
| `SYS:ID?` | Full identity as `key=value`. |
| `STATE?`, `FAULT?`, `FAULT:CLEar` | State; fault latched?; clear (re-checks inputs). |
| `ARM`, `DISARM` | Ready for one shot; stand down. **ARM needs the owner's OK.** |
| `SOURce:STOP` | Stop the output, back to IDLE. |
| `CONFig:DEADtime <ns>` / `?` | Dead time for all phases, 50-3000 ns, default 100 (rounded to 5.88 ns steps). |
| `EXTernal:TRIGger <0\|1>` / `?` / `:INPut?` | PF15 trigger on/off (HBRIDGE default on); raw pin level. |
| `*SAV <slot> [label]`, `*RCL <slot>` | Save / recall a profile (IDLE only). |
| `CONFig:PROFile:CATalog?`, `CONFig:PROFile? <slot>`, `CONFig:PROFile:DIFF? <slot>`, `CONFig:PROFile:CLEar <slot>` | List, preview, compare, erase profiles. |
| `FWUPdate:STATus?` | Running bank and update state (updates are done by `fw_update.py`). |
| `DIAGnostic:RSTCause?`, `DIAGnostic:OPTBytes?`, `DIAGnostic:LEDs?`, `DIAGnostic:UART?` | Diagnostics. |
| `PFMIN:CAPTURE <n>`, `PFMIN:STATus?`, `PFMIN:DATA? <ch> <start> <count>` | Capture periods on the PFM inputs during the next shot. |
| `LOG:ARM`, `LOG:DATA?` | Record a run's waveform (forms differ by product). |
| `GDS?` | Raw levels of the 12 PE0-PE11 inputs. |

## 9.3 HBRIDGE (Limiter, ECH-HVPS)

| Command | Purpose |
|---|---|
| `CONFig:MODE LIMiter\|HVPS` / `?`, `CONFig:LIMits?` | Supply type (IDLE only, discards the shot); the limits in force. |
| `PHASe:ENAble <phases> <0\|1>`, `PHASe:SELect <phases>`, `PHASe:ENAble?` | Which of U, V, W drive (IDLE only). |
| `SHOT:BEGin`, `SHOT:STEP <t_us> <hz> <duty>` (or `<dU> <dV> <dW>`), `SHOT:END`, `SHOT?`, `SHOT:POINt? <i>`, `SHOT:CLEar` | Program and read the open-loop shot. `0` Hz = output off. |
| `FIRE` | Play the shot (needs ARMED; **owner's OK**). |
| `PID:LOOPMODE <loop 0-3> <0\|1>`, `PID:GAINS <loop> <kp> <ki> <kd>` | Open/closed per phase; gains. |
| `TARGet:BEGin/STEP/END` | Closed-loop voltage target. |
| `CONFig:FEEDback:*`, `CONFig:CLOSEDloop:*`, `CONFig:MAXVolts`, `CONFig:PIDRate`, `CONFig:SLEWRate` | Commissioning settings (not for operators). |
| `GDS:ENAble <0-12> <0\|1>` / `?`, `GDS:NICKname <1-12> <name>` / `?`, `GDS:POLarity <NORMALLY_HIGH\|NORMALLY_LOW\|INVert>` / `?`, `GDS:LAST?` | Fault-input configuration and which inputs tripped last. |
| `CHARGE:FREQuency <hz>` / `?`, `CHARGE:STATus?` | Charger square wave on PG0 (0 = off). Stays on through faults. |
| `EXTernal:TRIGger:STATus?` | Trigger edge counts and last result. |
| `DEBUG:FAULT:BYPASS <0\|1>` | Bench only: ignore PC10 and the GDS inputs. Dangerous. |
| `GENERAL:TEST:FAULT` | Inject a fault (testing). |

## 9.4 XREX

| Command | Purpose |
|---|---|
| `CONFig:CHANnels?` | Channel count (4). |
| `SOURce:ENAble <ch> <0\|1>` / `?`, `CHANnel:NICKname <ch> <name>` | Enable a channel; label it. |
| `SHOT:TIMing <rampUpS> <flatTopS> <rampDownS>` / `?`, `SHOT:CURRent <ch> <amps>` / `?`, `SHOT:STARt` | Define and start a shot (needs ARMED; **owner's OK**). |
| `SOURce:SETpoint <ch> <hz>`, `SOURce:RUN`, `SOURce:STATus? <ch>` | Continuous run to a frequency setpoint. |
| `PID:GAINS <ch> <kp> <ki> <kd>`, `PID:LOOPMODE <ch> <0\|1>` | Gains; open or closed loop (0 = all channels). |
| `EXTernal:ENAble <0\|1>`, `EXTernal:INPut?` | PF13 interlock on/off; its raw level. |
| `XREX:CHANnel:STATus? <ch>`, `XREX:CHANnel:ENAOut`, `XREX:CHANnel:CONTactOut` | Per-channel fault inputs; enable and contactor outputs to the Transrex. |
| `CONFig:MAXCURRent <ch> <A>`, `CONFig:TURNONHz`, `CONFig:MAXFREQHz`, `CONFig:SLEWRate`, `CONFig:FaultRampTime`, `CONFig:FaultPolarity:*` | Calibration and protection settings (commissioning). |
| `OCP:TEST:FAULT <ch>`, `GENERAL:TEST:FAULT` | Inject faults (testing). |
| `SYS:EVENT <0\|1>`, `SYS:EVLOG?`, `SYS:TELEM?` | Event stream (off at boot), event history, telemetry. |
| `SIM:*` | Simulator build only. |

## 9.5 Reply and error codes worth memorising

Replies are `OK ...` or `ERR <code> <text>`. The full list is `docs/scpi_conventions.md`. Common: `ERR 1` unknown command; `ERR 4` table
format or order; `ERR 5` empty table; `ERR 6` a fault is latched; `ERR 10` outside the supply mode's limits; `ERR 12` bad or out-of-range
argument (including "not a number"); `ERR 13` not allowed in this state; `ERR 14` invalid nickname. Codes are never renumbered.

## 9.6 Host tools

| Tool | Use |
|---|---|
| `python3 python/wham_build.py` (in a product folder) | Build, test, publish the image to `build/`. |
| `python3 python/fw_update.py --url socket://localhost:<port> [--status\|--rollback] --yes` | Firmware update through the hub. |
| `python3 python/net_client.py --port <port> --name <you>` | Interactive terminal through the hub. |
| `python3 python/pfm_shot_test.py`, `pfm_gap_test.py` | Fire a shot and compare captured outputs with the table (they FIRE). |
| `python3 python/memory_report.py history\|report` | Flash/RAM tracking. |
| `python3 python/check_shared.py` | Shared files identical in both products. |
| `python3 python/sync_build_sources.py` | After adding or moving source files. |
| `python3 host/provision_board.py --url ... --check` | Compare a board's identity with `host/devices.csv`. |
| `host/operator_ui/start_wham_ui.*` | The operator web console. |
