# WHAM-HBRIDGE-PFMG474 — Operator's Guide

For the people who run the H-bridge power supplies, the **Limiter** and the **HVPS**, through the HBRIDGE controller. It explains **what the system is and how
its parts fit together**, then gives **step-by-step instructions** for connecting, preparing, running and shutting down a shot, and for responding to faults.

A real, worked example of the whole sequence (the commands, every reply, the measured result and a plot) is in `docs/examples/limiter_per_phase_duty_shot/`.

This guide is about operating. If you are writing control software, read `labview_interface.md` (this folder) and the three LabVIEW guides at the top of the
repository (`docs/labview_hub_guide.md`, `docs/labview_state_and_faults.md`, `docs/labview_commands.md`). For every command in detail, see `command_reference.md`.
For building and flashing the firmware, see `sop/wham_hbridge_pfmg474_sop.pdf`.

**Status of this guide.** Parts of the system are proven only on a bench simulator and are marked **[bench-verified only]** or **[not yet verified]** where it matters.
Section 13 lists them. Fill in the site-specific tables in section 12 before using this guide on a live supply.

---

## 1. Safety first

1. **The controller is not the emergency stop, and this guide does not describe one.** Use the supply's own protection and the facility's emergency stops for emergencies.
   Never use a typed command as an emergency stop.
2. **The controller drives gate drivers.** Whatever power stage those gate drivers switch is live whenever a shot runs. This repository does not describe that power stage:
   follow your facility's high-voltage and gate-driver procedures, and the supply's own documentation, for everything on the power side.
3. **A fault stops the output immediately, with no ramp-down** (section 4.2). The supply must be safe to stop in that way.
4. **If you do not understand a state or a message, stop.** Leave the controller `IDLE` or `FAULT`, and ask.
5. **Several people can be connected to the controller at once, and nothing stops any of them from sending a command.** Before you arm or fire, agree out loud with everyone
   connected (section 5.4) who is in charge.
6. **Do not guess limits.** The controller refuses anything outside the selected supply's limits; do not look for a way around a refusal.

---

## 2. The system at a glance

### 2.1 What is being controlled

The HBRIDGE controller drives two supplies, selected by an operator setting called the **supply mode**:

| Supply | How it is driven | Carrier frequency | Duty (each phase) |
|---|---|---|---|
| **Limiter** | **PWM**: the pulse width (duty) varies, the frequency stays high | 75 – 125 kHz | 5 – 95 % |
| **HVPS** | **PFM**: the frequency varies, the duty stays near half | 10 – 30 kHz | 40 – 60 % |

Both are **three-phase H-bridges**: phases **U, V and W**. Each phase has a main output and its complement, with a short **dead time** between them (so the two never conduct together),
sent to gate drivers. The three phases run from one clock: they share one carrier frequency and one timeline, spaced one third of a period apart, but **each phase can have its own duty**,
and each can be switched on or off individually.

The controller also **reads back 12 gate-driver status signals**, any of which can stop the output, and it has **four analog inputs** (used as the feedback in closed-loop
operation) and **two analog outputs**.

### 2.2 How the parts connect

```
   Operator / LabVIEW program / terminal
                  |        (TCP, over the lab network, or an ssh tunnel from a remote machine)
                  v
        +--------------------+
        |   HUB (a server    |   one hub per controller; logs every line; lets several people connect at once
        |   program, wham_hub|   and gives each command's reply to the person who sent it
        +--------------------+
                  | (TCP)
                  v
        +--------------------+
        | serial bridge      |   an Ethernet-to-serial adapter on the controller; accepts exactly ONE connection (the hub's)
        +--------------------+
                  | (serial, 115200 baud)
                  v
   +-------------------------------------------+
   | HBRIDGE CONTROLLER (STM32G474 board)      |   this firmware: state machine, shot generator, per-phase control loops
   +-------------------------------------------+
        |  outputs: phase U, V, W (main + complement)  -> gate drivers -> the supply's power stage
        |  inputs:  12 gate-driver status signals, hardware fault pin
        |  analog:  4 inputs (feedback), 2 outputs
```

- The **controller and the hub** are the same arrangement as for the XREX controller: the hub is the only thing connected to the controller's serial bridge, and everyone else goes through it.
- **The same circuit board runs either firmware.** The HBRIDGE and XREX controllers use identical hardware with different firmware. Always check which one you are connected to (section 5.3).

### 2.3 Who does what

| Role | Does |
|---|---|
| **Operator** (you) | connects, checks, loads or enters a shot within agreed limits, arms, runs, watches, responds to faults |
| **Controls programmer** | builds the LabVIEW front-end; sets control gains and calibration |
| **Maintainer** | flashes firmware, repairs wiring and gate-driver hardware, sets the identity of the board |
| **Hub administrator** | runs the hub service, decides who may connect |

---

## 3. What the controller does

### 3.1 A shot is a table in time

A **shot** is a list of **setpoints**. Each setpoint says: *at this time, the carrier frequency is this and the duty is this*. Between setpoints the controller moves
frequency and duty **in straight lines**, updating every switching period. A shot lasts up to **1 second** and may have up to **500 setpoints**. When the last setpoint's time is reached the
output stops by itself and the controller returns to `IDLE`.

```
 duty
  ^
  |          setpoint 2 (t = 10 ms, 80 %)
  |         /
  |        /
  |_______/  setpoint 1 (t = 0, 20 %)
  +------------------------------> time
```

**Per-phase duty.** A setpoint can carry **one duty for all three phases**, or **a separate duty for each of U, V and W** (the time and the frequency stay shared).

**Everything is checked when you enter it**: a frequency or duty outside the selected supply's limits is refused at once (`ERR 10`), so a shot already accepted cannot go outside them.

### 3.2 Open loop and closed loop, per phase

- **Open loop (the default).** The phase plays the shot exactly as entered. Nothing measures the result.
- **Closed loop.** The controller measures that phase's output voltage on an analog input and continuously adjusts the phase (Limiter: its duty; HVPS: the carrier frequency) so that the measured
  voltage follows a **voltage target** (a second table in time, entered like a shot). Each phase has its own loop, its own feedback input, and its own gains.
- **A run may mix them.** For example: phase U closed loop at 300 V while V and W follow fixed duties from the shot. The closed phases follow their targets, the open phases follow the shot, at one carrier frequency.
- **The HVPS has only one control loop** (loop 1), because its actuator, the frequency, is shared by all three phases.

The three loops are numbered like the phases: **1 = U, 2 = V, 3 = W**. By default their feedback inputs are **U on analog input PA0, V on PA1, W on PB0.**

> **Closed loop is not yet proven on real analog feedback [not yet verified].** The loops were verified on the simulator board against the firmware's own plant models, but the board's
> analog output-to-input path read zero when tried, so a loop closed on a real signal has not been seen working. Gains in the code are bench values. Treat closed-loop runs on a real supply as
> commissioning, with the controls programmer present.

### 3.3 The supply mode

You must select the mode before entering any shot, and the mode decides which limits apply. It starts as `NONE` after every reset. **Changing the mode throws away the uploaded shot**
(so that a shot checked against one supply is never played into the other).

---

## 4. How the system protects itself

### 4.1 The controller's states

| State | Meaning | Front-panel LED |
|---|---|---|
| `IDLE` | resting; output off; the normal state after boot, after a shot, and after a fault is cleared | IDLE lit |
| `ARMED` | "ready to fire": the controller checked its preconditions. **Output is still off** | ARMED lit |
| `FIRING` | a shot or run is in progress; output on | FIRING lit |
| `FAULT` | a fault was detected and latched; stays until cleared on purpose | FAULT lit |

Exactly one state LED is lit at any time and it always agrees with the `STATE?` command. Two more LEDs show serial activity: **TX** flickers when the controller sends a line, **RX** when it receives one.
They are cosmetic.

Every shot needs its own `ARM`. When a shot ends the controller returns to `IDLE`, not `ARMED`.

### 4.2 Faults

The HBRIDGE has one fault type, `FAULT GENERAL`. It is raised from **any** state by:

| Source (`FAULT:SOURces?` field) | Cause |
|---|---|
| `PC10` | the native hardware fault input tripped. This works in silicon, even if the processor has hung |
| `GATEDRV` | any one of the 12 gate-driver status inputs read as a fault |
| `TABLE` | the controller could not keep up while playing a shot (an internal overrun); the shot was stopped. Not an operator error |
| `OVP` | closed loop: the measured voltage went beyond the overvoltage limit |
| `FB` | closed loop: a feedback problem (the analog input stuck at the top or the bottom of its range for too long, or the loopback signal stopped) |
| `TEST` | a test fault from `GENERAL:TEST:FAULT` |

**Response: the output is stopped immediately, with no ramp-down.** The controller then stays in `FAULT` until cleared. A fault is **latched**: it stays even after the cause goes away.

Notes:
- When a **closed-loop** run is stopped by its own protection, `PID:FAULT?` says **which loop (phase)** and why (`OVERVOLTAGE`, `CLIPPED`, `CLIPPED_LOW`).
- The gate-driver status inputs are **not** masked for disabled phases: the firmware has no record of which input belongs to which phase, so any of the 12 can fault the controller even if you have left that phase out.
- Unlike the XREX controller, this firmware has **no external-enable input** and no software interlock to configure. The only things that stop an armed controller are
  the faults above and your own `DISARM`/`SOURce:STOP`.
- **It does have an external trigger** (section 4.3): a rising edge on one input starts the shot while the controller is armed.

### 4.3 The external trigger

A **rising edge on the controller's trigger input (PF15, the "Fiber_Enable" fiber input)** while the controller is `ARMED` **starts the shot**, exactly as typing `FIRE` would: the same checks and the
same shot. It is handled by interrupts, so the shot starts within a fraction of a millisecond of the edge, with nobody typing anything. **It is on by default.**

What this means for you:
- **An armed controller can fire without anyone sending a command.** `ARM` is therefore the last deliberate step: from the moment you arm, a rising edge on the trigger input starts the shot.
  Do not leave a controller armed, and do not arm with the trigger signal source live unless that is the plan.
- **Only a new rising edge counts.** A signal that is already high when you `ARM` does nothing. After a start is refused (no shot uploaded, a fault, a closed-loop problem) the controller stays armed
  and needs a **new** edge.
- **Anything that stops `FIRE` stops the trigger** (a latched fault, nothing uploaded, not armed). It leaves a record: `EXTernal:TRIGger:STATus?` shows how many edges were seen, how many started a
  shot, how many were ignored and why, and the reason for the last refusal (`last=NO_SHOT`, `last=FAULT`, ...).
- `EXTernal:TRIGger?` shows whether it is on; `EXTernal:TRIGger 0` turns it off for the session (it is on again after every reset); `EXTernal:TRIGger:INPut?` shows the input level right now.
- A shot started this way is **not announced** as such: there is no event stream on this controller. After the shot, `EXTernal:TRIGger:STATus?` (`started` went up, `last=STARTED`) tells you it was the trigger.
- **[no real rising edge has been applied yet]**: see section 13.

### 4.4 The emergency stop
There is no emergency-stop function in this controller's software and this guide does not define one. Emergency stopping is the supply's and facility's responsibility (section 1).

---

## 5. Getting connected

The full connection guide, with switching controllers and troubleshooting, is `docs/operator_hub_connection.md` at the top of the repository; this section is the short form.

### 5.1 What you need
- A computer on the lab network, **or** a remote computer with ssh access to the hub's server (section 5.2).
- The **hub's host name and the port for this controller** (one port per controller). Get these from the hub administrator; write them in section 12.
- A terminal. `python3 firmware/hbridge/python/net_client.py` is the simple client used below.

### 5.2 From a remote computer: the tunnel
If the hub is not directly reachable, open a tunnel through the server that runs it (it asks for your password and then shows nothing; leave the window open):

```bash
ssh -N -o ServerAliveInterval=30 -L <local-port>:127.0.0.1:<hub-port> <server>
```

Then connect to `127.0.0.1:<local-port>` as below. If the tunnel window shows a timeout, the network dropped: open it again and reconnect your client.

### 5.3 Connect and check you have the right controller

```bash
python3 firmware/hbridge/python/net_client.py --port <port> --name <your-name>
```

Type, pressing Enter after each:

```
*IDN?
STATE?
!hub status
!hub clients
```

- `*IDN?` must start with `OK WHAM-HBRIDGE-PFMG474`. **If it says anything else (for example `WHAM-XREX-PFMG474`), you have the wrong controller: disconnect.** The two controllers
  use the same hardware and have different command sets. Never assume which controller a port number reaches.
- It shows the board's serial (`SN=UNSET` means none has been assigned yet) and the firmware version.
- `STATE?` should be `OK IDLE` before you start.
- `!hub status` should show `connected=1`; `!hub clients` lists everyone connected.

### 5.4 Others on the same controller
The hub lets several people connect at once and **there is no lock**: anyone connected can send any command. The hub announces dangerous commands to everyone and logs them, but does not stop
them. So: check `!hub clients` and **agree with everyone listed** who is operating; if you find the controller `ARMED` or `FIRING` when you connect, find out who did that before touching
anything; a program (such as the LabVIEW front-end) counts as someone.

### 5.5 Leaving
Press Ctrl-D in the client window. Closing your connection does not stop a shot or change the state. To close a tunnel, press Ctrl-C in its window.

---

## 6. Normal operation, step by step

**If a reply is not what this guide says, stop and ask.** Every typed line is one command; wait for the reply before sending the next. `OK ...` is success; `ERR <number> <text>` is a refusal
with the reason.

### 6.1 Before any shot: checks
1. Connect (section 5.3). Confirm the product, serial and the person list.
2. `STATE?` must be `OK IDLE`. If it is `OK FAULT GENERAL`, go to section 9. If `ARMED` or `FIRING`, find out why first.
3. `FAULT?` must be `OK 0`.
4. After any power-up or reset, **nothing you set survives** unless it was stored in a profile and recalled (section 6.6): redo the setup.
5. `CONFig:MODE?` shows `NONE` after every reset. `PHASe:ENAble?` shows which phases will drive (all three by default).

### 6.2 Choose the supply and the phases (IDLE only)

```
CONFig:MODE LIMiter              -> OK      or HVPS
CONFig:LIMits?                   -> OK LIMITER FREQ 75000 125000 DUTY 5 95     check these are the limits you expect
PHASe:SELect UVW                 -> OK      exactly these phases drive (U, V and W); use e.g. UW to leave V out
PHASe:ENAble?                    -> OK U=1 V=1 W=1
```

A phase you leave out is **never connected to its output pins** while the shot runs: its gate drivers see no switching at all. `ARM` refuses if no phase is enabled.

### 6.3 Enter an open-loop shot

```
SHOT:BEGin                              -> OK
SHOT:STEP <time_us> <freq_hz> <duty_pct>                -> OK    one duty for all three phases
SHOT:STEP <time_us> <freq_hz> <dU> <dV> <dW>            -> OK    a duty for each phase
   ... one line per setpoint ...
SHOT:END                                -> OK <setpoints> <duration_us>
```

**Rules:** the first setpoint is at time 0; times strictly increase; the last time is the shot's length (at most 1,000,000 µs = 1 s); times are in **microseconds**, frequency in **hertz**, duty in **percent**.
Enter one line at a time and **check each reply is `OK`**: if one is refused, nothing after it makes sense.

| You see | Meaning |
|---|---|
| `ERR 13 No supply mode selected ...` | send `CONFig:MODE` first |
| `ERR 10 ...` | a frequency or duty is outside the mode's limits (it names which, and which phase); fix the number |
| `ERR 4 ...` | the times are not strictly increasing, a value is malformed, or the time exceeds 1 s |
| `ERR 3 ...` | too many setpoints (limit 500) |
| `ERR 12 not a number: ...` | a typo; the whole token must be a plain number |

Examples (illustrative; choose your own values):

**Limiter: constant 100 kHz at 50 % on all phases for 10 ms**
```
SHOT:BEGin
SHOT:STEP 0 100000 50
SHOT:STEP 10000 100000 50
SHOT:END                      -> OK 2 10000
```

**Limiter: duty ramps from 20 % to 80 % over 10 ms**
```
SHOT:BEGin
SHOT:STEP 0 100000 20
SHOT:STEP 10000 100000 80
SHOT:END
```

**Limiter: a different duty on each phase (U 20 %, V 50 %, W 80 %, then U and W swap)**
```
SHOT:BEGin
SHOT:STEP 0 100000 20 50 80
SHOT:STEP 10000 100000 80 50 20
SHOT:END
```

**HVPS: frequency ramps from 20 kHz down to 15 kHz, duty 40 % to 60 %** (after `CONFig:MODE HVPS`)
```
SHOT:BEGin
SHOT:STEP 0 20000 40
SHOT:STEP 10000 15000 60
SHOT:END
```

Read it back before arming: `SHOT?` (setpoint count, duration), and `SHOT:POINt? <n>` for any setpoint you want to check.

### 6.4 Closed-loop runs (only when told to, with the controls programmer)

```
PID:LOOPMODE 1 1                  -> OK      close phase U (loops are 1 = U, 2 = V, 3 = W; 0 = all three)
PID:GAINS 1 <kp> <ki> <kd>        -> OK      the controls programmer gives you these
TARGet:BEGin                      -> OK
TARGet:STEP 0 0                   -> OK      first target at time 0, in process volts
TARGet:STEP 25000 300             -> OK      at 25 ms the target is 300; or give a target per phase: TARGet:STEP 25000 300 -300 500
TARGet:END                        -> OK 2 25000
```

- A phase left open follows its duty from the shot (so you also need a shot if any enabled phase is open), and the carrier frequency then comes from the shot; with every phase
  closed the carrier is the fixed closed-loop carrier (`CONFig:CLOSEDloop:FREQuency`).
- A target beyond the overvoltage limit is refused. An HVPS target cannot be negative.
- Read everything back: `PID:LOOPMODE? 0` (the three loop modes, e.g. `OK 1 0 0`), `PID:GAINS? <loop>`, `TARGet?`.
- **Do not change feedback calibration, plant settings or gains yourself.** They are commissioning settings (section 11).

### 6.5 Arm and run

```
ARM            -> OK        nothing electrical has happened yet
STATE?         -> OK ARMED
```

`ARM` refusals:

| You see | Meaning | What to do |
|---|---|---|
| `ERR 6 Fault latched` | a fault is latched | section 9 |
| `ERR 13 ... not IDLE` | already armed, firing or faulted | `STATE?` and find out why |
| `ERR 13 ... no phase` | no phase is enabled | `PHASe:SELect` |

To stand down without running: `DISARM`. **While armed, a rising edge on the trigger input starts the shot by itself (section 4.3).**

**Run:** confirm that everyone who needs to be clear is clear, then

```
FIRE
```

`OK` means it is running (`STATE?` = `OK FIRING`). The shot is short (at most 1 s) and **ends by itself**; poll `STATE?` every 50-100 ms until `OK IDLE`. Refusals:

| You see | Meaning |
|---|---|
| `ERR 6` | a fault is latched |
| `ERR 13` | not `ARMED` (a fault, or someone else's action, intervened) |
| `ERR 5` | there is nothing to play: no shot uploaded (or an open phase has no shot) |

**There is no stop-in-the-middle for a shot** except `SOURce:STOP` or a fault. For a continuous closed-loop run (`SOURce:RUN`, every enabled phase closed, holding each loop's `SOURce:SETpoint`),
`SOURce:STOP` ends it. Neither stop command is an emergency stop.

### 6.6 After the run
1. Wait for `STATE?` = `OK IDLE`. If it is `OK FAULT GENERAL`, go to section 9.
2. For a closed-loop run, `PID:FAULT?` should be `OK 0 NONE`.
3. If logging was armed, read the log now (below).
4. Send `DISARM` if you armed and did not run.

**Waveform log of a closed-loop run (optional).** Before arming: `LOG:ARM <loop> <maxSamples> <decim>` (loop 0 = all three; up to 1000 samples for one loop, 333 for all three; one sample every `decim`-th control tick, the
control rate being 10,000 per second). After the run: `LOG:DATA? <loop> <start> <count>` returns up to 40 samples per request with a check sum; loop `start` from 0 in steps of 40. Values are millivolts and milli-hertz or
milli-percent. The log is in the controller's memory and is lost at reset: save it first. (Engineers' tools do this automatically.)

**Stored profiles.** Eight slots in the controller's flash keep the settings **and** the shot, target and HVPS-curve tables, so a prepared run can be recalled after a reset:
- `CONFig:PROFile:CATalog?` lists the slots;
- `CONFig:PROFile:DIFF? <slot>` shows what a recall would change (`OK SAME` if nothing);
- `*RCL <slot>` recalls it (IDLE only); the reply is `OK APPLIED=<n> FAILED=<k>` and **`FAILED` must be 0**;
- `*SAV <slot> <label>` saves what is currently set (the label is 1-15 characters, no spaces).

Nothing is recalled automatically at power-up. The **phase selection is not stored**: check it after a recall. Saving overwrites the slot.

---

## 7. Stopping

| Situation | Action |
|---|---|
| Armed but changed your mind | `DISARM` |
| A run is going and you want it stopped | `SOURce:STOP` (then `STATE?` must be `OK IDLE`) |
| Emergency | the supply's and facility's own emergency stops. **Not a command** |
| Do not want the trigger to be able to fire | `EXTernal:TRIGger 0` (until the next reset); `ARM` is still needed for `FIRE` |

---

## 8. What the logs and records are

- **The hub** keeps a record of **every line, in both directions**, with a timestamp, under the controller's serial number (or its chip ID if it has none yet), one file per day. It records which person
  (the name you gave your client) sent each command and marks dangerous ones. Ask the hub administrator for the location.
- **The controller** keeps no event history: this firmware has no `!EVT` stream. Its record of a run is the optional waveform log and the reply history in the hub's log.
- Record the date, time, your name, the shot (or its stored-profile slot), and the outcome in the operations log your facility uses.

---

## 9. Faults: what to do

**Do not clear a fault just to make it go away.** A fault was raised for a reason. Find it first.

1. **Read it.** `STATE?` gives `OK FAULT GENERAL`. `FAULT:SOURces?` says which kind (`OK PC10=0 GATEDRV=1 TABLE=0 TEST=0 OVP=0 FB=0`). Note it.
2. **The output has already stopped.** Make the supply safe by the facility's own means before touching anything.
3. **Find the cause.**
   - `GATEDRV=1`: a gate-driver status input. `GDS:LAST?` replies `OK <hex> <count>`: the raw levels of the 12 inputs when the fault latched, as hex digits, lowest bit = input 01. In the current build a set bit is a faulted input
     (e.g. `101` = inputs 01 and 09). `GDS?` shows the live levels. **The firmware cannot tell you which phase or gate driver an input belongs to**: ask the maintainer, who has the wiring.
   - `PC10=1`: the hardware fault input: check what is wired to it.
   - `OVP=1` or `FB=1` (closed loop): `PID:FAULT?` names the loop (1 = U, 2 = V, 3 = W) and the kind: `OVERVOLTAGE`, `CLIPPED` (feedback at full scale) or `CLIPPED_LOW` (feedback at the bottom: a dead or disconnected feedback line).
   - `TABLE=1`: the controller overran while playing a very demanding shot (for example a step every period at 125 kHz). Simplify the shot and tell the controls team.
   - `TEST=1`: someone injected a test fault.
4. **Fix the cause.**
5. **Clear it, deliberately:**
   ```
   FAULT:CLEar
   STATE?
   ```
   **`FAULT:CLEar` always answers `OK`, even if it did not work.** It re-checks the gate-driver inputs; if one still reads as a fault the fault comes straight back. Only `OK IDLE` means it cleared. Do not repeat it in a loop.
6. **Recover:** the controller is `IDLE`. Re-check the supply mode and phases (a reset clears them), then start again from section 6.5.

**After a controller reset, a power cut or a firmware update** the controller prints a three-line `!BOOT` banner. Everything you set is gone: the mode is `NONE`, every phase is enabled, all loops are open and the shot is empty. Check the state, redo
section 6 (or recall a profile), and record why it reset (`DIAGnostic:RSTCause?`) in the operations log.

**On the bench**, with nothing wired to the gate-driver status inputs, the inputs can read as faults and the controller may latch `FAULT GENERAL` as soon as it starts a shot. This is a known bench condition (floating inputs),
not a software fault; it must never be seen on a real supply.

---

## 10. If the connection fails

| Symptom | Likely cause | What to do |
|---|---|---|
| Client says `Connection refused` | the tunnel is not running, or the hub is down | start the tunnel (section 5.2); if it persists, ask the hub administrator |
| Client connects and is closed at once | your computer's address is not on the hub's allowed list | ask the hub administrator to add it |
| Replies show `ERR hub timeout` | the controller did not answer in time; the command **may or may not** have run | read back with a query before doing anything else; never resend a command that starts output |
| `ERR hub the controller is not connected` | the hub lost the controller (cable, power, serial bridge) | check the controller's power and network cable; `!hub status` shows `connected=0` until it returns |
| `ERR hub ... restarted ...` | the controller reset | treat as a reset (section 9, last paragraph) |
| Nothing at all and the TX/RX LEDs are dark | the controller may be unpowered or held in reset | check power; ask the maintainer |

A shot already running keeps running to its end on its own if you lose your connection.

---

## 11. Things operators must not do

| Do not | Why |
|---|---|
| Change `CONFig:FEEDback:*`, `CONFig:CLOSEDloop:*`, `CONFig:MAXVolts`, `CONFig:SLEWRate`, `CONFig:PIDRate`, `CONFig:ADC:*` | commissioning settings: they change the control loop's behaviour and its protection |
| Change `PID:GAINS` | tunes the control loop; the controls programmer only |
| Send `FWUPdate:*` or `BOOT` | reflash or reset the controller. Maintainer only, with the controller idle |
| Send `SYS:SERial`, `SYS:LABel`, `SYS:ID:CLEar` | change the board's permanent identity |
| Send `GENERAL:TEST:FAULT` | injects a fault on purpose; testing only |
| Set `DAC:OUTPut` to anything other than 0 without a reason | the two analog outputs are real outputs and are **independent of the state machine and of faults**: they stay on in `IDLE` and `FAULT` |
| Use `SIM:*` or `CONFig:FEEDback:SOURce SIM` / `DAC` / `LOOP` | simulator and bench-loopback modes, which replace the real feedback with a model |
| Use the `TABle:*` commands | a bench-only way of entering a per-period table (at most 100 entries); use `SHOT:*` |
| Clear a fault in a loop | hides a persisting cause |
| Assume the state you last saw is still true | others can connect and change it |

Where a command is marked dangerous (it starts or enables output, arms, weakens a protection, forces or clears a fault, or reflashes), the hub announces it to everyone connected and logs it.

---

## 12. Fill in for your installation

| Item | Value |
|---|---|
| Hub host and port for this controller | |
| Server to tunnel through (if remote) | |
| Hub administrator (contact) | |
| Maintainer (contact) | |
| Which supply this controller drives (Limiter or HVPS) | |
| Location of the hub's logs | |
| Operations log location | |
| Emergency stop (location, who tests it) | |

| Controller phase | Gate drivers / output it drives | Feedback analog input | Notes |
|---|---|---|---|
| U (loop 1) | | PA0 (analog input 1) by default | |
| V (loop 2) | | PA1 (analog input 2) by default | |
| W (loop 3) | | PB0 (analog input 4) by default | |

| Gate-driver status input (`GDS?` number) | Belongs to (phase / gate driver) |
|---|---|
| 01 – 12 | *(not recorded in the firmware: take from the wiring)* |

---

## 13. What has and has not been verified

| Item | Status |
|---|---|
| State machine (`ARM`, `FIRE`, auto-stop, `DISARM`), fault latching and clearing | verified on the bench and the simulator board |
| Open-loop shots, per-phase duties | verified on the simulator board by measuring the real output edges (programmed 20/50/80 % read 18.1/48.1/78.0 %: the same constant offset of about 1.9 points on every phase, the known dead time) |
| Per-phase on/off | verified: only the enabled phases switch |
| Closed loop, three loops, mixed runs, loop-attributed faults | verified on the simulator **against the firmware's own plant models** |
| Closed loop on **real analog feedback** | **[not yet verified]**: the board's analog output-to-input path read zero when tried |
| Carriers 100 – 125 kHz (the top of the Limiter range) | **[not yet verified]** on a logic analyzer; verified up to 100 kHz |
| Real gate-driver status signals and the hardware fault pin with real hardware | **[not yet verified]**: bench inputs float; a real trip of the hardware fault pin has not been captured |
| Which gate-driver input belongs to which phase | **[unknown]**: not recorded |
| External trigger on PF15 | added 2026-10-20; the start logic is covered by host tests (50 checks); **[flashed to the bench board and the commands verified; no real rising edge has been applied yet]**. The fibre receiver and the signal that will drive it are untested |
| Control-loop timing margin | three simulated loops at the highest carrier use about 80 % of the control tick; the margin is real but not large |

When in doubt about a behaviour, treat it as unverified and ask the controls team.

---

## 14. Quick reference

| I want to... | Type |
|---|---|
| see what the controller is doing | `STATE?` |
| check for a fault, and which kind | `FAULT?`, `FAULT:SOURces?`, `GDS:LAST?` |
| choose the supply | `CONFig:MODE LIMiter` or `HVPS` |
| see the limits | `CONFig:LIMits?` |
| choose phases | `PHASe:SELect UVW` (or `UW`, `W`, ...) |
| enter a shot | `SHOT:BEGin`, `SHOT:STEP ...` per setpoint, `SHOT:END` |
| check a shot | `SHOT?` |
| close / open a phase's loop | `PID:LOOPMODE <1-3> 1` / `0` |
| enter a target | `TARGet:BEGin`, `TARGet:STEP ...`, `TARGet:END` |
| arm / stand down | `ARM` / `DISARM` |
| run | `FIRE` (or a rising edge on the trigger input, while armed) |
| stop (not an emergency stop) | `SOURce:STOP` |
| see or switch the external trigger | `EXTernal:TRIGger?`, `EXTernal:TRIGger 0|1`, `EXTernal:TRIGger:STATus?` |
| clear a fault (check the result) | `FAULT:CLEar`, then `STATE?` |
| see who is connected | `!hub clients` |
| restore saved settings | `CONFig:PROFile:DIFF? <slot>`, then `*RCL <slot>` |
| leave | Ctrl-D |

**Glossary.** *Carrier*: the switching frequency. *Closed loop*: the controller measures the output and corrects it. *Dead time*: the short gap between a phase's main and complement outputs. *Duty*: the fraction of each period a phase
is on. *Gate driver*: the circuit that switches a power transistor. *HVPS*: the PFM-driven supply. *Hub*: the server program that shares the controller among people and logs everything. *Limiter*: the PWM-driven supply. *PFM*:
pulse-frequency modulation. *PWM*: pulse-width modulation. *Phase*: one of U, V, W. *Process volts*: the unit of closed-loop voltages, set by the feedback calibration. *Latched*: stays set until cleared on purpose. *Shot*: one programmed run.
