# LabVIEW guide, part 2: operating state, faults and recovery

Part 1 (`docs/labview_hub_guide.md`) explains how to connect to the hub and exchange commands. This part explains what the
controllers **do**: the operating states, what makes them change, what a fault is and how a front-end must react to one. Part 3
(`docs/labview_commands.md`) lists the command sets. Read this part before writing any code that arms or fires a controller.

Both products (XREX, the Transrex magnet-supply controller, and HBRIDGE, the H-bridge limiter/HVPS controller) share the same
state machine and the same command names for it (`ARM`, `DISARM`, `STATE?`, `FAULT?`, `FAULT:CLEar`). Where they differ, the text
says which product. You learn which product you are talking to from `*IDN?` (part 1, section 6).

All examples show what is sent (`>`) and what comes back (`<`). Every sent line ends in CR LF, as in part 1.

---

## 1. The four operating states

```
          ARM                      start a shot / run
  IDLE ----------> ARMED -----------------------------> FIRING
   ^   <---------- (DISARM)                                |
   |                                                       | shot finishes, or a stop command
   +-------------------------------------------------------+
   |
   +---- FAULT:CLEar (only if the cause is gone) ---- FAULT <---- any fault, from IDLE, ARMED or FIRING
```

| State | Meaning | Output |
|---|---|---|
| `IDLE` | Normal resting state: after boot, after a shot completes, after a fault is cleared | Off. Nothing electrical is happening |
| `ARMED` | The operator has said "ready to fire" and the controller has checked its preconditions. Nothing electrical changes on entry | Off |
| `FIRING` | A shot or run is in progress | On |
| `FAULT` | A fault was detected and latched. The output has been (or is being) made safe | Off, or ramping down (XREX, section 4) |

Rules that never change:

- **Every shot needs its own `ARM`.** When a shot ends, the state returns to `IDLE`, not `ARMED`.
- **`ARMED` is entered only from `IDLE`**, and left by starting a shot, by `DISARM`, or by a fault.
- **Leaving `FAULT` is only by `FAULT:CLEar`, and always lands in `IDLE`** (never straight to `ARMED` or `FIRING`). A fault never clears
  itself.
- **A fault can appear in any state, at any time**, from the hardware, from a protection check, or because of something another
  person did. Your program must be ready for it in every state, not just while firing.
- **`ARM` itself does nothing electrical.** The dangerous step is the one that starts output (`SHOT:STARt`, `SOURce:RUN`, `FIRE`).

### Which commands are allowed in which state

| | `IDLE` | `ARMED` | `FIRING` | `FAULT` |
|---|---|---|---|---|
| `ARM` | yes | `ERR 13` | `ERR 13` | `ERR 13` (HBRIDGE: `ERR 6`) |
| `DISARM` | `OK` (no effect) | **yes**, to `IDLE` | `OK` (no effect) | `OK` (no effect) |
| start a shot or run (`SHOT:STARt`, `SOURce:RUN`, `FIRE`) | `ERR 13` ("must ARM first") | **yes**, to `FIRING` | `ERR 13` | `ERR 13` (or `ERR 6` for the legacy `FIRE`) |
| `SOURce:STOP` | `OK` | `OK`, to `IDLE` | **yes**, to `IDLE` | `OK` |
| `FAULT:CLEar` | `OK` | `OK` | `OK` | **yes**, if the cause is gone |
| change settings (gains, limits, timing, targets, tables, mode, phases) | yes | **product dependent**, see below | mostly refused (`ERR 13` or `ERR 12`) | yes |
| save or recall a stored profile (`*SAV`, `*RCL`, `CONFig:PROFile:CLEar`) | yes | `ERR 13` | `ERR 13` | `ERR 13` |
| firmware update, `BOOT` | yes | `ERR 13` | `ERR 13` | allowed (`BOOT`) |
| any query (`...?`) | yes | yes | yes | yes |

The simple, safe rule for a front-end: **do all configuration in `IDLE`, only query in `ARMED` and `FIRING`, and treat a refused
write (`ERR 13`) as "the state is not what I thought", not as a reason to retry.** XREX allows some settings to change in other
states (for example `SOURce:ENAble` takes effect live, even mid-shot, and `XREX:CHANnel:ENAOut` can be changed at any time), so do not
treat those as freely safe: see part 3.

`DISARM` is deliberately harmless from any state, so a stop button may always send `DISARM` and `SOURce:STOP` without first checking the state.

---

## 2. Reading the state

### 2.1 `STATE?`

The authoritative answer. One line:

```
> STATE?
< OK IDLE
< OK ARMED
< OK FIRING
< OK FAULT GENERAL
< OK FAULT OVERCURRENT 2
< OK FAULT EXTERNAL_ENABLE
< OK FAULT ENABLE_OUTPUT 3
< OK FAULT ENERPRO 1
```

Parse it as: the word after `OK` is the state. If it is `FAULT`, the next word is the fault type, and (only for the per-channel
types `OVERCURRENT`, `ENABLE_OUTPUT`, `ENERPRO`) a 1-based channel number follows. HBRIDGE reports only `FAULT GENERAL`.
Unknown fault types may be added in future: treat any word after `FAULT` you do not recognise as "unknown fault, controller is
safe, operator must investigate", never as an error in your own parser.

### 2.2 `FAULT?`

`OK 0` (no fault) or `OK 1` (a fault is latched). Cheaper than `STATE?` and exactly equivalent to "is the state `FAULT`, or is a source
still latched". Use `STATE?` for the display and `FAULT?` where you only need a yes/no.

### 2.3 Events (XREX only)

XREX also **pushes** state changes as unsolicited lines, so a display does not have to poll fast (part 1, section 4, lines starting `!`):

```
!EVT <tick_ms> STATE <IDLE|ARMED|FIRING|FAULT>
!EVT <tick_ms> FAULT <GENERAL|OVERCURRENT|EXTERNAL_ENABLE|ENABLE_OUTPUT|ENERPRO> [<ch>]
!EVT <tick_ms> FAULT CLEAR
!EVT <tick_ms> TRIGGER FIRING        a shot was started by the external trigger input, not by a command
```

`<tick_ms>` is the controller's own millisecond clock since boot (the same clock `SYS:TIME?` reports); it restarts from zero at every
reset. The stream is **not buffered** across a lost connection: after any reconnect, read the real state with `STATE?`, do not
assume you saw everything. `SYS:EVLOG?` replays the controller's retained history since boot, oldest first (RAM only, lost on reset), which is the
way to find out what happened while you were disconnected. HBRIDGE has **no** `!EVT` stream: poll `STATE?`.

### 2.4 Recommended polling

| Situation | What to poll | How often |
|---|---|---|
| Idle display | `STATE?` | 1-2 per second |
| Shot in progress | `STATE?` (and the `SOURce:STATus?` of the channels you display) | up to 5-10 per second; faster polling adds nothing and just fills the hub's queue (part 1, section 5.2) |
| Waiting for a shot to finish | `STATE?` until it returns `OK IDLE` | every 100-250 ms, with a timeout a little over the shot's length |
| Anything else with events available (XREX) | rely on `!EVT`, plus one `STATE?` every few seconds as a cross-check | |

Always poll `STATE?` right after connecting, after reconnecting, after every `!BOOT` line, and after any `ERR hub` reply, before you
decide what to do next.

---

## 3. Booting and reset

Every controller reset prints an unsolicited **three-line banner**, each line starting `!BOOT` (part 1 treats it like any `!` line).
The first line looks like:

```
!BOOT <product> <firmware> <build> BANK=1 BFB2=0 STATE=IDLE tick=0 [SN=... UID=...]
```

What your program must do when it sees `!BOOT`:

1. Treat every command still waiting for a reply as lost (the hub already answers those with `ERR hub the controller restarted ...`).
2. **Assume the controller has forgotten everything you configured.** Gains, limits, channel enables, shot timing, uploaded shot
   and target tables, supply mode, phase selection, nicknames: **none of these survive a reset** unless they are in a *stored
   profile* (`*SAV`/`*RCL`, part 3, section 6), and a profile is only applied when you explicitly recall it (nothing is applied at boot).
3. Read `STATE=` on the first banner line: normally `IDLE`, or `FAULT` if a fault was already present at power-up (a bench board with
   nothing wired to its protection inputs does this routinely).
4. Re-identify (`*IDN?`, `SYS:ID?`) and compare the serial with the one you expect.
5. Show the operator that a reset happened. **Do not silently re-send the old configuration and carry on.** A reset is a fault-like
   event: output stopped, and the reason may need a human (power dip, firmware update, watchdog, crash). `DIAGnostic:RSTCause?`
   tells what caused it.

A planned reset (firmware update, `BOOT`) is the same from the program's point of view, except that you expected it.

---

## 4. Faults

### 4.1 What a fault is

A fault is a **latched** safety condition. When one occurs the controller:

1. makes the output safe (see section 4.3 for how, per product);
2. enters state `FAULT`;
3. **stays there until `FAULT:CLEar` succeeds**, even if the physical cause has long gone.

While latched, `ARM` and the start commands are refused. Faults are detected by hardware inputs (a native hardware fault pin that
acts in silicon without the CPU, plus twelve status inputs from the gate drivers or supplies), by software checks, and (HBRIDGE) by
closed-loop protection. They can also be injected on purpose for testing (`GENERAL:TEST:FAULT`, and on XREX `OCP:TEST:FAULT`): a test
fault is a real fault as far as everything else is concerned.

### 4.2 Fault types

**XREX** reports five types in `STATE?`:

| `STATE?` says | Channel | Cause |
|---|---|---|
| `FAULT GENERAL` | none | the native hardware fault input, or a Water or Temperature fault on any enabled channel |
| `FAULT OVERCURRENT <ch>` | 1-4 | that channel's overcurrent input |
| `FAULT ENERPRO <ch>` | 1-4 | that channel's Enerpro fault input |
| `FAULT EXTERNAL_ENABLE` | none | the external-enable interlock input dropped while firing |
| `FAULT ENABLE_OUTPUT <ch>` | 1-4 | that channel's enable or contactor output (the one this controller drives) was no longer HIGH while armed or firing |

Only **enabled** channels (`SOURce:ENAble <ch> 1`) count: a disabled channel's own fault inputs are ignored and never block anything.

**HBRIDGE** has one type, `FAULT GENERAL`, whatever the cause. To see which source caused it use:

```
> FAULT:SOURces?
< OK PC10=0 GATEDRV=1 TABLE=0 TEST=0 OVP=0 FB=0
```

| Field | 1 means |
|---|---|
| `PC10` | the native hardware fault input tripped |
| `GATEDRV` | one of the twelve gate-driver status inputs reads as a fault (these are **not** masked for disabled phases) |
| `TABLE` | the shot's period table ran empty before the shot ended (an internal overrun; the shot was stopped) |
| `TEST` | a test fault from `GENERAL:TEST:FAULT` |
| `OVP` | closed loop: the measured voltage went beyond the allowed limit (`CONFig:MAXVolts`) |
| `FB` | closed loop: a feedback problem (the ADC sat at the top, or at the bottom of its range, for too long, or the loopback signal stopped) |

When a closed-loop run was stopped by its own protection, `PID:FAULT?` says **which loop (phase)** and why:

```
> PID:FAULT?
< OK 2 CLIPPED_LOW       phase V's feedback sat at the bottom of its range: a dead input line, or beyond -Vmax
< OK 0 NONE
```
The kinds are `NONE`, `OVERVOLTAGE`, `CLIPPED` (ADC at full scale), `CLIPPED_LOW` (ADC at the bottom). It is cleared when the next closed-loop run starts.

### 4.3 What the output does

| | XREX | HBRIDGE |
|---|---|---|
| General fault while `IDLE` or `ARMED` | output stopped at once (it was not running) | output stopped at once |
| General fault while `FIRING` | **every running channel ramps down open loop** to its turn-on floor over the fault ramp time (default 1.0 s, `CONFig:FaultRampTime`), then disconnects; then state `FAULT` | **immediate stop** of the output, no ramp |
| Overcurrent, Enerpro or enable-output fault on one channel while `FIRING` | that channel is switched off **at once**; every other enabled channel is stepped down by (100 / number of channels enabled) percent of its output in one step, then they ramp down together over the fault ramp time | not applicable |
| External-enable lost while `FIRING` | same as General | not applicable |

Consequences for the front-end:

- **The output keeps changing for up to the ramp time after the fault.** On XREX the state shows `FAULT` as soon as the fault is
  detected (the `!EVT ... FAULT` line arrives at once), but the controller is still ramping down for about a second. Do not
  re-arm, change settings that touch the output, or tell the operator "all is safe" the instant `FAULT` appears. Wait out the ramp time, then confirm with
  `SOURce:STATus? <ch>` that `running` is 0.
- **A single-channel fault on XREX does not stop the others immediately.** The remaining channels keep running (derated, then ramping
  down). The operator display must show *which channel* faulted, from the channel number in `STATE?`.
- A fault that was already present at power-up shows as `STATE=FAULT` in the banner and in `STATE?`.

### 4.4 Reading the cause

| Question | Command | Product |
|---|---|---|
| Is anything latched? | `FAULT?` | both |
| What type, on which channel? | `STATE?` | both |
| Which source (hardware pin, gate drivers, table, test, protection)? | `FAULT:SOURces?` | HBRIDGE |
| Which loop (phase) stopped a closed-loop run, and why? | `PID:FAULT?` | HBRIDGE |
| Which of the twelve gate-driver status inputs were in fault when `GATEDRV` last latched? | `GDS:LAST?` (`OK <pins> <checks>`, see below) | HBRIDGE |
| Raw levels of one channel's Water, Temperature, Enerpro and OCP inputs | `XREX:CHANnel:STATus? <ch>` (`OK WATER=HIGH TMP=LOW ENERPRO=LOW OCP=LOW`) | XREX |
| Raw levels of the twelve gate-driver status inputs | `GDS?` (`OK 01=HIGH 02=LOW ...`) | both |
| Interlock and trigger input levels | `EXTernal:INPut?` (XREX only), `EXTernal:TRIGger:INPut?` (both products) | see the product |
| Did the external trigger start the shot, or why not? | `EXTernal:TRIGger:STATus?` | HBRIDGE (XREX has the `!EVT ... TRIGGER FIRING` event) |
| What happened, in order, since boot | `SYS:EVLOG?` | XREX |
| Why did the controller last reset? | `DIAGnostic:RSTCause?` | both |

**`GDS:LAST?` (HBRIDGE).** `FAULT:SOURces?` only says `GATEDRV=1`. To learn which input, send `GDS:LAST?`:

```
> GDS:LAST?
< OK 101 7
```

The first number is **hexadecimal, with no `0x`**: the raw levels of the 12 gate-driver status inputs at the moment the fault latched, bit 0 =
input 01 (the lowest digit's lowest bit), bit 11 = input 12. `101` is `0001 0000 0001` binary: inputs 01 and 09. In the current build a set bit
means the input is in its fault state (the polarity is normally low); the build's polarity setting decides this, so show it as "input 01 and 09 read HIGH"
rather than hard-coding the meaning if the firmware may change. The second number counts fault checks since boot and is only a rough activity measure. Limits:
it is kept in RAM (gone at reset), is **not** cleared by `FAULT:CLEar` (it still shows the previous latch until a new one overwrites it, so use it only when `FAULT:SOURces?`
currently reports `GATEDRV=1`), says nothing about the other sources, and **cannot name a phase** because the firmware has no map from input number to phase or gate driver.
XREX has no `GDS:LAST?`; it names the channel and category in `STATE?` instead.

**The raw pin readings are not "faulted / healthy".** They report `HIGH` or `LOW` only; which level means a fault depends on each
signal's configured polarity (`CONFig:FaultPolarity:WATER|TEMP|ENERPRO|OCP`, XREX). A front-end should show them as raw levels for
a technician and use `STATE?` for the faulted/healthy decision.

### 4.5 Clearing a fault, and recovering

```
> FAULT:CLEar
< OK
> STATE?
< OK IDLE            cleared
< OK FAULT OVERCURRENT 2   still faulted: the cause is still there
```

**`FAULT:CLEar` replies `OK` whether or not it worked.** It re-checks the cause when you send it; if the cause is still present, the
fault latches again immediately, before the `OK` is even sent. So **always follow it with `STATE?`** and treat only `OK IDLE` as
"cleared". Do not loop `FAULT:CLEar` until it "works": that hides a real, persisting cause (a tripped protection, a missing
interlock, floating inputs). Show the operator that the fault is still present and let them decide.

- It never restarts output. After a successful clear the controller is `IDLE`, and getting back to firing is a new `ARM`.
- XREX only: clearing an `EXTERNAL_ENABLE` fault checks that the external-enable input is HIGH again.
- On a bench controller with nothing wired to its protection inputs, `FAULT OVERCURRENT <ch>` appears whenever a channel is enabled
  and can't be cleared; that is the inputs floating, not a software problem.

**Recovery sequence, XREX** (all with replies checked):

1. Make sure the cause is understood and gone (inspect `STATE?`, `XREX:CHANnel:STATus? <ch>`, the operator).
2. `FAULT:CLEar`, then `STATE?` must return `OK IDLE`.
3. Re-assert each enabled channel's `XREX:CHANnel:ENAOut <ch> 1` and `XREX:CHANnel:CONTactOut <ch> 1` if they were dropped, and verify with the `?` forms.
4. `ARM` (read the error text if it refuses: section 5), then start the shot.

**Recovery sequence, HBRIDGE:** `FAULT:CLEar`, `STATE?` must return `OK IDLE`; re-select the supply mode and phases if the controller was reset; `ARM`; start.

**`DEBUG:FAULT:BYPASS 1` (XREX) is not a recovery tool.** It is a bench-only override that makes the controller ignore its own fault
detection (faults stop being raised, `FAULT:CLEar` clears unconditionally, and the external-enable permissive reads satisfied), so that
signal paths can be tested with nothing wired to the protection inputs. It is off at every reset. Never send it to a controller connected to a
real supply, and do not put it in an operator front-end; the hub treats it as a dangerous command.

A fault is also a reason to **ask a human**. Your program should never include an unattended "clear and retry" path for a fault that
occurred while firing.

---

## 5. What makes `ARM` and the start commands refuse

When a command is refused the reply is `ERR <code> <text>`. **Branch on the code, display the text** (the text names the exact reason
and is meant for the operator). Codes are stable across firmware versions; texts are not.

### `ARM`

| Reply | Meaning (XREX unless stated) |
|---|---|
| `OK` | now `ARMED` |
| `ERR 13 ... not currently IDLE` | already armed, firing or faulted |
| `ERR 15 External enable interlock not satisfied ...` | the external-enable interlock is switched on (**it is ON by default**) and its input reads LOW. An unwired bench controller always does |
| `ERR 13 Channel <n>'s ENA_OUT/CONTACT_OUT not both set ...` | an enabled channel's two "ready to energize" outputs are not both HIGH. Set `XREX:CHANnel:ENAOut` and `XREX:CHANnel:CONTactOut` first |
| `ERR 6 ...` (HBRIDGE) | a fault is latched |
| `ERR 13 ...` (HBRIDGE) | not `IDLE`, or no phase is enabled (`PHASe:ENAble`) (`ARM` does not check the supply mode; with none selected there is no shot, so `FIRE` answers `ERR 5`) |

Only the **first** failing precondition is reported. Fix it and send `ARM` again to learn whether another remains.

### Starting a shot

XREX: `SHOT:STARt` (profiled, ends by itself) and `SOURce:RUN` (continuous, ends only on `SOURce:STOP` or a fault). HBRIDGE: `FIRE`
(plays the uploaded shot, ends by itself) and `SOURce:RUN` (closed loop, constant setpoints). Refusals, in the order they are checked:

| Reply | Meaning |
|---|---|
| `ERR 6` | a fault is latched (HBRIDGE `FIRE`; XREX reports the state instead) |
| `ERR 13 Must ARM first ...` | not `ARMED` |
| `ERR 15 ...` | the external-enable interlock input is LOW (XREX; also checked again at the instant of firing) |
| `ERR 12 SHOT:TIMing must be set before SHOT:STARt` | XREX: no shot timing has been set since boot |
| `ERR 5` | HBRIDGE: nothing uploaded to play, or a phase that needs a shot (open loop) has none |
| `ERR 13` | HBRIDGE: `SOURce:RUN` needs every enabled phase in closed loop (`ERR 13` also when none is) |

Re-checking the interlock **again at the instant of firing** means a condition that was fine at `ARM` can still refuse the start. Read
the reply to the start command. Never assume that a successful `ARM` guarantees a successful start.

---

## 6. External interlocks and trigger

Two physical inputs affect state and are worth explaining to operators. The **trigger is in both products**; the **external enable is XREX only**.

- **External enable** (`EXTernal:ENAble`, input read back by `EXTernal:INPut?`): a permissive from the facility. **On by default.** When on: `ARM`
  refuses unless the input is HIGH; the start command re-checks it; and while `FIRING` losing it causes `FAULT EXTERNAL_ENABLE`. While merely
  `ARMED` or `IDLE` losing it only means the next start is refused. `EXTernal:ENAble 0` turns it off for the session; **the setting is not stored, and the
  default returns on every reset.** Turning it off removes a safety permissive: do it only on a bench with nothing connected, never on a
  live system without knowing why it is not satisfied. The hub logs and announces that command as dangerous.
- **External trigger** (`EXTernal:TRIGger`): when on, a rising edge on a separate input, while `ARMED`, starts a shot exactly as `SHOT:STARt` would,
  including the interlock re-check. A `!EVT <tick_ms> TRIGGER FIRING` line (XREX) tells you it happened. Your display must handle the state
  changing from `ARMED` to `FIRING` **without** any command from your program. **HBRIDGE has it too (since 2026-10-20), with the same name and meaning, but it
  has no external enable and no `!EVT` stream**: a rising edge on PF15 while `ARMED` starts the shot as `FIRE` does, from an interrupt, with the same refusals
  as `FIRE`; `EXTernal:TRIGger:STATus?` records the edges and the reason when one did not start a shot. The XREX trigger is polled about once a millisecond; HBRIDGE's is interrupt-driven.

---

## 7. Several clients: other people change the state

Because several programs and people may be connected at once (part 1, section 8), the state can change under you. **Never assume that the state you last
read is still true.** Specifically:

- Another client may `ARM`, `DISARM`, start, stop, change a setting, or clear a fault. You learn of it from `!EVT` (XREX), the next `STATE?`, or an
  `ERR 13` on your own command.
- Do not run a **blind sequence** such as `ARM` then `SHOT:STARt` then walk away. Read each reply, and read `STATE?` before the start command if any
  time has passed since `ARM`.
- A stop control (a "kill" button) in your program should send `SOURce:STOP` and `DISARM` regardless of state, then read `STATE?`.
- If your program finds the controller `ARMED` or `FIRING` on connect, it was **not** put there by this session. Show the state and let the operator
  decide; do not silently take over or stop a shot another person started.

---

## 8. What a front-end must implement (checklist)

1. A state indicator driven by `STATE?` (and `!EVT` on XREX), showing `IDLE` / `ARMED` / `FIRING` / `FAULT` and, for `FAULT`, the type and channel.
2. A latched, visible fault banner that stays up until the controller says `IDLE` after a `FAULT:CLEar`, and shows the exact fault type, channel, and (HBRIDGE) the source from `FAULT:SOURces?` and `PID:FAULT?`.
3. A `FAULT:CLEar` action that always re-reads the state afterwards, and reports "still faulted" if it is.
4. Start controls that are only enabled when the state is `ARMED`, and that still show any `ERR` text returned.
5. An `ARM` action that surfaces the specific refusal text and code (`6`, `13`, `15`).
6. A stop control that always sends `SOURce:STOP` and `DISARM`.
7. Handling of `!BOOT`: stop all sequences, mark configuration as lost, re-identify, re-read state, and require the operator to confirm before re-sending configuration.
8. Handling of hub and link failures (part 1, section 7): stale state is marked as unknown ("link down") rather than left showing the last known value.
9. A way to see recent events (`SYS:EVLOG?`) and the reset cause (`DIAGnostic:RSTCause?`) for diagnosis.
10. No unattended auto-clear or auto-restart after a fault that occurred while firing.

Next: part 3, `docs/labview_commands.md`, the command sets.
