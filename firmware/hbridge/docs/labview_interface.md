# WHAM-HBRIDGE-PFMG474 — LabVIEW Interface Command Reference

This document is the serial-command contract a LabVIEW programmer needs to build a front-end for the
**WHAM-HBRIDGE-PFMG474**, the controller for the three-phase H-bridge power supplies (the **Limiter**, driven by PWM, and
the **HVPS**, driven by PFM). It is the HBRIDGE counterpart of `firmware/xrex/docs/labview_interface.md`, and like that document it
lists what to send and what comes back. It covers:

1. how the link works (read this first),
2. the concepts the commands are built on (supply mode, phases, open and closed loop),
3. configuration commands,
4. the open-loop shot (frequency and duty over time, per phase),
5. the closed loop (per-phase voltage targets),
6. arming, starting and stopping,
7. status, fault and health polling,
8. logging a run,
9. the analog inputs and outputs,
10. stored profiles,
11. the error codes,
12. worked end-to-end sequences,
13. an appendix of limits.

**Companion documents.** Everything about *connecting* to the hub, framing and the failure modes of the link is in
`docs/labview_hub_guide.md`; the operating states, fault behaviour and recovery are in `docs/labview_state_and_faults.md`; both are
at the top of the repository and apply to this controller unchanged. The command-by-command detail is in this folder's
`command_reference.md`. This document is the product-specific contract that ties them together, so a programmer can build the
HBRIDGE front-end from it, using the others for depth.

All commands and replies are plain ASCII text. Everything here is the *current* wire protocol (firmware v0.9 and later).
Phase/loop numbers are **1-based**: **1 = phase U, 2 = phase V, 3 = phase W**, and `0` means "all three" where a command says so.

---

## 1. Serial link and framing (must-read)

### 1.1 Through the hub (the normal way)

Do not open a serial port. Connect over TCP to the hub for this controller and send `@quiet <your-program-name>` as the first line
(`docs/labview_hub_guide.md`, sections 1-3). The hub owns the serial link to the controller, shares it with other people, and logs
everything. The rest of this section describes what flows over that TCP connection.

| Parameter | Value |
|---|---|
| Transport | TCP to the hub (the hub's host and port come from the lab; one port per controller) |
| First line | `@quiet <name>` (CR LF), no reply |
| Line terminator, both directions | `\r\n` (the hub also accepts a lone `\r` or `\n` from you, but always send CR LF) |
| Encoding | ASCII |
| Longest command | **127 characters**; longer text is silently cut and the cut text is run |
| Locale | format numbers with an explicit period as the decimal point; never a comma |

If your program ever talks to the controller directly instead (a bench, no hub), the serial settings are 115200 baud, 8 data bits, no
parity, 1 stop bit, no flow control; the framing below is identical. Only one client can use a serial line, so this is for a stand-alone
bench only.

### 1.2 Framing invariants

Every command is one line. Every reply is one line ending in CR LF, with the exceptions in section 7.4. There are no length prefixes,
no binary framing. Read whole lines (TCP Read in CRLF mode with *bytes to read* of at least 65536; see the hub guide).

**Three kinds of line come back, classified by the first character:**

```
OK ...             a command succeeded (optional data after "OK ")
ERR <code> <msg>   a command failed (<code> is a stable integer, see section 11)
ERR hub ...        the HUB could not do the command (timeout, controller restarted, link lost): see the hub guide
!BOOT ...          UNSOLICITED, three lines after every controller reset
# ...              the hub answering one of YOUR "!hub ..." commands (never a controller reply)
```

Unlike XREX, the HBRIDGE has **no `!EVT` telemetry stream**. The only unsolicited lines are the three `!BOOT` lines after a reset. All state
changes must be found by polling `STATE?` (section 7).

**One reply per command, in order.** The hub sends your commands to the controller one at a time in the order you sent them and
returns each reply to you, so a first-in first-out queue of "commands waiting for a reply" is all the matching you need.

**Mnemonic matching.** Commands are SCPI-style and case-insensitive; each level (`CONFig:CLOSEDloop:FREQuency`) has a short form (the
leading capitals). To keep LabVIEW code unambiguous, **always send the full form exactly as written in this document**.

### 1.3 After a reset (`!BOOT`)

A reset prints three `!BOOT` lines. The first looks like:

```
!BOOT WHAM-HBRIDGE-PFMG474 v0.9 <build> BANK=1 BFB2=0 STATE=IDLE tick=0 SN=<serial> UID=<chip id>
```

After any `!BOOT`: assume **every setting you made is gone** (supply mode `NONE`, all phases enabled, loops open, gains and tables at their
defaults) unless it was stored in a profile (section 10); re-identify the controller; read `STATE=` (normally `IDLE`, `FAULT` if a fault was
present at power-up); and tell the operator. Details: `docs/labview_state_and_faults.md`, section 3.

---

## 2. Concepts

**Two supplies, one controller: the supply mode.** `CONFig:MODE` selects the Limiter or the HVPS, and every frequency and duty you upload
is checked against that mode's limits.

| Mode | Drive | Carrier frequency | Duty (per phase) |
|---|---|---|---|
| `LIMITER` (`LIM`) | PWM: the duty varies | 75 – 125 kHz | 5 – 95 % |
| `HVPS` | PFM: the frequency varies | 10 – 30 kHz | 40 – 60 % |

The mode starts as `NONE` at every reset. Nothing can be uploaded until a mode is chosen.

**Three phases: U, V, W.** Each phase is a main output and its complement (with dead time between them). The three share **one carrier
frequency and one time base** (they are phase-locked to one master timer, spaced one third of a period apart), but each phase can have
**its own duty**, and can be enabled or disabled independently.

**Open loop and closed loop, per phase.**
- *Open loop*: the phase plays the uploaded shot, a time series of carrier frequency and duty. No feedback.
- *Closed loop*: the phase has its own controller, which measures that phase's output on an analog input and adjusts its actuator
  (Limiter: duty; HVPS: frequency) to follow a **voltage target** series (`TARGet:*`).
- A run can **mix** them: closed phases are driven by their controllers, open phases by the shot. Default feedback inputs: **U on analog input
  PA0, V on PA1, W on PB0.**
- The **HVPS has one loop only** (loop 1): its actuator, the frequency, is shared by every phase, so loops 2 and 3 apply to the Limiter only.

**State machine.** Every shot follows `IDLE` → `ARMED` → `FIRING` → `IDLE`; any fault goes to `FAULT`, latched until cleared. See
`docs/labview_state_and_faults.md`. The HBRIDGE has **one** fault type (`FAULT GENERAL`) and an immediate stop (no ramp-down).

**Process volts.** Closed-loop voltages (targets, measured values, `CONFig:MAXVolts`) are in "process volts", a unit defined by the feedback
calibration: ADC volts by default, real supply volts once `CONFig:FEEDback:RANGE` (or `SCALE`/`OFFSet`) has been set. All voltages in
one run must be in the same unit.

---

## 3. Identity, system and configuration

### 3.1 Identity and system (read-only for a front-end)

| Command | Reply | Meaning |
|---|---|---|
| `*IDN?` | `OK WHAM-HBRIDGE-PFMG474 REVA v0.9 <git hash> SN=<serial> UID=<chip id>` | board and firmware identity; **check the product name on every connect**. `SN=UNSET` until a serial is assigned |
| `SYS:ID?` | `OK serial=<s> uid=<id> flashuid=<id> product=<p> hw=<r> fw=<v> commit=<c> label=<l> verified=<0\|1> identity=<ok\|unset\|corrupt\|nochip>` | parse by `key=value` |
| `CONFig:CHANnels?` | `OK 3` | number of phases (channels) in this build |
| `DIAGnostic:RSTCause?` | `OK CSR=... BOR=0 PIN=0 SFT=0 IWDG=0 WWDG=0 LPWR=0 OBL=0` | why the controller last reset |
| `DIAGnostic:LEDs?` | `OK TX=0 RX=0 IDLE=1 ARMED=0 FIRING=0 FAULT=0` | what the front-panel LEDs show (cosmetic; the state LEDs always match `STATE?`) |

Do **not** send `SYS:SERial`, `SYS:LABel` (writes) or `SYS:ID:CLEar` from a front-end: they write the board's permanent identity.

### 3.2 Supply mode, limits and phases (IDLE only)

| Command | Reply | Notes |
|---|---|---|
| `CONFig:MODE LIMiter\|HVPS` | `OK` | `ERR 13` unless `IDLE`. **A change of mode discards the uploaded shot**; re-selecting the current mode changes nothing |
| `CONFig:MODE?` | `OK NONE\|LIMITER\|HVPS` | |
| `CONFig:LIMits?` | `OK <mode> FREQ <min> <max> DUTY <min> <max>` or `OK NONE` | the limits in force, in hertz and percent |
| `PHASe:ENAble <phases> <0\|1>` | `OK` | `ERR 13` unless `IDLE`. `<phases>` is letters in any order (`U`, `UW`), channel numbers (`1`, `13`, `1,3`) or `ALL`; the others are left as they are. **A disabled phase's outputs are never connected to its pins when a shot starts**, so its gate drivers see no switching |
| `PHASe:SELect <phases\|NONE>` | `OK` | exactly these phases on, the rest off |
| `PHASe:ENAble?` / `PHASe:ENAble? <phase>` | `OK U=1 V=0 W=1` / `OK <0\|1>` | |
| `PHASe:SELect?` | `OK UW` (or `OK NONE`) | |

All three phases are enabled at every boot, and the choice is **not stored in profiles** (a recall must never change which legs of the
bridge are driven). `ARM` refuses while no phase is enabled.

---

## 4. Open-loop shot: frequency and duty over time

A shot is a time series of **setpoints**. At each setpoint you give a time, a carrier frequency and a duty; the controller ramps frequency
and duty **linearly in time** between setpoints, one switching period at a time. Up to **500 setpoints**, and a shot may be up to
**1 second** long. Everything is checked against the mode's limits at upload.

| Command | Reply | Notes |
|---|---|---|
| `SHOT:BEGin` | `OK` | discards the previous shot and starts an upload. `ERR 13` if no supply mode is chosen or the controller is `FIRING` |
| `SHOT:STEP <t_us> <freq_hz> <duty_pct>` | `OK` | one setpoint, the same duty on all three phases |
| `SHOT:STEP <t_us> <freq_hz> <dU> <dV> <dW>` | `OK` | one setpoint with **a duty per phase**; time and frequency stay shared. The two forms can be mixed from one setpoint to the next |
| `SHOT:END` | `OK <setpoints> <duration_us>` | closes the upload. At least two setpoints are required |
| `SHOT:CLEar` | `OK` | drops the shot |
| `SHOT?` | `OK <setpoints> <duration_us> <PROFILE\|TABLE>` | what `FIRE` will play (`TABLE` if a bench `TABle:*` upload came last) |
| `SHOT:POINt? <i>` | `OK <t_us> <freq_hz> <duty>` (three duties if the phases differ) | setpoint *i*, counting from 0 |

**Units on the wire:** time is an **integer number of microseconds**; frequency is in **hertz** with up to 3 decimals (`20000.5`); duty is in
**percent** with up to 2 decimals (`47.25`).

**Rules:**
- the **first** setpoint is at `t = 0`; times strictly increase; the last setpoint's time is the shot length (at most 1 s);
- the shot ends at the first period that starts at or after the last setpoint's time (it overshoots by less than one period), and the last
  setpoint's values are approached, not held;
- limits are checked at upload, **per setpoint and per phase**: `ERR 10` names the limit (and the phase, for a per-phase duty); `ERR 4` is
  an ordering or format error or a time beyond the longest shot; `ERR 3` is a full upload; `ERR 13` is no mode, or `FIRING`;
- a straight line between two setpoints inside the limits stays inside them.

**Upload one `SHOT:STEP` per line and read the reply before sending the next**, so the first refused step is caught where it happens.

The bench commands `TABle:BEGin`, `TABle:STEP`, `TABle:END`, `TABle?` upload a per-period table directly (at most 100 entries, played once).
They are a bench tool; use `SHOT:*` for real work.

---

## 5. Closed loop: voltage targets per phase

### 5.1 Switching a phase to closed loop, and gains

| Command | Reply | Notes |
|---|---|---|
| `PID:LOOPMODE <loop> <0\|1>` | `OK` | `1` closes that phase's loop; `0` (the default) leaves it open. Loop 0 = all three |
| `PID:LOOPMODE? <loop>` | `OK <0\|1>` | `PID:LOOPMODE? 0` returns all three: `OK 1 0 0` |
| `PID:GAINS <loop> <kp> <ki> <kd>` | `OK` | up to 3 decimals. Units: **hertz per volt** (HVPS), **percent duty per volt** (Limiter) |
| `PID:GAINS? <loop>` | `OK <kp> <ki> <kd>` | |

### 5.2 Target series

A target is a time series of voltages, uploaded like a shot: linear between setpoints, the first at `t = 0`, up to 500 setpoints.

| Command | Reply | Notes |
|---|---|---|
| `TARGet:BEGin` | `OK` | start an upload |
| `TARGet:STEP <t_us> <volts>` | `OK` | one voltage for every phase |
| `TARGet:STEP <t_us> <vU> <vV> <vW>` | `OK` | one voltage per phase, on a shared time grid |
| `TARGet:END` | `OK <setpoints> <duration_us>` | |
| `TARGet:CLEar` | `OK` | |
| `TARGet?` | `OK <setpoints> <duration_us>` | |
| `TARGet:POINt? <i>` | `OK <t_us> <volts>` (or three voltages) | |

A step beyond `CONFig:MAXVolts` is refused (naming the phase). An HVPS target may not be negative (HVPS "voltage" stands in for power).

### 5.3 Holding a constant value

| Command | Reply | Notes |
|---|---|---|
| `SOURce:SETpoint <loop> <volts>` | `OK` | the constant target used by `SOURce:RUN` (below); loop 0 = all |
| `SOURce:SETpoint? <loop>` | `OK <volts>` | |

### 5.4 Settings that belong to commissioning (set once, store in a profile)

These are not operator settings; a front-end normally only reads them. See `command_reference.md` ("Closed loop") before writing any of them.

| Group | Commands |
|---|---|
| Feedback input and calibration | `CONFig:FEEDback:CHANnel`, `SCALE`, `OFFSet`, `RANGE`, `FILTer`, `CLIPTime`, `SOURce ADC\|SIM\|DAC\|LOOP`, `DACRatio`. `CHANnel`, `SCALE`, `OFFSet` and `RANGE` take an optional loop first |
| Plant model | `CONFig:CLOSEDloop:DUTY` (HVPS duty), `FREQuency` (Limiter carrier), `RESonance`, `VPEAK`, `VSCALE`, `BRANCH ABOVE\|BELOW`, and the HVPS curve `CONFig:CLOSEDloop:CURVe:BEGin\|STEP\|END\|CLEar` |
| Loop rate, limits | `CONFig:PIDRate` (control rate, default 10 kHz), `CONFig:SLEWRate`, `CONFig:MAXVolts` (the overvoltage limit, default 4 process volts), `CONFig:ADC:AVERage`, `CONFig:ADC:SAMPle` |
| Simulated plant | `SIM:PLANT:*`, `CONFig:FEEDback:SOURce SIM`: runs the loops against the firmware's own model when nothing is wired. Bench use only |

**Protection.** The closed loop stops the run and raises `FAULT GENERAL` on: overvoltage (the unfiltered measurement beyond
`CONFig:MAXVolts`); the ADC at full scale for `CLIPTime`; the ADC at the bottom of its range for `CLIPTime` (a dead feedback line);
and, in `SOURce LOOP` mode, loss of the loopback signal. A closed-loop start is also refused if the analog reference is more than 3 % from
its configured value.

---

## 6. Arming, starting and stopping

| Command | Reply | Effect |
|---|---|---|
| `ARM` | `OK` | `IDLE` → `ARMED`. Nothing electrical happens. `ERR 6` if a fault is latched; `ERR 13` if not `IDLE` or no phase is enabled. (`ARM` does not check the supply mode: with no mode there is simply no shot to play, and `FIRE` answers `ERR 5`) |
| `DISARM` | `OK` | `ARMED` → `IDLE`; harmless in any other state |
| `FIRE` | `OK` | starts the run from `ARMED`. Open-loop phases play the uploaded shot; closed-loop phases run their controllers. Ends by itself; state returns to `IDLE` |
| `SOURce:RUN` | `OK` | closed-loop run holding each loop's `SOURce:SETpoint`; every enabled phase must be closed loop. Needs `ARMED`. Ends on `SOURce:STOP` or a fault |
| `SOURce:STOP` | `OK` | stops the run, state `IDLE` |

**`FIRE` is checked in this order:** `ERR 6` (fault latched), `ERR 13` (not `ARMED`), `ERR 5` (nothing uploaded to play, or an open phase with no
shot). `SOURce:RUN` with an open phase is `ERR 13`.

**How a run is composed.**
- Every loop open: `FIRE` plays the open-loop shot as uploaded.
- Some loops closed: the closed phases follow their targets; the open phases follow the shot's duty; the carrier is the **shot's frequency** while
  any enabled phase is open, otherwise the fixed closed-loop carrier (`CONFig:CLOSEDloop:FREQuency`).
- The run lasts as long as the shot when any enabled phase follows it, otherwise as long as the target. A closed phase whose target ends earlier
  holds its last value.
- A disabled phase takes no part: its loop does not run and its feedback input is not checked.

There is no stop-in-the-middle for a shot except `SOURce:STOP` or a fault. Shots are at most 1 s long (open loop), so "waiting for the end" is a short poll.

### 6.1 The external trigger: a start without a command

A **rising edge on PF15 (the "Fiber_Enable" input) while the state is `ARMED` starts the shot exactly as `FIRE` does**: the same checks, open or closed loop alike. It is handled
by interrupts (within a fraction of a millisecond of the edge) and is **on at every boot**.

| Command | Reply | Meaning |
|---|---|---|
| `EXTernal:TRIGger <0\|1>` | `OK` | turn the trigger off or on for the session (`ERR 12` for any other argument). Not stored in a profile |
| `EXTernal:TRIGger?` | `OK <0\|1>` | |
| `EXTernal:TRIGger:INPut?` | `OK <0\|1>` | the raw level of PF15 right now |
| `EXTernal:TRIGger:STATus?` | `OK enabled=1 input=0 edges=0 started=0 ignored_disabled=0 ignored_state=0 ignored_glitch=0 refused=0 busy=0 last=NONE` | counters since boot, and the result of the latest attempt (`NONE`, `STARTED`, `FAULT`, `NOT_ARMED`, `NO_SHOT`, `CONTROL:<n>`, `BUSY`, `CHANGED`). Parse by key |

What a front-end must allow for:
- **The state can go `ARMED` to `FIRING` with no command from your program.** There is no `!EVT` stream, so you find out by polling `STATE?`. Do not assume that an `ARMED` controller stays armed.
- **Edge-triggered, once per arm.** A level already HIGH at `ARM` is not an edge. After a refused start (no shot uploaded, a latched fault, a closed-loop refusal) the controller stays `ARMED` and needs a new edge.
- **A trigger edge that arrives while your `FIRE` is being processed is ignored** (your `FIRE` starts the one shot). Your `FIRE` may then answer `ERR 13 Not ARMED` if the trigger got there first.
- After a shot, `EXTernal:TRIGger:STATus?` tells whether a trigger started it (`started` went up and `last=STARTED`) and, when a trigger did *not* start one, why (`last=NO_SHOT`, ...).
- Only an `EXTernal:TRIGger 0` stops a live trigger input from being able to start a shot once armed. The hub treats enabling it as a dangerous command.

---

## 7. Status, fault and health polling

### 7.1 `STATE?`

```
OK IDLE
OK ARMED
OK FIRING
OK FAULT GENERAL
```

Poll it to drive the state indicator and the fault lamp (a few times per second; faster during a run is fine for a 1 s shot, 10 per second is plenty).
Use `FAULT?` (`OK 0` / `OK 1`) for a cheap yes/no.

### 7.2 Live loop status

| Command | Reply | Meaning |
|---|---|---|
| `SOURce:STATus? <loop>` | `OK <running> <target> <measured> <output>` | that loop's running flag, target and measured voltage, and actuator (an open phase shows its shot duty as the output) |
| `PID:LOOPMODE? 0` | `OK <m1> <m2> <m3>` | loop modes |
| `ADC:INPut? <1-4>` | `OK <volts> <code>` | the analog inputs, for a feedback signal check |

### 7.3 Faults

A fault is latched until cleared. `FAULT:CLEar` replies `OK` whether or not it worked, so **always follow it with `STATE?`**: only `OK IDLE`
means it cleared. Reading the cause:

| Command | Reply | Meaning |
|---|---|---|
| `FAULT:SOURces?` | `OK PC10=0 GATEDRV=0 TABLE=0 TEST=0 OVP=0 FB=0` | which source(s) hold the fault: the native hardware fault pin, a gate-driver status input, a table underrun, a test fault, overvoltage, feedback |
| `PID:FAULT?` | `OK <loop 0-3> <NONE\|OVERVOLTAGE\|CLIPPED\|CLIPPED_LOW>` | which loop (phase) stopped the last closed-loop run, and why |
| `GDS?` | `OK 01=LOW 02=HIGH ... 12=LOW` | live raw levels of the 12 gate-driver status inputs |
| `GDS:LAST?` | `OK <pins> <checks>` | the raw 12 pin levels (hex, bit 0 = input 01) at the moment the last gate-driver fault latched, and a count of fault checks. Not cleared by `FAULT:CLEar` |
| `DIAGnostic:RSTCause?` | `OK CSR=... BOR=... ` | the last reset cause |

The raw readings are `HIGH`/`LOW`, not "faulted/healthy"; the build's polarity (currently: HIGH is a fault) decides. There is no map from input number to
phase in the firmware. `GENERAL:TEST:FAULT` injects a test fault (testing only).

### 7.4 Replies that are not one short line

| Command | Reply |
|---|---|
| `LOG:DATA? <loop> [<start> [<count>]]` | one line of up to 40 samples, with a check sum (section 8) |
| `PFMIN:DATA?`, `PFMIN:RAW?`, `PFM:GAPLOG?` | one very long line (bench diagnostics) |
| `CONFig:PROFile:CATalog?` | one line, or an `OK` line and an `ERR` line if a slot cannot be read |

Use a read timeout of 40 s or more for these (the hub gives them 30 s).

---

## 8. Logging a run

```
LOG:ARM <loop> <maxSamples> <decim>     loop 0 = all three; maxSamples <= 1000 for one loop, <= 333 for all three; decim 1..10000
... ARM, FIRE ...
LOG:DATA? <loop> <start> <count>        count <= 40 per reply
```

- One sample is taken every `decim`-th control tick (the default control rate is 10 kHz, `CONFig:PIDRate?`).
- `LOG:DATA?` answers `OK <count> <rateHz> SUM=<s> <sp1> <pv1> <out1> <sp2> ...`: `count` is the **total** number logged, `rateHz` the sample rate, then up to 40
  samples of three integers each: target and measured value in **millivolts**, and the actuator in **milli-hertz** (HVPS) or **milli-percent** (Limiter).
  `SUM` is the sum of all the numbers in the page modulo 2^32. **Always recompute it and compare**: a damaged reply is retried, not used.
- To read it all: loop `start` from 0 in steps of 40 until `start >= count`.
- `ERR 12 That loop was not logged` if `LOG:ARM` armed a different loop. Arm logging in `IDLE` or `ARMED`; read after the run ends.

---

## 9. Analog inputs and outputs

| Command | Reply | Notes |
|---|---|---|
| `ADC:INPut? <1-4>` | `OK <volts> <12-bit code>` | analog inputs 1-4 are PA0, PA1, PC0, PB0. An unconnected input reads near 0 V |
| `ADC:VREF?` | `OK <volts> <code> <factory code>` | the measured analog reference. It is the full scale of both the ADC and the DAC, set by the firmware build (2.9 V in the current build, **not** 3.3 V) |
| `ADC:STATus?` | `OK <running> <tickHz> <scansPerTick> <sampleX10> <scanUs> <blocks>` | sampler diagnostic |
| `DAC:OUTPut <1\|2> <volts>` | `OK` | the two analog outputs (0 to the full scale, up to 3 decimals). **They are independent of the state machine, faults and `FIRE`**: they can be on while the controller is `IDLE` or `FAULT` |
| `DAC:OUTPut? <1\|2>` | `OK <volts> <code commanded> <code in the DAC>` | |

The DAC outputs are real outputs: do not set one except for a reason.

---

## 10. Stored profiles

Eight numbered slots (1-8) in the controller's external flash keep a snapshot of the settings **and** the shot, target and HVPS-curve tables.
**Nothing is applied at boot: recall explicitly after every reset.** Saving and recalling work in `IDLE` only.

| Command | Reply |
|---|---|
| `*SAV <slot> [label]` | `OK` (label 1-15 characters, no spaces) |
| `*RCL <slot>` | `OK APPLIED=<n> FAILED=<k> [<names refused>] [IGNORED=<m>]`. **Check `FAILED` is 0** |
| `CONFig:PROFile:CATalog?` | `OK 1=<label>/<fw> 2=empty ...` |
| `CONFig:PROFile? <slot>` | the saved values, nothing applied |
| `CONFig:PROFile:DIFF? <slot>` | what a recall would change (`name=live>saved`), or `OK SAME`. Show this before a recall |
| `CONFig:PROFile:CLEar <slot>` | `OK` |

Not stored: the run state, the DAC outputs, the simulation settings, and the phase choice. A save or recall takes a fraction of a second, during which
the controller does not answer: allow for it in your timeout. `ERR 18` is an empty or corrupt slot or a flash failure. Details: `docs/saved_profiles.md`.

---

## 11. Error codes

Replies are `ERR <code> <msg>`. Codes are **stable**; parse the integer and map it (the full table is in `docs/scpi_conventions.md`).

| Code | Meaning |
|---|---|
| 1 | Unknown command / mnemonic |
| 2 | `*:STEP`/`*:END` without its `*:BEGin` |
| 3 | An upload table is full |
| 4 | An uploaded step is malformed (value count, ordering) or a time is beyond the longest shot |
| 5 | **Nothing to play** (`FIRE`, closed-loop start) |
| 6 | **Fault latched**: show it, then `FAULT:CLEar` and check `STATE?` |
| 7 | External flash failed |
| 10 | **A frequency or duty is outside the supply mode's limits** (the text names the limit and the phase) |
| 11 | Invalid loop/phase number (1-3, or 0 where accepted) |
| 12 | Wrong argument count, not a number, or out of range |
| 13 | **Not allowed in the current state** (`ARM` when not `IDLE`, `FIRE` when not `ARMED`, a change while `FIRING`, no mode selected when uploading, no phase enabled) |
| 17 | Firmware update failure (not a front-end concern) |
| 18 | Stored profile: empty or corrupt slot, or a flash failure |
| 19 | Identity write refused (not a front-end concern) |
| `ERR hub ...` | the hub, not the controller (`docs/labview_hub_guide.md`, section 7) |

The front-end should show `ERR 6`, `ERR 10` and `ERR 13` as distinct operator messages, with the controller's own text.

---

## 12. Worked end-to-end sequences

Every line is one command; **read and check the reply to each before sending the next**. Lines marked "read back" are queries to display what the controller
actually holds.

### 12.1 Limiter, open loop, a different duty per phase

```
*IDN?                              -> OK WHAM-HBRIDGE-...      (confirm the product)
STATE?                             -> OK IDLE                  (anything else: stop and look)
CONFig:MODE LIMiter                -> OK
PHASe:SELect UVW                   -> OK
SHOT:BEGin                         -> OK
SHOT:STEP 0 100000 20 50 80        -> OK      t = 0, 100 kHz, duty U=20 V=50 W=80 %
SHOT:STEP 10000 100000 80 50 20    -> OK      at 10 ms the duties of U and W have swapped
SHOT:END                           -> OK 2 10000
CONFig:LIMits?  SHOT?  PHASe:ENAble?    (read back and display)
ARM                                -> OK
STATE?                             -> OK ARMED
FIRE                               -> OK
   ... STATE? every 50-100 ms until OK IDLE (the shot is 10 ms long) ...
```

### 12.2 HVPS, open loop

```
CONFig:MODE HVPS                   -> OK
SHOT:BEGin                         -> OK
SHOT:STEP 0 20000 40               -> OK      20 kHz, 40 % duty (all phases)
SHOT:STEP 10000 15000 60           -> OK      at 10 ms: 15 kHz, 60 %
SHOT:END                           -> OK 2 10000
ARM -> OK ; FIRE -> OK ; poll STATE? until OK IDLE
```

### 12.3 Limiter, closed loop on all three phases, then read the log

```
CONFig:MODE LIMiter                -> OK
PID:LOOPMODE 0 1                   -> OK      all three loops closed
PID:GAINS 0 0.167 6.667 0          -> OK      (gains per loop 1..3 or 0 = all; illustrative values)
TARGet:BEGin                       -> OK
TARGet:STEP 0 0                    -> OK
TARGet:STEP 25000 720 -300 500     -> OK      at 25 ms: U = 720, V = -300, W = 500 process volts
TARGet:END                         -> OK 2 25000
LOG:ARM 0 300 1                    -> OK      log every tick, all three loops, 300 samples
PID:LOOPMODE?  PID:GAINS? 1 ...    (read back and display)
ARM -> OK ; FIRE -> OK ; poll STATE? until OK IDLE
PID:FAULT?                         -> OK 0 NONE   (anything else: show which loop and why)
LOG:DATA? 1 0 40                   -> OK 300 10000 SUM=... ...   page 1 of loop 1
LOG:DATA? 1 40 40 ... until start >= 300, then the same for loops 2 and 3
```

(For a closed loop to follow the target on a real supply, the feedback calibration must be set first: see section 5.4. The numbers above are illustrative.)

### 12.4 Mixed: phase U closed loop, phases V and W open loop

```
PID:LOOPMODE 1 1                   -> OK      U closed
PID:LOOPMODE 2 0                   -> OK      V open
PID:LOOPMODE 3 0                   -> OK      W open
TARGet:BEGin / TARGet:STEP 0 300 / TARGet:STEP 10000 300 / TARGet:END
SHOT:BEGin / SHOT:STEP 0 100000 50 30 70 / SHOT:STEP 10000 108000 50 30 70 / SHOT:END
ARM ; FIRE
```
U follows the 300-volt target; V and W play their duties (30 % and 70 %) from the shot, and the carrier is the shot's frequency (100 to 108 kHz) because
two phases are open.

### 12.5 If anything goes wrong

If any reply is `ERR 6`, or `STATE?` returns `FAULT`: stop the sequence, read the cause (section 7.3), clear it only when a person has decided to, then `ARM`
again. After an `ERR hub` reply or a `!BOOT` line: re-identify, read `STATE?`, and re-send nothing until the operator confirms. See
`docs/labview_state_and_faults.md`.

---

## Appendix A: key numeric limits (this build)

| Constant | Value |
|---|---|
| Phases (channels) | 3 (U, V, W) |
| Limiter carrier / duty | 75 – 125 kHz / 5 – 95 % |
| HVPS carrier / duty | 10 – 30 kHz / 40 – 60 % |
| Shot / target setpoints | 500 each |
| Longest open-loop shot | 1 s (1,000,000 µs) |
| Control rate (default) | 10 kHz (one tick per analog scan block) |
| Log samples | 1000 for one loop, 333 for all three; 40 per `LOG:DATA?` page |
| Overvoltage limit (default) | 4 process volts (`CONFig:MAXVolts`; up to 10000) |
| Analog reference | `ADC:VREF?`; set by the build (2.9 V now) |
| Bench table entries | 100 |
| Stored profile slots | 8 |
| Command line length | 127 characters |

**What has and has not been verified.** The commands in this document were verified on the simulator board: per-phase open-loop duties (measured on the
output edges), three simultaneous closed loops against the firmware's own plant models, a mixed run, loop-attributed faults, and stored profiles in real
flash. **Not verified: closed loops on real analog feedback** (the board's DAC-to-ADC path read zero when tried), and carriers between 100 and 125 kHz on a logic
analyzer. Do not assume closed-loop behaviour on real hardware from these results.
