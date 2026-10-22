# WHAM-XREX-PFMG474 — Operator's Guide

For the people who run the Transrex magnet power supplies through the XREX controller. It explains **what the system is and how its
parts fit together**, then gives **step-by-step instructions** for connecting, preparing, firing and shutting down a shot, and for
responding to faults.

This guide is about operating. If you are writing control software, read `labview_interface.md` (this folder) and the three LabVIEW guides at the
top of the repository (`docs/labview_hub_guide.md`, `docs/labview_state_and_faults.md`, `docs/labview_commands.md`). For every command in detail, see
`command_reference.md`. For building and flashing the firmware, see `sop/wham_xrex_pfmg474_sop.pdf`.

**Status of this guide.** Parts of the system are proven only on the bench and are marked **[bench-verified only]** or **[not yet verified]**
where it matters. Section 13 lists them. Fill in the site-specific tables in section 12 before using this guide on the live system.

---

## 1. Safety first

1. **The controller is not the emergency stop.** The Transrex design requires the E-stop to work even if the controller's software has crashed. Never treat a
   command typed into this system as an emergency stop. Use the physical E-stop (section 4.3).
2. **Nothing here protects the magnets by itself.** The supplies' own protection (overcurrent comparators, interlocks) and the facility interlocks are the
   primary protection. The controller reacts to them; it does not replace them.
3. **If you do not understand a state or a message, stop.** Leave the controller `IDLE` or `FAULT`, and ask. Nothing in this guide requires you to
   guess.
4. **Several people can be connected to the controller at once, and nothing stops any of them from sending a command.** Before you arm or fire, agree
   out loud with everyone else who is connected (section 5.4) who is in charge.
5. **Never turn a safety setting off on a connected system** (section 11). A bench controller with nothing attached is different; ask before assuming
   you are on one.

---

## 2. The system at a glance

### 2.1 What is being controlled

Four **Transrex ISR-2126 magnet power supplies**, connected in parallel, power the two Phaedrus mirror coils and the two W7A central trim coils.
Each Transrex has a modern 12-pulse SCR firing board made by Enerpro, driven by an analog demand signal (0–10 V, mapped to a firing angle of 180° to 0°).
The upgrade added: current sensors (LEMs) on each unit, a new **interface board** in each Transrex, and a new **control unit** (this controller),
with every signal between the Transrexes and the control unit carried over **fiber optic cable** to avoid ground loops.

What the control unit does is **closed-loop current control**: it measures each supply's output current and continuously adjusts that supply's demand
to follow a programmed current profile (a "shot"). The original system was open loop; the closed loop makes the field repeatable and lets one faulty unit
trigger a graceful ramp-down of the others instead of an abrupt trip of all four.

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
   +------------------------------------+
   | XREX CONTROLLER (STM32G474 board)  |  this firmware: state machine, four PID loops, fault handling
   +------------------------------------+
        |  per channel (x4), all fiber optic            |  system-wide
        |    DEMAND out   (0-100 kHz, = firing angle)   |    SYS_ENABLE in  (facility permissive)
        |    UNIT_CURRENT in (0-100 kHz, = current)      |    SHOT_START in  (external trigger)
        |    ENABLE out, CONTACTOR out                  |    E-STOP        (hardware, section 4.3)
        |    OCP in, FAULT_INTLK in (water/temp/Enerpro)|
        v                                               
   Interface board in each Transrex  ->  Enerpro firing board  ->  SCRs  ->  magnet current
```

- **Signals are encoded as frequencies on the fiber.** The analog demand and current are sent as a frequency between 100 Hz (zero) and 100 kHz (full scale).
  Three slow faults (water, temperature, Enerpro) share one fiber by using different tones: 500 Hz, 5 kHz and 50 kHz.
- **The controller has four independent channels.** Channel *n* talks to Transrex *n*. A channel has no relationship to the others except that they share one
  shot time base and the system state (all four are armed together, fired together, and a fault in one is handled with the others in mind).
- **Which channel is which Transrex and coil** is a wiring fact for your installation. It is not stored in the controller: fill in the table in section 12.
  A channel can be given a text nickname (`CHANnel:NICKname`) to make that visible.

### 2.3 Who does what

| Role | Does |
|---|---|
| **Operator** (you) | connects, checks, configures a shot within agreed limits, arms, fires, watches, responds to faults |
| **Controls programmer** | builds the LabVIEW front-end; changes control gains and calibration |
| **Maintainer** | flashes firmware, repairs the fiber and interface hardware, sets the identity of the board |
| **Hub administrator** | runs the hub service, decides who may connect |

---

## 3. What the controller does during a shot

A **shot** is a trapezoid of current, the same shape in every channel:

```
 current
    ^        peak (per-channel demand, e.g. 3000 A)
    |        ______________
    |       /              \
    |      /   flat-top      \
    |     /                    \
    +----/------------------------\-----------> time
        ramp up               ramp down
```

You set, in advance: the three **times** (ramp-up, flat-top, ramp-down; seconds, shared by all channels) and each channel's **peak current** (amps).
The controller turns that into a demand for each Transrex and, while the shot runs, adjusts each channel's demand so that the **measured** current follows
it (a PID loop, 1000 times per second). When the shot ends the controller turns the output off by itself and goes back to `IDLE`.

- **Closed loop is the default.** A channel can be switched to open loop (`PID:LOOPMODE <ch> 0`), which sends the programmed demand with no correction.
  Operators should not change this without being told to.
- **Current-to-demand calibration is a placeholder.** The conversion from amps to demand frequency is a straight line between a "turn-on" frequency and a maximum
  frequency, **pending real characterization** against the supplies. Treat the numbers in amps as approximate until the commissioning team has calibrated them. **[not yet verified]**
- **A channel can be left out** (`SOURce:ENAble <ch> 0`): its outputs are disconnected, it takes no part in the shot, and its fault inputs are ignored.

---

## 4. How the system protects itself

### 4.1 The controller's states

| State | Meaning | Front-panel LED |
|---|---|---|
| `IDLE` | resting; output off; the normal state after boot, after a shot, and after a fault is cleared | IDLE lit |
| `ARMED` | "ready to fire": the controller checked its preconditions. **Output is still off** | ARMED lit |
| `FIRING` | a shot is in progress; output on | FIRING lit |
| `FAULT` | a fault was detected and latched; stays until cleared on purpose | FAULT lit |

Exactly one state LED is lit at any time and it always agrees with the `STATE?` command. Two further LEDs show serial activity: **TX** flickers when the controller
sends a line and **RX** when it receives one. They are cosmetic: a steady state LED with flickering TX/RX is a healthy, communicating, idle controller.

Every shot needs its own `ARM`. When a shot ends the controller returns to `IDLE`, not `ARMED`.

### 4.2 What triggers a fault, and what happens

| Fault (as `STATE?` shows it) | Cause | Response |
|---|---|---|
| `FAULT OVERCURRENT <n>` | channel *n*'s overcurrent protection output tripped | channel *n* is switched off at once; every other running channel is stepped down by 100/N % (25 % with four channels) of its own output and then ramps down over the fault ramp time (1.0 s by default) |
| `FAULT ENERPRO <n>` | channel *n*'s Enerpro firing board reported a fault | same response as overcurrent |
| `FAULT GENERAL` | the controller's native hardware fault input, or a **water** or **temperature** fault on any enabled channel | **all** running channels ramp down together to zero demand over the fault ramp time, then disconnect |
| `FAULT EXTERNAL_ENABLE` | the facility "system enable" permissive was lost while firing | same as `GENERAL` |
| `FAULT ENABLE_OUTPUT <n>` | channel *n*'s enable or contactor output (the lines this controller drives to the Transrex) was no longer set while armed or firing | same response as overcurrent |

Notes:
- **The state shows `FAULT` immediately, but the output keeps ramping for about a second.** Do not treat the supplies as safe, re-arm, or change settings in
  that time. Wait until the ramp has finished (about the fault ramp time).
- A fault is **latched**: it stays even after the condition clears, until someone clears it on purpose (section 9).
- Only **enabled** channels count. A channel you have left out cannot cause a fault or block anything.
- On the supply side, an overcurrent also locks the Enerpro enable out for **120 seconds** (hardware, in the interface board). After an overcurrent trip you
  cannot fire that supply again for 120 s regardless of what the controller does.

### 4.3 The emergency stop

The Transrex upgrade plan requires that **the E-stop be completely independent of software**, so that a crash or lock-up of the controller cannot stop an operator from stopping
the system, and that pressing it sets the enable and contactor fibers to all four Transrexes low immediately. In this controller the pin map assigns the E-stop fiber receiver's
input to the processor's **reset pin**; the firmware has **no E-stop command** (a software E-stop feature was removed, and its pin is documented as "permanently retired" in
`pin_mapping_reference.pdf`). **[Not yet verified: this repository has no test record of the E-stop path being exercised against the real supplies.]** The E-stop procedure and its
periodic test belong to the facility, not to this software. **Do not use any typed command as a substitute.**

### 4.4 Permissives and interlocks the controller watches

| Input | Meaning | What it gates |
|---|---|---|
| **External enable** (the plan's SYS_ENABLE: a fiber input from the facility control system) | a permissive: high = OK to run | **On by default.** `ARM` is refused unless it is high; the start command re-checks it; losing it **while firing** is a fault. Losing it while only armed or idle simply means the next start is refused |
| **External trigger** (the plan's SHOT_START: the facility's "go" fiber input) | when switched on, a rising edge while `ARMED` starts the shot, as if you had pressed Fire | off by default; a trigger start still re-checks the external enable |
| **Per channel: water, temperature, Enerpro, overcurrent** | fault inputs from each Transrex | see 4.2 |
| **Per channel: enable and contactor outputs** | this controller's own "ready to energize" lines to each Transrex | `ARM` is refused unless both are on for every enabled channel |

---

## 5. Getting connected

The full connection guide, with switching controllers and troubleshooting, is `docs/operator_hub_connection.md` at the top of the repository; this section is the short form.

### 5.1 What you need
- A computer on the lab network, **or** a remote computer with ssh access to the hub's server (section 5.2).
- The **hub's host name and the port for this controller** (one port per controller). Get these from the hub administrator; write them in section 12.
- A terminal. `python3 firmware/hbridge/python/net_client.py` (or the identical copy in `firmware/xrex/python/`) is the simple client used below.

### 5.2 From a remote computer: the tunnel
If the hub is not directly reachable, open a tunnel through the server that runs it (it asks for your password and then shows nothing; leave the window open):

```bash
ssh -N -o ServerAliveInterval=30 -L <local-port>:127.0.0.1:<hub-port> <server>
```

Then connect to `127.0.0.1:<local-port>` as below. If the tunnel window shows a timeout, the network dropped: open it again (and reconnect your client).

### 5.3 Connect and check you have the right controller

```bash
python3 firmware/xrex/python/net_client.py --port <port> --name <your-name>
```

Type, pressing Enter after each:

```
*IDN?
STATE?
!hub status
!hub clients
```

- `*IDN?` must start with `OK WHAM-XREX-PFMG474`. **If it says anything else (for example `WHAM-HBRIDGE-PFMG474`), you have the wrong controller: disconnect.**
  Never assume which controller a port number reaches.
- It also shows the board's serial number (`SN=`), once one has been assigned (`SN=UNSET` means none yet) and the firmware version.
- `STATE?` should be `OK IDLE` before you start (section 6).
- `!hub status` should show `connected=1`. `!hub clients` lists everyone connected.

### 5.4 Others on the same controller
The hub lets several people connect at once and **there is no lock**: anyone connected can send any command, including `ARM` and the commands that start output.
The hub announces dangerous commands to everyone and logs them, but it does not stop them. So:
- before you arm, check `!hub clients` and **agree with everyone listed** who is operating;
- if you find the controller `ARMED` or `FIRING` when you connect, **you did not put it there**: find out who did before touching anything;
- a program (such as the LabVIEW front-end) that is connected counts as someone.

### 5.5 Leaving
Press Ctrl-D in the client window. Closing your connection does not stop a shot or change the state. To close a tunnel, press Ctrl-C in its window.

---

## 6. Normal operation, step by step

Use a person's judgement at every step: **if a reply is not what this guide says, stop and ask.**
Every typed line below is one command; wait for the reply before sending the next. Replies starting `OK` are success; `ERR <number> <text>` means the controller refused it and
says why.

### 6.1 Before any shot: checks
1. Connect (section 5.3). Confirm the product and serial.
2. `STATE?` must be `OK IDLE`. If it is `OK FAULT ...`, go to section 9. If it is `ARMED` or `FIRING`, find out why before continuing.
3. `FAULT?` must be `OK 0`.
4. `EXTernal:ENAble?` must be `OK 1` (the facility permissive is being honoured). `EXTernal:INPut?` shows whether the permissive is currently high (`OK 1`) or low (`OK 0`).
   **If it is low, find out why. Do not turn the setting off to get around it** (section 11).
5. Check who else is connected (`!hub clients`).
6. After any power-up or reset, recheck the settings: **nothing you set survives a reset** unless it was stored in a profile and recalled (section 6.5).

### 6.2 Set up the shot
Do all of this in `IDLE`. Repeat the per-channel lines for each channel you will use (1–4).

```
SOURce:ENAble 1 1                 -> OK      include channel 1 (use 0 to leave a channel out)
XREX:CHANnel:ENAOut 1 1           -> OK      "enable" line to Transrex 1
XREX:CHANnel:CONTactOut 1 1       -> OK      "contactor" line to Transrex 1
PID:LOOPMODE 1 1                  -> OK      closed loop (the default)
SHOT:TIMing 5 2 5                 -> OK      ramp-up, flat-top, ramp-down seconds (example)
SHOT:CURRent 1 3000               -> OK      peak current for channel 1 in amps (example)
```

Then **read everything back** and check it against what you intended. A value can be silently limited, so the read-back is the only proof:

```
SOURce:ENAble? 1     XREX:CHANnel:ENAOut? 1     XREX:CHANnel:CONTactOut? 1
PID:LOOPMODE? 1      PID:GAINS? 1               SHOT:TIMing?       SHOT:CURRent? 1
```

- `SHOT:TIMing?` answers `ERR 12` until timing has been set since the last reset. That means "not set yet", not a fault.
- `SHOT:CURRent` is limited to 0 to the channel's configured maximum current (6000 A per channel by default) and is a **demand**, not a guarantee of the current achieved (see the calibration note in section 3).
- Do **not** change the PID gains (`PID:GAINS`) unless the controls programmer has told you to: they decide how the loop behaves.

### 6.3 Arm

```
ARM
STATE?
```

`ARM` returns `OK` and `STATE?` returns `OK ARMED`. **Nothing electrical has happened yet.** If `ARM` is refused, read the message:

| You see | What it means | What to do |
|---|---|---|
| `ERR 13 ... not currently IDLE` | already armed, firing or faulted | `STATE?` and find out why |
| `ERR 15 External enable interlock not satisfied` | the facility permissive is low | find out why; **do not disable it** |
| `ERR 13 Channel <n>'s ENA_OUT/CONTACT_OUT not both set` | an enabled channel's enable or contactor line is off | set both (`XREX:CHANnel:ENAOut`, `CONTactOut`) for that channel and `ARM` again |

Only the first problem is reported; fix it and send `ARM` again to see the next.

To stand down without firing: `DISARM` (returns to `IDLE`).

### 6.4 Fire

Confirm once more that everyone who needs to be clear is clear. Then:

```
SHOT:STARt
```

`OK` means the shot is running (`STATE?` = `OK FIRING`). Refusals:

| You see | Meaning |
|---|---|
| `ERR 13 Must ARM first` | the controller is not `ARMED` (a fault or someone else's action intervened); `STATE?` |
| `ERR 15 ...` | the external-enable permissive dropped between `ARM` and the start |
| `ERR 12 SHOT:TIMing must be set before SHOT:STARt` | no shot timing was set after the last reset |

While it runs, watch the state: `STATE?` every second or so, and for each channel `SOURce:STATus? <n>` (`OK <running> <setpointHz> <measuredHz> <outputHz>`: whether it is running and the
target, measured and actual **frequencies in hertz**, not amps). The shot **ends by itself** and `STATE?` returns `OK IDLE`; you do not have to stop it.

To **abort** a running shot from the keyboard: `SOURce:STOP`. This is an operator convenience, **not** the emergency stop (section 4.3).

### 6.5 After the shot
1. Wait for `STATE?` = `OK IDLE`. If it is `OK FAULT ...`, go to section 9.
2. If logging was armed (below), retrieve the log now.
3. Leave channels you will not use again soon enabled or disabled as agreed; send `DISARM` if you armed and did not fire.

**Waveform log (optional).** Before arming, `LOG:ARM <channel> <maxSamples> <decim>` (channel 0 = all; up to 1750 samples; `decim` = sample every Nth control tick, the control rate being 1000 per second).
After the shot, `LOG:DATA? <channel>` returns the recorded setpoint, measured and output frequencies in hertz as one long line (a few kilobytes, taking seconds to arrive). The log is held in the controller's memory
and is lost at reset: save it first.

**Stored settings (profiles).** If the controls team has set up a stored profile, `*RCL <slot>` restores the saved gains, loop modes and shot values after a reset. Show what it would change first with
`CONFig:PROFile:DIFF? <slot>`, and check that the reply to `*RCL` ends `FAILED=0`. Recall works only in `IDLE`.

---

## 7. Aborting and stopping

| Situation | Action |
|---|---|
| Armed but changed your mind | `DISARM` |
| Shot running and you want it to stop | `SOURce:STOP` (then `STATE?` must be `OK IDLE`) |
| **Emergency** | **the physical E-stop. Not a command.** |
| You are about to leave the controller armed | don't: `DISARM` first |

---

## 8. What the logs and records are

- **The hub** keeps a record of **every line, in both directions**, with a timestamp, under the controller's serial number (or its chip ID if it has none yet), one file per day. Ask the hub
  administrator for the location. The log includes which person (the name you gave your client) sent each command, and marks dangerous ones.
- **The controller** keeps an event history in memory since it last started (`SYS:EVLOG?`: state changes and faults, with times), lost at reset.
- Record the date, time, your name, the shot settings and the outcome in the operations log your facility uses; the hub's log is the cross-check, not the primary record.

---

## 9. Faults: what to do

**First, do not clear a fault just to make it go away.** A fault was raised for a reason. Find the reason first.

1. **Read it.** `STATE?` gives the type and channel (for example `OK FAULT OVERCURRENT 2`). Note it down.
2. **Wait for the ramp-down to finish** (about the fault ramp time, 1 s by default).
3. **Make sure the supplies are safe** by the facility's own means.
4. **Find the cause.** For a single channel, `XREX:CHANnel:STATus? <n>` shows the raw levels of its four fault inputs (`OK WATER=HIGH TMP=LOW ENERPRO=LOW OCP=LOW`). Read these as **levels, not good or bad**: which level means a fault depends on
   the input's polarity setting, so use `STATE?` for the verdict and these for a technician's diagnosis.
5. **Fix the cause** (water flow, temperature, Enerpro, a tripped overcurrent comparator, a fiber, the facility permissive). Remember the 120-second lockout after an overcurrent trip.
6. **Clear it, deliberately:**
   ```
   FAULT:CLEar
   STATE?
   ```
   **`FAULT:CLEar` always answers `OK`, even if it did not work.** The controller re-checks the cause when you clear; if it is still present the fault comes straight back. Only `OK IDLE` means it cleared.
   Do not repeat `FAULT:CLEar` in a loop: if it will not clear, the cause is still there.
7. **Recover:** the controller is `IDLE`. Check that each enabled channel's enable and contactor lines are still set (`XREX:CHANnel:ENAOut? <n>`, `CONTactOut? <n>`), set them again if not, then start from section 6.3.

| Fault | Likely first checks |
|---|---|
| `OVERCURRENT <n>` | Transrex *n* tripped its overcurrent protection: do not retry until the supply side has been checked; the 120 s lockout applies |
| `ENERPRO <n>` | the Enerpro firing board on Transrex *n* |
| `GENERAL` | cooling water flow or temperature switches on any unit, or the controller's hardware fault input |
| `EXTERNAL_ENABLE` | the facility permissive was withdrawn; ask the facility control room why |
| `ENABLE_OUTPUT <n>` | the enable or contactor line to Transrex *n* was turned off by a command, or lost |

**On the bench** (a controller with nothing connected to its fault inputs) the controller sits in `FAULT OVERCURRENT 1` after every reset, because the unconnected overcurrent input reads as a fault. This is normal on the bench and never
on a real system.

**After a controller reset, a power cut or a firmware update** the controller prints a three-line `!BOOT` banner. Everything you configured is gone. Check the state, then redo section 6.2 (or recall a profile), and note in the
operations log that a reset happened, why (`DIAGnostic:RSTCause?`), and who was there.

---

## 10. If the connection fails

| Symptom | Likely cause | What to do |
|---|---|---|
| Client says `Connection refused` | the tunnel is not running, or the hub is down | start the tunnel (section 5.2); if it persists, ask the hub administrator |
| Client connects and is closed at once | your computer's address is not on the hub's allowed list | ask the hub administrator to add it |
| Replies show `ERR hub timeout` | the controller did not answer in time; the command **may or may not** have run | read back with a query before doing anything else; never resend a command that starts output |
| `ERR hub the controller is not connected` | the hub has lost the controller (cable, power, serial bridge) | check the controller's power and network cable; `!hub status` shows `connected=0` until it returns |
| `ERR hub ... restarted ...` | the controller reset | treat as a reset (section 9, last paragraph) |
| Nothing at all, and the TX/RX LEDs are dark | the controller may be unpowered or held in reset | check power; ask the maintainer |

A shot already running keeps running on its own if you lose your connection. The connection is not the safety system.

---

## 11. Things operators must not do

| Do not | Why |
|---|---|
| Send `EXTernal:ENAble 0` on a connected system | switches off the facility permissive. It is for a bench controller with nothing attached only. It reverts to on at the next reset |
| Send `DEBUG:FAULT:BYPASS 1` | disables fault detection. Bench only |
| Send `OCP:TEST:FAULT` or `GENERAL:TEST:FAULT` | these inject faults on purpose, for testing |
| Send `FWUPdate:*` or `BOOT` | reflash or reset the controller. Maintainer only, with the controller idle and the supplies not in use |
| Send `SYS:SERial`, `SYS:LABel`, `SYS:ID:CLEar` | change the board's permanent identity |
| Change `CONFig:...` settings (limits, polarity, ramp times, slew) | they change protection behaviour; commissioning only |
| Change `PID:GAINS` | tunes the control loop; the controls programmer only |
| Use `SIM:*` commands | simulator only |
| Use the `TABle:*` commands or `FIRE` | a bench-only legacy mode |
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
| Facility control room / who owns the permissive | |
| Location of the hub's logs | |
| Operations log location | |
| Where the physical E-stop is, and who tests it | |

| Controller channel | Transrex unit | Coil it feeds | Nickname set? | Notes |
|---|---|---|---|---|
| 1 | | | | |
| 2 | | | | |
| 3 | | | | |
| 4 | | | | |

---

## 13. What has and has not been verified

| Item | Status |
|---|---|
| State machine (`ARM` / fire / auto-stop / `DISARM`), fault latching and clearing, per-channel gating | verified on the bench controller and the simulator |
| Closed-loop current control | confirmed converging on a **bench loopback** and against the Transrex **simulator** board; **[not yet verified against the real supplies]**. Gains in use are bench values |
| Amps-to-demand calibration | **a linear placeholder**, pending characterization against the real supplies |
| Fault ramp-down, overcurrent derating | verified on the bench with injected faults and a logic analyzer |
| Water, temperature, Enerpro and overcurrent inputs with **real** signals | **[not yet verified]**: tested only with inputs floating or driven from the simulator; the default polarity (healthy = HIGH) has not been checked against real signals |
| Refusal of `ARM` by an enabled channel with its enable/contactor lines off; fault on losing them | **[not yet verified]** on a real enabled channel (bench channels trip on their unconnected inputs) |
| E-stop path | hardware; **[no test record in this repository]** |
| External trigger and external enable | verified by a loopback test on the bench; the facility wiring is unverified |

When in doubt about a behaviour, treat it as unverified and ask the controls team.

---

## 14. Quick reference

| I want to... | Type |
|---|---|
| see what the controller is doing | `STATE?` |
| check for a fault | `FAULT?` |
| include / leave out a channel | `SOURce:ENAble <n> 1` / `0` |
| set a channel's enable and contactor lines | `XREX:CHANnel:ENAOut <n> 1`, `XREX:CHANnel:CONTactOut <n> 1` |
| set the shot timing | `SHOT:TIMing <up> <flat> <down>` (seconds) |
| set a channel's peak current | `SHOT:CURRent <n> <amps>` |
| read back a setting | the same command with `?` and no value |
| arm / stand down | `ARM` / `DISARM` |
| fire | `SHOT:STARt` |
| stop a running shot (not the E-stop) | `SOURce:STOP` |
| watch a channel | `SOURce:STATus? <n>` |
| clear a fault (check the result) | `FAULT:CLEar`, then `STATE?` |
| see who is connected | `!hub clients` |
| restore saved settings | `CONFig:PROFile:DIFF? <slot>`, then `*RCL <slot>` |
| leave | Ctrl-D |

**Glossary.** *Channel*: one of the four control paths, each to one Transrex. *Closed loop*: the controller measures the output and corrects it. *Demand*: the signal that tells a Transrex how hard to drive. *Enerpro*:
the SCR firing board in each Transrex. *Hub*: the server program that shares the controller among people and logs everything. *Interlock / permissive*: a signal that must be present for output to be allowed.
*OCP*: overcurrent protection. *Shot*: one programmed cycle of ramp-up, flat-top and ramp-down. *Latched*: stays set until cleared on purpose. *Transrex*: one of the four ISR-2126 magnet supplies.
