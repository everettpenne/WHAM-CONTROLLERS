# Limiter: worked example shots

Five worked examples for the **Limiter** (the HBRIDGE controller in `LIMITER` mode: PWM carrier 75 – 125 kHz, duty 5 – 95 % per phase). Read them next to
`docs/operators_guide.md` (sections 5 and 6) and `docs/command_reference.md`. Every command below is in those documents; the replies shown are the controller's real reply
formats.

| # | Example | You will see |
|---|---|---|
| 1 | [From a reboot to the first shot](#1-from-a-reboot-to-the-first-shot-open-loop) | what is empty after a reset, and the whole sequence |
| 2 | [Reprogram the shot after one has finished](#2-reprogram-the-shot-after-another-has-finished) | what survives a shot, how to run it again, how to replace it |
| 3 | [Open loop, with PFM input logging](#3-open-loop-with-pfm-input-logging) | the carrier frequency (3a) and the **duty cycle** (3b) measured on PFM_Input_1..3 |
| 4 | [Closed loop, with input logging](#4-closed-loop-with-input-logging) | target, measured feedback and output logged by the controller |
| 5 | [Pull the data and plot it](#5-pull-the-shot-data-and-plot-it) | CSV files and a plot, with `pull_and_plot.py` |

> **These examples ARM and FIRE the controller.** Do them only on a Limiter that is yours to run, with nothing connected to its outputs that a shot could hurt. A bench board with only
> measurement fibres on it is the intended target. Everything in section 5 is read-only.

## 0. Before you start

**Connect.** From outside the lab, open the ssh tunnel (**Open tunnel** on the operator console's start screen, or `ssh -N -L 5102:127.0.0.1:5102 andrew`); the Limiter's hub is then
`127.0.0.1:5102`. Type commands in the operator console's **Console** tab (connect through the host), or in a terminal:

```bash
python3 host/net_client.py --port 5102 --name <your-name>
```

In the examples `>` is a line you send and `<` the reply. Send **one command at a time and read the reply** before the next; a reply that is not `OK ...` means stop and read it
(`ERR <n> <reason>`).

**Check you have the right controller and that it is free:**

```
> *IDN?
< OK WHAM-HBRIDGE-PFMG474 REVA v0.11 7577bf7d SN=WB-0002-6 UID=0055002B4232500220343558     <- the Limiter is WB-0002-6, label LMTR-CTRL (SYS:ID?)
> !hub clients                  who else is connected: agree with them before you arm anything
> STATE?
< OK IDLE                       must be IDLE. ARMED/FIRING that you did not cause: find out who did it first
> FAULT?
< OK 0
```

**Five rules the examples rely on**

1. **Every shot needs its own `ARM`.** `ARM` -> `FIRE` -> the shot ends by itself -> `IDLE`. To run again, `ARM` again.
2. **Nothing survives a reset** unless it was saved in a profile and recalled (`*SAV`/`*RCL`, examples 1 and 2 show where). The supply mode is `NONE` after every boot.
3. **`ARM` is the last deliberate step.** The external trigger (a rising edge on PF15) is **on at every boot** and starts the shot of an ARMED controller exactly as `FIRE` does. Do not
   leave the controller armed; `DISARM` if you change your mind (`EXTernal:TRIGger 0` turns the trigger off for the session).
4. **`FIRE` follows `PID:LOOPMODE`.** If any enabled phase is closed loop, `FIRE` runs the closed-loop target for it instead of (or mixed with) the shot. Examples 1-3 set all three loops open
   (`PID:LOOPMODE 0 0`) first so there is no doubt; example 4 closes one and puts it back.
5. **Limits:** frequency 75 000 – 125 000 Hz, duty 5 – 95 %, times in **microseconds**, a shot at most 1 s (1 000 000 µs) and at most 500 setpoints.

---

## 1. From a reboot to the first shot (open loop)

**What a reboot looks like.** There is no reset command (`BOOT` enters the ROM bootloader, which is not a reboot): power-cycle the board, or a firmware update reboots it. A client that is
connected through the hub sees the controller announce itself, three lines, the first being the one to read:

```
< !BOOT WHAM-HBRIDGE-PFMG474 v0.11 7577bf7d BANK=1 BFB2=0 STATE=IDLE tick=0
< !BOOT diag boot=1 prev: stage=8 fault=0@0 ... rst=BOR,
< !BOOT Rise and shine, controller's awake and ready to work
```

`STATE=IDLE` is what you want (`STATE=FAULT` means a fault was already present at boot: `FAULT:SOURces?` says which, operators guide section 9). A hub marks a new "boot epoch" in its log. Wait for the `!BOOT`
lines (or for `*IDN?` to answer) before sending anything.

**Everything is empty after the reboot.** Confirm it rather than assume it:

```
> CONFig:MODE?
< OK NONE                       no supply selected: no shot can be uploaded yet
> SHOT?
< OK 0 0 PROFILE                no shot
> PID:LOOPMODE? 0
< OK 0 0 0                      all three loops open (this is what you want here)
> PHASe:ENAble?
< OK U=1 V=1 W=1
```

**Choose the supply and the phases (IDLE only)**

```
> CONFig:MODE LIMiter
< OK
> CONFig:LIMits?
< OK LIMITER FREQ 75000 125000 DUTY 5 95          check these are the limits you expect
> PHASe:SELect UVW
< OK                            exactly these phases drive
> PID:LOOPMODE 0 0              (only needed if a previous session left a loop closed: see rule 4)
< OK
```

**Enter the shot: 100 kHz, duty 20 % rising to 80 % over 10 ms, the same on all three phases**

```
> SHOT:BEGin
< OK
> SHOT:STEP 0 100000 20          t_us freq_hz duty_pct   (the first setpoint is always t = 0)
< OK
> SHOT:STEP 10000 100000 80      at 10 ms
< OK
> SHOT:END
< OK 2 10000                     2 setpoints, 10 000 µs long
```

**Read it back from the controller** (what it holds is the proof, not what you sent):

```
> SHOT?
< OK 2 10000 PROFILE
> SHOT:POINt? 0
< OK 0 100000 20
> SHOT:POINt? 1
< OK 10000 100000 80
```

**Arm, fire, wait for the end, check**

```
> ARM
< OK                             nothing electrical has happened yet
> STATE?
< OK ARMED
> FIRE                           confirm everyone who must be clear is clear
< OK
> STATE?                         poll every 50 - 100 ms
< OK FIRING
> STATE?
< OK IDLE                        the shot ended by itself (a 10 ms shot is over within about 0.1 s)
> FAULT?
< OK 0
> FAULT:SOURces?
< OK PC10=0 GATEDRV=0 TABLE=0 TEST=0 OVP=0 FB=0
```

**Keep this setup for next time (optional):** `*SAV 3 limiter-100k` stores the mode, the shot and the settings in slot 3 of the controller's flash (IDLE only). **Look at `CONFig:PROFile:CATalog?` first and choose an empty slot: saving
overwrites** (on 2026-11-11 the Limiter's slots 1 and 2 held other people's profiles, `bench` and `regress`). After the next reboot,
`CONFig:PROFile:DIFF? 3` shows what a recall would change, `*RCL 3` applies it (`OK APPLIED=<n> FAILED=0`: **`FAILED` must be 0**), then check `PHASe:ENAble?` (the phase selection is not
saved) and carry on from `ARM`. Nothing is recalled automatically at boot.

---

## 2. Reprogram the shot after another has finished

Start from the end of example 1: the shot has run and the controller is back in `IDLE`.

**What survived the shot.** The mode, the phase selection and the shot itself are still there. Confirm it:

```
> STATE?
< OK IDLE
> CONFig:MODE?
< OK LIMITER
> SHOT?
< OK 2 10000 PROFILE             the 20 -> 80 % shot is still loaded
```

**Run the same shot again:** just `ARM`, `FIRE` (rule 1). Nothing needs to be uploaded again.

**Replace it with a different shot** (IDLE only; refused while FIRING). Here a 50 ms shot in which the carrier ramps from 90 kHz to 110 kHz while the duty ramps 30 % -> 70 %, and then
a per-phase profile (U up, V down, W flat) - the five-value form gives each phase its own duty:

```
> SHOT:BEGin                     discards the previous shot at once; from here on SHOT? is not the old shot
< OK
> SHOT:STEP 0 90000 30
< OK
> SHOT:STEP 50000 110000 70
< OK
> SHOT:END
< OK 2 50000
> SHOT?
< OK 2 50000 PROFILE
```

```
> SHOT:BEGin
< OK
> SHOT:STEP 0     100000 20 80 50          t_us freq_hz duty_U duty_V duty_W
< OK
> SHOT:STEP 10000 100000 80 20 50
< OK
> SHOT:END
< OK 2 10000
> SHOT:POINt? 0
< OK 0 100000 20 80 50           all three phases have their own duty, so all three are shown
> ARM
< OK
> FIRE
< OK
```

**If a line is refused** (`ERR 10` a frequency or duty outside the Limiter's limits, `ERR 4` times not strictly increasing, `ERR 3` too many setpoints, `ERR 12 not a number`): the shot you were
building is incomplete. **Do not `ARM`.** Send `SHOT:BEGin` and start the shot again; only a `SHOT:END` that answered `OK <setpoints> <µs>` means the shot is whole.

**Other ways a shot goes away:** `SHOT:CLEar` drops it; changing the supply mode (`CONFig:MODE HVPS`) drops it, a reset drops it, and `*RCL` replaces it with the saved one. A frequency ramp that changes
the duty every period at the top of the range (125 kHz, 5 % -> 95 % in 10 ms) is the one case the shot generator cannot keep up with: it stops the shot and latches a fault (`FAULT GENERAL`,
`TABLE=1`, cleared by `FAULT:CLEAR`); see `docs/command_reference.md` ("What the CPU can sustain").

---

## 3. Open loop, with PFM input logging

The controller can **measure its own outputs**: with a phase output fibred back to a PFM_Input (on the bench: U, V, W to **PFM_Input_1, 2, 3**), the controller records the time of every
switching period of the shot. This is *logging of the PFM inputs*, and it works in open loop (no feedback needed). It tells you what the outputs really did (carrier frequency, ramps, missed
pulses), not what a power stage would do.

**How it works, in four lines.** `PFMIN:CAPTURE <M>` only *arms* a capture of up to `M` switching periods per input (1 – 3800). The recording starts at `FIRE`/the trigger, at the same moment as
the outputs, and stops at the end of the shot or after `M` periods. The result stays in the controller's memory until the next run starts or a reset. Inputs 1 – 4 are active; the others are never touched.

### 3a. Carrier frequency (rising edges, the default)

**The shot** is the per-phase profile from `examples/limiter_per_phase_duty_shot/`: 100 kHz, 10 ms, U 20 -> 50 -> 80 %, V 80 -> 50 -> 20 %, W 50 -> 90 -> 50 %.

```
> CONFig:MODE LIMiter
< OK
> PHASe:SELect UVW
< OK
> PID:LOOPMODE 0 0
< OK
> SHOT:BEGin
< OK
> SHOT:STEP 0 100000 20 80 50
< OK
> SHOT:STEP 5000 100000 50 50 90
< OK
> SHOT:STEP 10000 100000 80 20 50
< OK
> SHOT:END
< OK 3 10000
> PFMIN:EDGes?                   RISING only (period capture) is the default
< OK R R R R R R
> PFMIN:CAPTURE 1005             10 ms at 100 kHz is 1000 periods; 1005 leaves a margin. Arms inputs 1 - 4
< OK
> ARM
< OK
> FIRE
< OK
  ... poll STATE? until OK IDLE ...
> PFMIN:STATus?                  periods captured on each of inputs 1 .. 6
< OK 1000 1000 1000 0 0 0        about 1000 on the three wired inputs; 0 on an input with nothing wired to it
```

(`PFMIN:STATus?` counts periods in this default mode. The counts are what the controller captured, so they may differ by one or two from 1000; an input that reads `0` after a shot had nothing wired to it, or was not armed. An unwired input that
floats can show a few hundred bogus periods: judge by the three that are wired.)

**Read it back** with `PFMIN:DATA? <input> <start> <count>` - one page of at most 100 periods per request, each period in ticks of the 170 MHz capture clock, with a check sum - or let
`pull_and_plot.py pfmin` (example 5) do it. By hand, one page:

```
> PFMIN:DATA? 1 0 5
< OK 1000 OVERCAP=0 SUM=8495 1699 1699 1699 1699 1699
```

`OK <periods in total> OVERCAP=<n> SUM=<sum of the numbers that follow> <period> <period> ...`. `1699` ticks is `170 000 000 / 1699 = 100.06 kHz`: the capture reads one tick
short of the programmed 1700, a constant of this measurement, not a frequency error. `OVERCAP` should be `0`; above 0 means some periods may not be trustworthy.

### 3b. Measuring the duty cycle (both edges)

The default capture is **rising edge to rising edge**: it gives each period, so the carrier frequency, but **not the duty**. The duty is the time the output is high divided by the period, so the
input must record the **falling** edge too. That is a per-input setting, **`PFMIN:EDGes <input> BOTH`**, and it is what you want for the Limiter. Same shot as above, three extra steps:

1. **Before arming**, switch each wired input to both-edge mode (IDLE only; it takes effect at the next `PFMIN:CAPTURE`). A both-edge capture holds two edges per cycle, so it
   fills twice as fast: `M` is still counted in switching periods, at most 3800 (38 ms at 100 kHz).
2. Arm and fire as before.
3. Read the **raw edges** with `PFMIN:RAW?` (not `PFMIN:DATA?`, which answers `ERR 13` for a both-edge input) and decode them into cycles.

```
> PFMIN:EDGes 1 BOTH             phase U's input (2 for V, 3 for W: send one line per input)
< OK
> PFMIN:EDGes 2 BOTH
< OK
> PFMIN:EDGes 3 BOTH
< OK
> PFMIN:EDGes?
< OK B B B R R R                 B = both edges, R = rising only (inputs 1 .. 6)
> PFMIN:CAPTURE 1005
< OK
> ARM
< OK
> FIRE
< OK
  ... poll STATE? until OK IDLE ...
> PFMIN:STATus?                  now EDGES per input: two per cycle
< OK 2001 2001 2001 0 0 0
> PFMIN:RAW? 1 0 5
< OK 2001 OVERCAP=0 LEVEL=0 SUM=<sum> <t0> <d1> <d2> <d3> <d4>
```

`PFMIN:RAW? <input> <start> <count>` returns `OK <edges> OVERCAP=<k> LEVEL=<0|1> SUM=<check> <t0> <d1> <d2> ...`: `t0` is the first edge's raw timer value, each following number is the **tick
difference (170 MHz) to the previous edge**, `LEVEL` is the pin level just before the capture (so the first edge is the opposite transition and edges alternate high/low), and `SUM` is the check sum of the page. Pages
of 100, as for `PFMIN:DATA?`. One cycle is *rising edge, falling edge, next rising edge*: **period** = rising to rising, **high time** = rising to falling, **duty = 100 x high / period**. With the
numbers above, a cycle with differences `340, 1360` has period 1700 ticks (100.0 kHz) and duty 340/1700 = 20.0 %.

**Let the tool do the decoding and the plot** (read-only; see 5b):

```bash
python3 firmware/hbridge/docs/examples/limiter_example_shots/pull_and_plot.py duty --port 5102 --inputs 1,2,3 \
    --programmed "1=0:20,5000:50,10000:80" --programmed "2=0:80,5000:50,10000:20" --programmed "3=0:50,5000:90,10000:50" \
    --out my_duty_shot
```

`--programmed "<input>=<time_us>:<duty>,..."` overlays what you programmed (straight lines between the setpoints) and prints the measured-minus-programmed difference. It writes `duty_input<n>.csv` (every cycle:
`period_us, high_us, duty_pct, frequency_Hz, flag`) and `duty.png` (duty and carrier of every cycle against time).

**What to expect, from the real run in `examples/limiter_per_phase_duty_shot/`** (this same shot): the measured duty sits a **constant ~2 percentage points below** the programmed duty on every phase (the
dead time the hardware inserts, about 200 ns on a 10 us period; not separately confirmed), with a spread of a few hundredths of a point, and the carrier reads 100.06 kHz (one capture tick short of the programmed period). The
first cycle is the start-up cycle and is flagged; so is any cycle whose period is far from the median (an edge missing from the record), which the tool leaves out of its statistics. The decoding
logic is the one `run_example.py` used on that run; this tool was tested only on synthetic edges and has not yet been run on a fresh capture from the real board.

**Afterwards, put the inputs back** (IDLE only) so the next period capture is as in 3a: `PFMIN:EDGes 1 RISING`, `PFMIN:EDGes 2 RISING`, `PFMIN:EDGes 3 RISING`. `PFMIN:EDGes?` should read `R R R R R R`.
(The edge mode is not saved in profiles, and a reset puts every input back to rising only.)

---

## 4. Closed loop, with input logging

In closed loop the controller reads the supply's output voltage back on an analog input (the **feedback input**), compares it with a **target** you give it as a time series, and moves the duty
to follow it. The controller can **log** the target, the measured feedback and its output as it goes (`LOG:ARM` / `LOG:DATA?`). Phases are the loops: **1 = U, 2 = V, 3 = W**.

> **Closed loop on a real analog signal is not yet verified on this hardware** (operators guide section 13): the board's analog output-to-input path read zero when tried, and the gains in
> the code are bench values. This example therefore uses the firmware's own **simulated plant** (`CONFig:FEEDback:SOURce SIM`): the controller answers its own commands through a first-order
> lag, so it runs with nothing wired and shows the whole chain - target, loop, log - working. Do not use these numbers on a real supply: the controls programmer gives you the calibration and
> the gains for that.

**Setup for one closed phase (U), against the simulated plant**

```
> CONFig:MODE LIMiter
< OK
> PHASe:SELect U                 only phase U takes part; V and W do not switch and their loops do not run
< OK
> CONFig:FEEDback:SOURce SIM     the simulated plant instead of the real ADC input
< OK
> CONFig:MAXVolts 1000           overvoltage limit in process volts (the default, 4 V, would stop a 400 V target at once)
< OK
> CONFig:CLOSEDloop:VSCALE 900   the plant model: volts at full swing (|2D-1| = 1); here a 900 V capacitor bank
< OK
> PID:GAINS 1 0.167 6.667 0      kp ki kd for loop 1, % duty per volt (illustrative; 900 V range)
< OK
> PID:LOOPMODE 1 1               close loop 1 (U)
< OK
> PID:LOOPMODE? 0
< OK 1 0 0
```

**The target:** 0 V rising to 400 V over 15 ms, then held to 30 ms. Process volts; keep targets inside about ±0.8 × VSCALE (here ±720 V) or the duty saturates.

```
> TARGet:BEGin
< OK
> TARGet:STEP 0 0
< OK
> TARGet:STEP 15000 400
< OK
> TARGet:STEP 30000 400
< OK
> TARGet:END
< OK 3 30000                     3 setpoints, a 30 ms run (the run lasts as long as the target when every enabled phase is closed)
```

**Log the run.** `LOG:ARM <loop> <maxSamples> <decimation>`: one sample every `decimation`-th control tick (the loop ticks at `CONFig:PIDRate`, 10 000 per second), up to 1000
samples for one loop (333 per loop when logging all three with loop 0). 300 samples at decimation 1 cover 30 ms:

```
> LOG:ARM 1 300 1
< OK
> ARM
< OK
> FIRE
< OK
  ... poll STATE? until OK IDLE ...
> PID:FAULT?                     which loop stopped the run and why
< OK 0 NONE                      anything else names the phase, e.g. OK 1 CLIPPED_LOW
> FAULT?
< OK 0
> SOURce:STATus? 1               running, target, measured, output of loop 1 (after the run: the last values)
```

**What was logged** is read with `LOG:DATA? <loop> <start> <count>` - at most 40 samples per reply, with a check sum, so a 300-sample log is 8 requests:

```
> LOG:DATA? 1 0 3
< OK 300 10000 SUM=158069 0 0 50000  2667 0 50022  5333 3 50044
```

`OK <samples> <log rate per second> SUM=<check> ` then three numbers per sample: **target (millivolts), measured feedback (millivolts), output (milli-percent duty for the Limiter)**.
Sample *i* is at `i / rate` seconds. (The values shown are only the layout; yours will differ.) Plotting is example 5: `pull_and_plot.py log`.

**Put the loop back** so the next open-loop shot is not hijacked (rule 4): `PID:LOOPMODE 0 0`, `PHASe:SELect UVW`, and `CONFig:FEEDback:SOURce ADC` if the board is to be used with
real feedback. `*RCL` of a profile saved before this example restores the others.

**4b. The same run, also logging the real output with the PFM inputs.** The log above is the controller's *view* (target and feedback in its own units). To also record what its output pin really
did, fibre phase U to PFM_Input_1 as in example 3, switch input 1 to both-edge mode, keep the run within the capture's size (at most 3800 periods per input: 30 ms at 100 kHz is 3000), and arm
both captures before `ARM`:

```
> PFMIN:EDGes 1 BOTH
< OK
> PFMIN:CAPTURE 3050
< OK
> LOG:ARM 1 300 1
< OK
> ARM
< OK
> FIRE
< OK
```

Then read the log (example 5) and `PFMIN:RAW? 1 <start> 100` with `read_raw()`/`decode_edges()` from `run_example.py`: the measured duty of every cycle against the logged output. The closed-loop
start calls the same capture hook as the open-loop start, but a closed-loop run **with** a PFM capture has not been run on the bench for this document: if input 1 reports 0 periods afterwards, say so.
`CONFig:FEEDback:SOURce LOOP` (the controller measuring its own phase U on PFM_Input_1 to build the feedback) needs input 1 for itself and refuses (`ERR 13`) while a PFMIN capture owns it.

---

## 5. Pull the shot data and plot it

The data is in the controller's memory: **read it after the shot ended (`STATE?` = `OK IDLE`) and before you `ARM` again or reset**. Three ways, from easiest.

### 5a. The operator console (closed-loop runs)

On the controller's page tick **Record data for the next shot** *before* `ARM`, run the shot, and the page reads the log back, saves it (`host/operator_ui/shots/<serial>/<date-time>/`: `shot.csv`,
`report.html`, `report.md`, `shot.json`) and shows the plots and statistics in the **Shot data** tab. Limiter limitation: it records **closed-loop** runs (it uses `LOG:ARM`); an open-loop shot
leaves nothing for it to read. For open-loop measurements use 5b.

### 5b. `pull_and_plot.py` (this folder)

A read-only script: it sends only queries, refuses anything else (it cannot arm, fire or change a setting), checks every page against its `SUM=` and reads a damaged page again.

```bash
# the closed-loop log of example 4 (target, measured input, output) for loop 1, 2, 3 (default: all that hold data)
python3 firmware/hbridge/docs/examples/limiter_example_shots/pull_and_plot.py log --port 5102 --loops 1 --out my_closed_loop_shot

# the PFM_Input period capture of example 3a for inputs 1, 2 and 3 (phases U, V, W): carrier frequency
python3 firmware/hbridge/docs/examples/limiter_example_shots/pull_and_plot.py pfmin --port 5102 --inputs 1,2,3 --out my_open_loop_shot

# the both-edge capture of example 3b: the duty of every switching cycle (inputs must be in BOTH mode)
python3 firmware/hbridge/docs/examples/limiter_example_shots/pull_and_plot.py duty --port 5102 --inputs 1,2,3 --out my_duty_shot
```

`--port` is the hub's (tunnel) port; `--out` defaults to a new `pulled_<time>` folder next to the script. It needs `numpy` and `matplotlib`. What it writes:

| File | Contents |
|---|---|
| `log_loop<n>.csv` | `t_s, target_V, measured_V, output_duty_pct` for loop *n* (example 4) |
| `log.png` | three panels: target and measured feedback; the error (measured - target); the output duty |
| `pfmin_input<n>.csv` | `period_index, t_end_s, period_ticks, frequency_Hz` for input *n* (example 3) |
| `pfmin.png` | the carrier frequency of each input against time |
| `duty_input<n>.csv` | `cycle, t_s, period_us, high_us, duty_pct, frequency_Hz, flag` for input *n* (example 3b) |
| `duty.png` | the duty of every cycle (with the programmed duty if you gave `--programmed`) and the carrier, against time |
| `transcript.txt` | every line sent and received, with the time |

It prints the numbers too: for the log, samples, duration, and the rms and maximum error against the target; for the capture, periods, duration, and the mean, minimum and maximum carrier
frequency per input with `OVERCAP`. For example 3 you should see about 1000 periods per phase over about 10 ms and a mean carrier of about 100.06 kHz.

### 5c. By hand, for any plotting program

```
> PFMIN:STATus?                              how many periods each input holds
> PFMIN:DATA? 1 0 100                        then 100 100, 1 200 100, ... until start >= the total
> LOG:DATA? 1 0 40                           then 1 40 40, 1 80 40, ... until start >= the total (<samples>)
```

Add the numbers of each page and compare with its `SUM=` (modulo 2^32); a reply that does not match lost a character on the serial link - ask again. For `PFMIN:DATA?`, frequency =
170 000 000 / period (ticks). For `LOG:DATA?`, divide the three numbers by 1000 for volts and percent.

---

## If something is not as shown

| You see | Meaning |
|---|---|
| `ERR 13 No supply mode selected` on `SHOT:BEGin`/`STEP` | after a reboot the mode is `NONE`: `CONFig:MODE LIMiter` (example 1) |
| `ARM` -> `ERR 6` | a fault is latched: `FAULT:SOURces?`, operators guide section 9 |
| `ARM` -> `ERR 13` | not IDLE (already armed, firing or faulted): `STATE?` |
| `FIRE` -> `ERR 5` | nothing to play: no whole shot, or an open phase has no shot, or a closed phase has no target |
| the run does something other than the shot | a loop is still closed: `PID:LOOPMODE? 0` (rule 4) |
| `ERR 10 ...` while uploading | a frequency or duty outside the Limiter's limits (it names the phase) |
| a closed-loop `FIRE` fails at once or `PID:FAULT?` names a loop | `OVERVOLTAGE`: target beyond `CONFig:MAXVolts`; `CLIPPED`/`CLIPPED_LOW`: the feedback input is stuck at an end of its range (nothing wired, wrong source) |
| `PFMIN:STATus?` reads 0 after a shot | `PFMIN:CAPTURE` was not sent before `ARM`, the input is not wired, or the shot did not run |
| `PFMIN:DATA?` -> `ERR 13 ... BOTH-edge mode` | that input is in both-edge mode: read it with `PFMIN:RAW?` (the `duty` mode of the tool), or put it back with `PFMIN:EDGes <n> RISING` |
| `PFMIN:RAW?` -> `ERR 13 ... not in BOTH-edge mode` | the input was rising-only when the capture ran: `PFMIN:EDGes <n> BOTH` **before** `PFMIN:CAPTURE`/`ARM`, then run the shot again |
| the hub answers `ERR hub ...` | the hub could not reach the controller (power, network): `!hub status` |

## What was and was not checked for this document

- **Checked on the real Limiter (WB-0002-6, v0.11, 2026-11-11):** the identification reply, the idle state and the reply format of the queries quoted above, through the hub. No shot was
  fired for this document.
- **Checked against the source and the existing references:** the commands and their arguments against `docs/command_reference.md` and the command handlers; the open-loop per-phase shot and the 100.06 kHz
  / 1699-tick reading against `examples/limiter_per_phase_duty_shot/` (a real run); the closed-loop gains and plant values against the simulator-run entries of `docs/changelog.txt`.
- **`pull_and_plot.py`** was tested against the host simulator (`log`, including the check sum) and against a fake controller that deliberately damages one reply in five (`pfmin` and `duty`, re-reads; `duty` recovered a synthetic 2-point offset to within 0.03 points); it
  was **not yet run against the real controller after a real shot**. The numbers in the `<` lines of sections 3 and 4 that depend on the shot (`SUM=`, sample values, `OK 1000 1000 1000 0 0 0`) show the
  layout, not a recorded result.
- **Not verified anywhere yet:** closed loop on real analog feedback (operators guide section 13), carriers above 100 kHz on a logic analyzer, and a closed-loop run together with a PFM_Input capture.
