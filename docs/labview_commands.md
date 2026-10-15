# LabVIEW guide, part 3: the command sets

Part 1 (`docs/labview_hub_guide.md`) is the connection and framing. Part 2 (`docs/labview_state_and_faults.md`) is the operating
state machine, faults and recovery; read it first, since it explains why most commands below refuse in some states. This part lists
the commands a front-end needs, for each product, with what each does, the argument units and ranges, and the reply format.

The complete lists, with every diagnostic and bench command, are in the two `command_reference.md` files (section 9). This chapter
lists only what a front-end should use, and says what to leave alone.

---

## 1. Conventions

| Item | Rule |
|---|---|
| Which product? | Ask `*IDN?` first; the product name is the first word after `OK`. **Use the command set that matches it** (the two products have different commands, and the same word can mean a different unit) |
| Send the long form | `SHOT:STARt`, not `SHOT:STAR` or `shot:start`. Both are accepted, but code that always sends the form written here is easier to search and review |
| Replies | `OK` or `OK <fields...>` or `ERR <code> <text>`. Fields are separated by single spaces. Decimal numbers; booleans `0`/`1` |
| Channels | numbered from **1** on the wire. XREX: channels 1-4 (confirm with `CONFig:CHANnels?`). HBRIDGE: *phases* U, V, W are **loops/channels 1, 2, 3**; `0` means "all" in the commands that say so |
| Numbers you send | the whole token must be a plain number (`12`, `-7`, `0.167`, `2.5E-2`). `1.5` where an integer is needed, `12x`, `abc`, `nan` and hex are refused with `ERR 12 not a number: ...`. Never send thousands separators, units, or a trailing `A`/`Hz`/`V` |
| Locale | **LabVIEW's number-to-string functions can use a comma as the decimal point** on some systems. Always format with an explicit period (use `%.3f`/`%g` through *Format Into String* with the *Use system decimal point* option off), never the default |
| Max line | 127 characters. Long data goes through commands built for it (upload one step per line) |
| **DANGEROUS** | in the tables below, marks a command the hub treats as dangerous (part 1, section 8). They start or enable output, arm, weaken a protection, reset or reflash |
| Persistence | **nothing you set survives a reset** unless saved to a stored profile (section 6) and recalled again afterwards |

Order of operations for any set-up: **identify, read the state, set configuration while `IDLE`, read it all back with the `?` queries and show it to the operator, ARM, then start.** Reading back is cheap and
is the only way to find out that a value was clamped (several commands clamp silently; see the notes).

---

## 2. Commands both products have

| Command | Reply | Notes |
|---|---|---|
| `*IDN?` | `OK <product> <hw rev> <firmware> <build> [SN=<serial> UID=<chip id>]` | the `SN=`/`UID=` fields are present on current firmware only; the build ends in `-dirty` if the firmware came from an uncommitted source tree. Parse by field, not by character position |
| `SYS:ID?` | `OK serial=<s> uid=<id> flashuid=<id> product=<p> hw=<r> fw=<v> commit=<c> label=<l> verified=<0\|1> identity=<ok\|unset\|corrupt\|nochip>` | parse by `key=value`; new keys can appear at the end. Older firmware answers `ERR 1` |
| `SYS:LABel?`, `SYS:SERial?` | `OK <text>` | read-only use only. **`SYS:SERial <sn>` and `SYS:LABel <text>` write the board's permanent identity. Not for a front-end** (`SYS:SERial` can be written only once) |
| `STATE?`, `FAULT?`, `FAULT:CLEar`, `ARM`, `DISARM` | see part 2 | `ARM` and `FAULT:CLEar` are marked dangerous |
| `SOURce:STOP` | `OK` | stops the output, back to `IDLE` |
| `CONFig:CHANnels?` | `OK <n>` | number of drive channels the firmware was built with |
| `GDS?` | `OK 01=HIGH 02=LOW ...` | raw gate-driver status pin levels; diagnostic only |
| `DIAGnostic:RSTCause?` | `OK ...` | why the controller last reset; `DIAGnostic:RSTCause:CLEar` forgets it |
| `DIAGnostic:LEDs?` | `OK ...` | what the front-panel LEDs are being driven to (cosmetic) |
| `*SAV`, `*RCL`, `CONFig:PROFile:*` | section 6 | stored settings |
| `PID:GAINS <ch> <kp> <ki> <kd>` / `PID:GAINS? <ch>` | `OK` / `OK <kp> <ki> <kd>` | up to 3 decimals; units are per product (below) |
| `PID:LOOPMODE <ch> <0\|1>` / `PID:LOOPMODE? <ch>` | `OK` / `OK <0\|1>` | `0` open loop, `1` closed loop; `ch` 0 = every channel (HBRIDGE: query `PID:LOOPMODE? 0` returns all three, e.g. `OK 1 0 0`) |
| `LOG:ARM`, `LOG:DATA?` | section 3.6 / 4.7 | recorded waveform of a run (different argument forms) |
| `PFMIN:...`, `PFM:...`, `QSPI:...`, `DIAGnostic:OPTBytes?`, `TABle:*`, `FIRE` (XREX) | | bench diagnostics and a legacy table mode: leave to the console tools, do not build a front-end on them |
| `FWUPdate:*`, `BOOT` | | reflash and reset. Done with the maintained update tool, **not** from a front-end. Marked dangerous |

---

## 3. XREX (Transrex magnet supply controller)

Four independent channels, one per Transrex supply. Each channel has a peak demand current; a shot ramps every enabled channel up,
holds, then ramps down, with a closed loop per channel (the controller measures each supply's output as a frequency and
corrects its drive). Units are **amps** for the demand and **hertz** for the drive and measured frequencies.

### 3.1 Readiness: the "ready to energize" lines and interlocks

| Command | Reply | Notes |
|---|---|---|
| `XREX:CHANnel:ENAOut <ch> <0\|1>` **DANGEROUS if 1** | `OK` | the channel's enable output (high = energize). Effective immediately, any state |
| `XREX:CHANnel:ENAOut? <ch>` | `OK <0\|1>` | level actually driven |
| `XREX:CHANnel:CONTactOut <ch> <0\|1>` **DANGEROUS if 1** | `OK` | the contactor output. Effective immediately, any state |
| `XREX:CHANnel:CONTactOut? <ch>` | `OK <0\|1>` | |
| `XREX:CHANnel:STATus? <ch>` | `OK WATER=HIGH TMP=LOW ENERPRO=LOW OCP=LOW` | the channel's four fault inputs as **raw levels** (`HIGH`/`LOW`, not "good"/"bad") |
| `EXTernal:ENAble <0\|1>` / `?` | `OK` / `OK <0\|1>` | the external-enable interlock (part 2, section 6). **On by default**; turning it off is dangerous |
| `EXTernal:INPut?` | `OK <0\|1>` | the interlock input level right now |
| `EXTernal:TRIGger <0\|1>` / `?`, `EXTernal:TRIGger:INPut?` | `OK` / `OK <0\|1>` | external trigger on or off, and its input level |
| `SOURce:ENAble <ch> <0\|1>` **DANGEROUS if 1** | `OK` | whether a channel participates at all: `0` disconnects its outputs entirely, ignores its fault inputs, and **takes effect live, even mid-shot**. Default on for every channel |
| `SOURce:ENAble? <ch>` | `OK <0\|1>` | |
| `CHANnel:NICKname <ch> <name>` / `?` | `OK` / `OK <name>` (`OK -` none) | cosmetic label: 1-15 characters, **no spaces**, not `-`. `ERR 14` if invalid |

`ARM` refuses unless, for every **enabled** channel, both its enable output and contactor output are HIGH (part 2, section 5).

### 3.2 Per-channel control settings

| Command | Reply | Notes |
|---|---|---|
| `PID:GAINS <ch> <kp> <ki> <kd>` | `OK` | resets that channel's integrator |
| `PID:GAINS? <ch>` | `OK <kp> <ki> <kd>` | |
| `PID:LOOPMODE <ch> <0\|1>` | `OK` | `1` = closed loop (**the default**), `0` = open loop: the profile is written straight to the output with no correction. `ch` 0 = all |
| `SOURce:SETpoint <ch> <hz>` | `OK` | target output **frequency** in Hz for a continuous run (`SOURce:RUN`); clamped to 3000-150000 Hz. **Write-only: there is no `SOURce:SETpoint?`.** Read it back from `SOURce:STATus?` |
| `CONFig:MAXCURRent <ch> <amps>` / `CONFig:MAXCURRent? <ch>` | `OK` | per-channel full-scale calibration: the current at which the drive reaches its maximum frequency. Must be > 0. A commissioning setting, not an operator one |

### 3.3 Shot definition

A shot is a trapezoid, the same for every channel, scaled by each channel's own peak current:

```
SHOT:TIMing <rampUpS> <flatTopS> <rampDownS>      seconds, each > 0, shared by all channels
SHOT:CURRent <ch> <amps>                          peak demand for that channel, clamped to 0..6000 A
```

| Command | Reply | Notes |
|---|---|---|
| `SHOT:TIMing <up> <flat> <down>` | `OK` | all three > 0 or `ERR 12`. **Must be set at least once after every reset**, or the start command refuses |
| `SHOT:TIMing?` | `OK <up> <flat> <down>` | `ERR 12` if never set since reset: this is "not configured", not an error in your code |
| `SHOT:CURRent <ch> <amps>` | `OK` | clamped into 0 to the configured maximum; read back to see the stored value |
| `SHOT:CURRent? <ch>` | `OK <amps>` | |
| `SOURce:RAMP <ch> <startHz> <endHz> <durationMs>` | `OK` | a bench/trajectory ramp of one channel's frequency setpoint, independent of the shot. A diagnostic: not for normal operation |

The amps-to-hertz mapping is a **linear placeholder** (turn-on frequency at 0 A, maximum frequency at full-scale current: `CONFig:TURNONHz`,
`CONFig:MAXFREQHz`, `CONFig:MAXCURRent`) pending characterization against the real supplies. Do not put physical-current claims on a display
until the supplies have been calibrated.

### 3.4 Starting and stopping

| Command | Reply | Notes |
|---|---|---|
| `SHOT:STARt` **DANGEROUS** | `OK` | starts the profiled shot on every enabled channel. Needs `ARMED`. The state goes `FIRING`, and **returns to `IDLE` by itself at the end**. Wait for `STATE?` = `OK IDLE` (or the `!EVT ... STATE IDLE`) |
| `SOURce:RUN` **DANGEROUS** | `OK` | continuous closed-loop run at each channel's `SOURce:SETpoint`. Needs `ARMED`. No end: stops only on `SOURce:STOP` or a fault. Not for normal shots |
| `SOURce:STOP` | `OK` | stops everything, back to `IDLE` |
| `DISARM` | `OK` | `ARMED` -> `IDLE` |

### 3.5 Live status

| Command | Reply | Notes |
|---|---|---|
| `SOURce:STATus? <ch>` | `OK <running> <setpointHz> <measuredHz> <outputHz>` | `running` 0/1 (the whole loop, not just this channel); then that channel's target, measured (feedback) and actual output frequency in hertz |
| `SYS:TIME?` | `OK <ms>` | milliseconds since boot; the clock the `!EVT` lines use |

### 3.6 Logging a run

```
LOG:ARM <ch> <maxSamples> <decim>       ch 0 = all channels together, maxSamples 1..1750, decim >= 1
... run a shot ...
LOG:DATA? [<ch>]                        OK <count> <rateHz> s1 m1 o1 s2 m2 o2 ...
```

- Samples are taken every `decim`-th control tick (default tick rate 1000 Hz, `CONFig:PIDRate?`), so `rateHz` = tick rate / `decim`. A shot of
  length T seconds at `decim` d needs about `T x 1000 / d` samples; `maxSamples` bounds it.
- Each sample is three numbers in hertz: setpoint, measured, output. Convert to amps on the host if needed.
- `LOG:DATA?` is **one long line** (several kilobytes, arriving over seconds) and is one of the commands the hub gives a 30-second reply time. Use a read timeout of 40 s or more, and a line buffer
  of at least 64 KB (part 1, section 3).
- With `ch` 0 in `LOG:ARM`, pass the channel to `LOG:DATA?` (`ERR 12` if omitted). All channels share the same sample times.
- Arm logging in `IDLE` or `ARMED`, before the start command. Pull the data after the shot has ended.

### 3.7 Events and history

`!EVT` lines (part 2, section 2.3) arrive without being asked for. `SYS:EVENT 0|1` / `SYS:EVENT?` switch the live stream (default on). `SYS:EVLOG?` replays the history
since boot: `OK <n>` followed by exactly `n` `!EVT` lines (part 1, section 5.3). `SYS:TELEM?` returns the event format version: read it on connect and
refuse to decode a version you do not know.

### 3.8 Calibration and limits (commissioning, not operators)

All are RAM-only (reset on every reboot unless stored in a profile); write with the value, read back with `?`. They change the behaviour of the output and of the
protections, and the hub marks most of them as dangerous: `CONFig:MAXFREQHz`, `CONFig:MAXCURRent <ch>`, `CONFig:SLEWRate` (hertz per tick), `CONFig:FaultRampTime` (seconds) and
`CONFig:FaultPolarity:WATER|TEMP|ENERPRO|OCP` (`0` = the signal is normally HIGH, so LOW is the fault; `1` = normally LOW). The others are `CONFig:PIDRate` (control
loop rate, 700-10000 Hz, refused while a shot is running; it also changes the real duration of already-set shot timing, so set it first) and `CONFig:TURNONHz`. Keep them
on a separate, access-controlled commissioning screen if you offer them at all.

### 3.9 A worked shot (XREX)

Every line is one command; read and check the reply to each before sending the next. `ch` here is 1; repeat the per-channel lines for each channel used.

```
*IDN?                          -> OK WHAM-XREX-... : XREX command set
STATE?                         -> OK IDLE             (anything else: stop and look)
CONFig:CHANnels?               -> OK 4
SOURce:ENAble 1 1              -> OK                  (only the channels you use; the others stay 0)
SOURce:ENAble 2 0                  ...
XREX:CHANnel:ENAOut 1 1        -> OK
XREX:CHANnel:CONTactOut 1 1    -> OK
PID:LOOPMODE 1 1               -> OK
PID:GAINS 1 <kp> <ki> <kd>     -> OK
SHOT:TIMing 5 2 5              -> OK                  (rampUp flatTop rampDown, seconds)
SHOT:CURRent 1 3000            -> OK                  (amps)
SHOT:TIMing?  / SHOT:CURRent? 1 / PID:GAINS? 1 / SOURce:ENAble? 1   read back and display
LOG:ARM 1 1750 4               -> OK                  (optional; 1000 Hz / 4 = 250 samples per second)
ARM                            -> OK  (or ERR 15 / ERR 13: show the text, stop)
STATE?                         -> OK ARMED
SHOT:STARt                     -> OK
   ... poll STATE? and SOURce:STATus? 1 a few times a second until STATE? = OK IDLE ...
LOG:DATA? 1                    -> OK <count> <rateHz> ...   (long line)
```

If at any point the state becomes `FAULT`, stop the sequence and follow part 2, section 4.

---

## 4. HBRIDGE (three-phase H-bridge: Limiter and HVPS)

One controller, two supplies selected at run time by the **supply mode**. Three phases: **U, V, W = channels 1, 2, 3**. Each phase can run **open loop**
(it plays the uploaded shot: a time series of frequency and duty) or **closed loop** (its own controller holds that phase's measured voltage to a target series using an analog feedback
input), and phases can be mixed in one run.

### 4.1 Supply mode and limits

| Mode | Drive | Carrier frequency | Duty (per phase) |
|---|---|---|---|
| `LIMITER` (`LIM`) | PWM | 75-125 kHz | 5-95 % |
| `HVPS` | PFM | 10-30 kHz | 40-60 % |

| Command | Reply | Notes |
|---|---|---|
| `CONFig:MODE LIMiter\|HVPS` | `OK` | `IDLE` only. **A mode change discards the uploaded shot** and any table checked against the old limits. Boot state is `NONE`: nothing can be uploaded until a mode is chosen (`ERR 13`) |
| `CONFig:MODE?` | `OK NONE\|LIMITER\|HVPS` | |
| `CONFig:LIMits?` | `OK <mode> FREQ <min> <max> DUTY <min> <max>` | the limits in force (Hz, percent) |
| `PHASe:ENAble <phases> <0\|1>` **DANGEROUS if 1** | `OK` | `IDLE` only. `<phases>` = letters (`U`, `UW`), channel numbers (`1`, `13`) or `ALL`; the others are left as they are. A disabled phase is never connected to its output pins. All three are enabled at every boot; `ARM` refuses if none is |
| `PHASe:SELect <phases\|NONE>` **DANGEROUS** | `OK` | exactly these phases on and the rest off |
| `PHASe:ENAble?` / `PHASe:SELect?` | `OK U=1 V=0 W=1` / `OK UW` | the phase choice is **not** part of a stored profile, by design (a recall never changes which legs of the bridge are driven) |

### 4.2 Open-loop shot: frequency and duty over time

A shot is a time series of setpoints; the controller ramps frequency and duty **linearly in time** between them, one switching period at a time. Up to 500
setpoints and 1 second long. Everything is checked against the mode's limits **at upload**.

```
SHOT:BEGin                              -> OK        discard the previous shot
SHOT:STEP <t_us> <freq_hz> <duty_pct>   -> OK        first step must be t_us = 0
SHOT:STEP <t_us> <freq_hz> <dU> <dV> <dW> -> OK      per-phase duties (carrier and time stay shared)
SHOT:END                                -> OK <setpoints> <duration_us>
```

| Rule | Detail |
|---|---|
| Units | time in **integer microseconds**; frequency in Hz (up to 3 decimals); duty in **percent** (up to 2 decimals) |
| First setpoint | exactly `t = 0`; times strictly increasing; at least two setpoints; the last setpoint's time is the shot length (<= 1 s) |
| The two forms | the one-duty form applies to all three phases; the three-duty form gives each phase its own duty; the forms can be mixed from one step to the next |
| Errors | `ERR 10` frequency or duty outside the mode (the text names the phase for a per-phase duty); `ERR 4` ordering or format error, or a time beyond the longest shot; `ERR 3` full; `ERR 13` no mode selected, or the controller is `FIRING` |
| Shot end | the shot ends at the first period that starts at or after the last setpoint's time, so it overshoots by less than one period; the last setpoint's value is approached, not held |
| Query | `SHOT?` -> `OK <setpoints> <duration_us> PROFILE\|TABLE`; `SHOT:POINt? <i>` -> `OK <t_us> <freq_hz> <duty>` (or three duties if the phases differ); `SHOT:CLEar` drops it |

Upload one `SHOT:STEP` per line and check each reply. Do not stream hundreds of steps without reading the replies: a refused step (`ERR 10`, `ERR 4`) would
otherwise be found only at `SHOT:END` or, worse, never.

### 4.3 Start, state and stop

```
CONFig:MODE HVPS ... upload ... ARM -> OK -> FIRE -> OK        (FIRE plays the uploaded shot)
```

| Command | Reply | Notes |
|---|---|---|
| `FIRE` **DANGEROUS** | `OK` | plays the open-loop shot (and runs any closed phases). Needs `ARMED`. Refusals in order: `ERR 6` fault latched, `ERR 13` not armed, `ERR 5` nothing to play. The state goes `FIRING` and returns to `IDLE` at the end of the shot. **There is no stop-in-the-middle for a shot except a fault or `SOURce:STOP`** |
| `SOURce:RUN` **DANGEROUS** | `OK` | closed-loop run holding each loop's `SOURce:SETpoint`; every enabled phase must be closed loop (`ERR 13` otherwise) |
| `SOURce:STOP` | `OK` | |
| `SOURce:SETpoint <loop> <volts>` / `?` | `OK` / `OK <volts>` | per-loop hold target for `SOURce:RUN`; loop 0 = all |
| `SOURce:STATus? <loop>` | `OK <running> <target> <measured> <output>` | an open phase shows its shot duty as the output |

### 4.4 Closed loop (per phase)

The loops share their rate, limits and plant settings and have their own mode, gains, feedback input and calibration, hold setpoint and target series. Default feedback inputs:
**U on analog input PA0, V on PA1, W on PB0**.

```
PID:LOOPMODE <loop> <0|1>             loop 1-3 (0 = all). 1 closes that phase; default open
PID:GAINS <loop> <kp> <ki> <kd>       up to 3 decimals; HVPS: Hz per volt; Limiter: percent duty per volt
TARGet:BEGin                          open a target upload
TARGet:STEP <t_us> <volts>            one voltage for every phase, or:
TARGet:STEP <t_us> <vU> <vV> <vW>     one per phase. First setpoint at t = 0; linear between setpoints
TARGet:END                            -> OK <setpoints> <duration_us>
LOG:ARM <loop> <maxSamples> <decim>   loop 0 = all (<= 333 samples when all three, <= 1000 for one)
ARM  ->  FIRE
PID:FAULT?                            which loop stopped the run, and why
LOG:DATA? <loop> <start> <count>      <= 40 samples per reply; loop through <start> to read it all
```

| Topic | Rule |
|---|---|
| Voltage unit | "process volts": the target, the measurement and `CONFig:MAXVolts` share one unit, defined by the feedback calibration. By default ADC volts; real supply volts once `CONFig:FEEDback:RANGE` or `SCALE`/`OFFSet` is set. **Never mix numbers from before and after a calibration change** |
| Limits | a `TARGet:STEP` beyond `CONFig:MAXVolts` is refused (naming the phase). An HVPS target may not be negative |
| HVPS is power | the HVPS feedback voltage stands in for power: highest at resonance (about 19 kHz), falling away on both sides; a run typically starts near 20 kHz |
| Mixed runs | closed phases are driven by their controllers, open phases by the shot, at one carrier: the shot's frequency when any enabled phase is open, otherwise `CONFig:CLOSEDloop:FREQuency`. The run lasts as long as the shot if any enabled phase follows it, otherwise as long as the target. A closed phase whose target ends earlier holds its last value |
| One loop in HVPS | the HVPS has ONE loop (loop 1): loops 2 and 3 apply to the Limiter only |
| Disabled phases | take no part: their loops do not run and their feedback inputs are not checked |
| Errors | `ERR 5` no shot for an open phase; `ERR 13` `SOURce:RUN` with an open phase; a closed-loop start also refuses if the analog reference is more than 3 % off |
| Reading `PID:FAULT?` | `OK <loop 0-3> <NONE\|OVERVOLTAGE\|CLIPPED\|CLIPPED_LOW>` (part 2, section 4.2) |

Feedback calibration commands (`CONFig:FEEDback:CHANnel|SCALE|OFFSet|RANGE|FILTer|CLIPTime|SOURce|DACRatio`), plant models
(`CONFig:CLOSEDloop:DUTY|FREQuency|RESonance|VPEAK|VSCALE|BRANCH`, `CONFig:CLOSEDloop:CURVe:*`), the control rate (`CONFig:PIDRate`), output slew (`CONFig:SLEWRate`)
and the overvoltage limit (`CONFig:MAXVolts`) are commissioning settings: set once per installation, kept in a stored profile, not on an operator screen. See `firmware/hbridge/docs/command_reference.md`, the
"Closed loop" section, before writing any of them.

### 4.5 Analog inputs and outputs

| Command | Reply | Notes |
|---|---|---|
| `ADC:INPut? <1-4>` | `OK <volts> <code>` | analog inputs 1-4 (PA0, PA1, PC0, PB0), 12-bit, averaged. An unconnected input reads near 0 V |
| `ADC:VREF?` | `OK <volts> <code> <factory code>` | measured reference; full scale of both the ADC and the DAC (nominally 2.5 V, not 3.3 V) |
| `ADC:STATus?` | `OK <running> <tickHz> <scansPerTick> <sampleX10> <scanUs> <blocks>` | the analog sampler's state: whether it is running, the control-tick rate, and its timing and block count. Diagnostic |
| `DAC:OUTPut <1\|2> <volts>` **DANGEROUS if volts > 0** | `OK` | the two analog outputs (0 to full scale); independent of the state machine, faults and `FIRE`. Up to 3 decimals |
| `DAC:OUTPut? <1\|2>` | `OK <volts> <code commanded> <code in the DAC>` | |

### 4.6 Diagnostics worth surfacing

`PID:DIAG?` -> `OK <ticks> <longest tick> <mean tick> <cycles per tick period>` (the control loop's CPU cost); `PFM:DIAG?` (last shot's timing);
`FAULT:SOURces?` and `GDS:LAST?` (the gate-driver inputs that were in fault at the last latch; part 2, section 4.4); `CONFig:PROFile:*` (section 6).

### 4.7 A worked open-loop shot (HBRIDGE, Limiter)

```
*IDN?                              -> OK WHAM-HBRIDGE-...  : HBRIDGE command set
STATE?                             -> OK IDLE
CONFig:MODE LIMiter                -> OK
PHASe:SELect UVW                   -> OK     (or PHASe:ENAble ALL 1)
SHOT:BEGin                         -> OK
SHOT:STEP 0 100000 20 50 80        -> OK     t=0, 100 kHz, duties U=20 V=50 W=80 %
SHOT:STEP 10000 100000 80 50 20    -> OK     at 10 ms: duties swapped
SHOT:END                           -> OK 2 10000
SHOT?  / CONFig:LIMits?  / PHASe:ENAble?     read back and display
ARM                                -> OK
STATE?                             -> OK ARMED
FIRE                               -> OK
   ... STATE? until OK IDLE (shot length is known: duration_us) ...
```

(Every `SHOT:STEP` is one line; the reply to each must be `OK` before the next is sent.)

---

## 5. Error codes you will see

Codes are the same in both products and never renumbered. Show the text to the operator; branch on the number.

| Code | Meaning | Typical front-end action |
|---|---|---|
| 1 | unknown command (a typo, or a command the other product has) | a programming error, or the wrong product: re-check `*IDN?` |
| 2 | `*:STEP`/`*:END` without its `*:BEGin` | restart the upload |
| 3 | an upload table is full | too many setpoints |
| 4 | an uploaded step is malformed (count, range, ordering, or beyond the longest shot) | fix the data |
| 5 | nothing to play (`FIRE`, closed-loop start) | upload first |
| 6 | **a fault is latched** | show the fault (part 2); never retry blindly |
| 7 | external flash failed | tell the maintainer |
| 10 | a frequency or duty is outside the supply mode's limits (HBRIDGE); a carrier above the maximum (XREX legacy) | fix the value |
| 11 | invalid channel or loop number | a programming error |
| 12 | wrong number of arguments, not a number, or out of range, or a precondition such as `SHOT:TIMing` not set | the text says which |
| 13 | **not allowed in the current state** (`ARM` when not `IDLE`, a start when not `ARMED`, settings while running, no mode selected, an enabled channel's outputs not ready) | read `STATE?`; do not retry |
| 14 | XREX: invalid nickname | fix the name |
| 15 | XREX: the external-enable interlock is on and its input is LOW | satisfy the interlock, or the operator decides |
| 17 | firmware update failed | not a front-end concern |
| 18 | saved profile slot empty or corrupt, or the flash failed | choose another slot |
| 19 | identity write refused | not a front-end concern |
| `ERR hub ...` | the hub, not the controller (part 1, section 7) | per that table |

---

## 6. Stored profiles (`*SAV`, `*RCL`)

Eight numbered slots (1-8) in the controller's external flash keep a snapshot of its settings, so they survive a power cycle. **Nothing is applied at boot.
You must recall explicitly after every reset.** Saving and recalling work in `IDLE` only (`ERR 13` otherwise), take a fraction of a second during which the serial link does not answer, and
use the commands below in both products.

| Command | Reply |
|---|---|
| `*SAV <slot> [label]` | `OK`. Label 1-15 characters, no spaces. Overwrites a used slot |
| `*RCL <slot>` | `OK APPLIED=<n> FAILED=<k> [<names refused>] [IGNORED=<m>]`. Every value goes through the normal limit checks; one that is refused is named and the rest still load. `FAILED` must be 0 for a clean recall; **check it** |
| `CONFig:PROFile:CATalog?` | `OK 1=<label>/<fw> 2=empty 3=corrupt ...` |
| `CONFig:PROFile? <slot>` | the saved values, nothing applied |
| `CONFig:PROFile:DIFF? <slot>` | what a recall would change, as `name=live>saved`, or `OK SAME`. Show this to the operator before a recall |
| `CONFig:PROFile:CLEar <slot>` | `OK` |

What is stored differs by product. XREX: control rates and calibration, fault polarities, shot timing, and per channel gains, loop mode, maximum current, demand current and nickname; **not**
channel enables, the external-enable setting, setpoints or any run action (the two board enable outputs `GPOut:ENAble` and `PWMAlt:ENAble` are stored). HBRIDGE: the settings *and* the shot, target and HVPS-curve tables; **not** run state, the DAC outputs, the simulation settings or the phase choice.
A profile saved by older firmware still recalls on newer firmware; `IGNORED` counts saved values the running firmware does not know.

---

## 7. What a front-end should leave alone

| Commands | Why |
|---|---|
| `FWUPdate:*`, `BOOT` | firmware management belongs to the maintained update tool, with the hub's cooperation, in `IDLE`, never during operations |
| `SYS:SERial`, `SYS:LABel`, `SYS:ID:CLEar` | the board's permanent identity |
| `DEBUG:FAULT:BYPASS` (XREX) | disables fault detection; bench only |
| `OCP:TEST:FAULT`, `GENERAL:TEST:FAULT` | inject faults on purpose; testing only |
| `SIM:*` | simulator-only: the plant models, simulated fault inputs |
| `DIAGnostic:GPOut09..12`, `GPOut:ENAble`, `PWMAlt:ENAble` | raw board outputs; commissioning only |
| `PFMIN:*`, `PFM:GAPLOG?`, `QSPI:*`, `DIAGnostic:OPTBytes?` | bench diagnostics |
| `TABle:*`, `FIRE` (XREX) | the legacy table mode, not this product's way of firing |

---

## 8. Quick checklist for a command layer

1. One function per command that formats the arguments with an explicit period, sends one line, waits for the matching reply, and returns `(ok, fields)` or `(error code, text)`.
2. A table of which product each command belongs to, filled from `*IDN?`; refuse to send a command the connected product does not have.
3. Every setter followed by its `?` getter, for display, so the operator sees what the controller **actually** holds (clamped, rounded or refused).
4. `ARM` and every start command wrapped so that the refusal text reaches the operator.
5. Upload loops that stop at the first `ERR`.
6. Long-reply commands (`LOG:DATA?`, `SYS:EVLOG?`, `CONFig:PROFile:CATalog?`) on their own, patient read path (part 1, section 5.3).
7. A "controller was reset" path (part 2, section 3) that re-recalls the profile or asks the operator.

---

## 9. Source documents

| File | Contents |
|---|---|
| `firmware/xrex/docs/command_reference.md` | every XREX command, in full detail, with the fault architecture |
| `firmware/hbridge/docs/command_reference.md` | every HBRIDGE command, in full detail |
| `firmware/xrex/docs/scpi_conventions.md` | reply grammar, error codes, argument rules shared by both |
| `firmware/xrex/docs/labview_interface.md` | the earlier XREX-only LabVIEW contract (direct serial). Superseded by parts 1-3 where they overlap |
| `firmware/xrex/docs/telemetry.md` | the event stream design |
| `firmware/hbridge/docs/saved_profiles.md`, `firmware/xrex/docs/saved_profiles_shared.md` | stored-profile storage |
