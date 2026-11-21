# 6. Known hardware issues

Snapshot: 2026-11-16, `main` after `666fd35`. "Board" means the PFM-Controller V4 unless stated. Each item: symptom, what is known,
what to do now, and what the next board revision should change. Full evidence is in the dated changelog entries named.

## 6.1 NRST is not held firmly high: the board stays in reset (CRITICAL)

* **Symptom.** After a firmware swap (`FWUPdate:SWAP`) or a cold power-up, the board is silent: no `!BOOT` banner, no reply to `*IDN?`.
  Plugging in an ST-Link "fixes" it.
* **Cause, as far as known.** The STM32G474 has only a weak internal pull-up on NRST (about 40 kOhm), and the reference manual says it is
  disabled during internal resets. Something on the board pulls the net partway down: it measured **1.6 V**, inside the undefined band
  between logic low and high. So the chip can stay in reset. **The source of the pull-down was never found.**
* **Fix in use.** A **5.1 kOhm resistor from NRST to 3.3 V** (a bench bodge). With it: 30 clean operations on one board (swaps, a full
  reflash, a 20-swap soak, a true cold start) and 16 of 16 swaps on another.
* **Do not** tie NRST hard to 3.3 V (it defeats the chip's own reset hold and was an invalid test), and **never** put a pull-down on NRST.
* **Consequence for the ST-Link.** With the bodge fitted, connect SWDIO, SWCLK, GND and 3V3 only; leave the ST-Link's NRST wire off and use
  `reset_config none`. With the pull-up lifted and NRST undriven, the core could not even be halted.
* **Before chasing any "silent after swap or power-on" report, measure NRST** (healthy is near 3.3 V). It is not the firmware layout, not
  the option bytes `nSWBOOT0`/`IRHEN`, and not the update code: all three were ruled out.
* **Next revision:** a proper NRST pull-up (about 5-10 kOhm) on the PCB, and find what loads the net.
* Record: `firmware/xrex/docs/changelog.txt` 2026-09-28 and 2026-09-29; the unified SOP's "Required hardware modifications" section (`docs/sop/wham_controllers_sop.tex`); the retired XREX SOP's section 3.3, with a next-revision list, is in the git history (`firmware/xrex/docs/sop/README.md`).

## 6.2 BOOT0 is floating

* PB8/BOOT0 has only a capacitor to ground on some boards. A floating BOOT0 booted the ST ROM bootloader instead of the application on the
  former simulator board (now LMTR-CTRL); the board is then silent on the serial link.
* That board's option bytes were changed to `FLASH_OPTR = 0xBBEFF8AA` (`nSWBOOT0 = 0`, `IRHEN = 0`) so the pin is ignored. The XREX
  controller still has the original option bytes (`0xFFFFF8AA` on bank 2, `0xFFEFF8AA` on bank 1); its BOOT0 measured 0 V but is not
  protected. Ask before changing a controller's option bytes.
* **Next revision:** a 10 kOhm pull-down on BOOT0.

## 6.3 PG10 is NRST, not a usable input

The pin map's label for PG10 suggested an input. An emergency-stop input was built on it and failed with reproducible glitches. PG10 is the
MCU's NRST pin, on the same net as the debug connector (confirmed on the schematic, 2026-09-17). Dead end; do not reuse it.

## 6.4 Inputs with no pull resistors read as faults when unwired

* The 12 gate-driver/fault inputs (PE0-PE11) are configured with no pull-up or pull-down. Unwired, they read LOW or float.
* HBRIDGE uses `NORMALLY_HIGH` polarity, so an unwired input is a fault: **a bench board boots in `FAULT GENERAL`** (expected). Switch the
  unwired inputs off (`GDS:ENAble`), or clear their bits in `GDS_DEFAULT_ENABLE_MASK` for a supply with fewer signals.
* XREX bench boards boot in `FAULT OVERCURRENT 1` for the same reason (its overcurrent pins float).
* On the former simulator board, floating overcurrent pins caused random spurious faults (fixed in firmware for that build).
* **Next revision:** defined pull resistors on every fault input, in the direction that makes a broken wire a fault.

## 6.5 Pin conflicts and EXTI limits

The STM32 can route only one port per EXTI line number. XREX overcurrent inputs on PF4, PF5 and PF8 clash with the gate-driver interrupts on
PE4, PE5 and PE8, so XREX polls overcurrent instead of using interrupts (only PF12 was free). Keep this in mind when assigning new
interrupt inputs.

## 6.6 Network serial bridges (WIZnet)

* Accept **one TCP client**. A second connection is refused; this is why the hub exists.
* Fixed at **8N1** with no remote parity control, so the ST ROM bootloader (8E1) cannot be reached over the network: firmware updates use
  the application's own `FWUPdate:*` path.
* **Lose characters in long replies**, at a fixed place and randomly. Long data is read in pages with a checksum (`SUM=`) and retried.
* Round trip about **65 ms**.
* On 2026-11-16 the ECH-HVPS bridge **dropped off the network** (no ping, "No route to host") right after a burst of commands, and came back
  later after the board was re-installed. Cause not found. A frozen MCU would not remove the bridge from the network, so suspect power,
  cabling or the network first.

## 6.7 Fiber links

* 2026-09-18: fiber transmitters on the controller/simulator drive and feedback paths showed visible light inconsistently, changing state
  with no firmware or (reported) wiring change. Partly explained later (the simulator was running stale firmware for part of that time),
  **never fully explained**. If it recurs, change one variable at a time with the cabling physically undisturbed.
* 2026-09-21: the controller's PFM input receivers showed a frozen pin level on all six inputs while the simulator was confirmed transmitting.
  Leading explanation: the drive/feedback fiber pairs were disturbed by earlier loopback rewiring. A fiber patching mix-up also once looked
  like a firmware channel bug (2026-09-15).
* Lesson: when measured feedback reads zero, check the physical fiber path and the far board's `STATE?` before suspecting the controller.

## 6.8 Analog path not proven

On 2026-10-13 every ADC input read 0 with the DAC driven (the reference voltage was fine): the board's DAC-to-ADC test path was not live.
HBRIDGE closed loop has therefore been proven only with firmware plant models and the frequency loopback, **not on real analog inputs**.
Earlier (2026-10-02) the analog reference was found misconfigured and fixed, and a "fault" turned out to be miscabling.

## 6.9 Signals that have never been looked at on a scope

These are written and partly tested but have no electrical verification (see `08_open_items.md`): the HBRIDGE dead time at values other
than 100 ns; the CHARGE output on PG0 (and whether the charger input has a pull-down, since the pin floats until the firmware starts);
the Limiter at 100-125 kHz (the pulse-collision fix was confirmed only up to 100 kHz); 4- and 5-channel HRTIM builds; PC10 hardware fault
trips; a real PF15 edge on HBRIDGE.

## 6.10 Serial speed

**921600 baud fails** on this hardware (garbled replies). 115200 is the tested rate; 230400 and 460800 were never tried. If a wrong baud ever
strands a board, recover over SWD (5.4).

## 6.11 Smaller hardware facts worth knowing

* The QSPI flash must be read with `SampleShifting = NONE`; the commonly recommended half-cycle shift gives every ID shifted by one bit.
* The W25Q128 unique-ID read needs 32 dummy clocks but the QUADSPI dummy field holds at most 31: the driver reads extra bytes and drops them.
* Both boards on the bench use CH340 USB-serial chips with the same IDs; the port name tells you nothing about which board it is.
* `HW_BOARD_REV` is the placeholder `"REVA"`, never checked against the silkscreen.

## 6.12 Next board revision: consolidated list

1. NRST pull-up on the PCB; find and remove whatever loads NRST.
2. BOOT0 pull-down.
3. Pull resistors on all fault inputs (fail-safe direction) and on CHARGE (PG0).
4. Keep PG10 out of any I/O function (it is NRST).
5. Avoid EXTI line clashes between ports for interrupt inputs (6.5).
6. Make the analog test path (DAC to ADC) usable and documented.
7. Confirm `HW_BOARD_REV` against the silkscreen and record it in the firmware.
