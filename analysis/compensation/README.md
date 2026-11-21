# Demand compensation tools

These tools correct a shot's programmed profile from shot to shot, so that the supply's output follows what the operator asked for when
the controller has to run open loop. Each tool is **one file**. The operator runs it on **one CSV**, and the CSV carries the whole
state of the iteration.

| Tool | Supply | Status |
|---|---|---|
| `limiter_compensate.py` | Limiter (HBRIDGE, `CONFig:MODE LIMiter`) | tested on a simulated bank only, not yet on a real shot |
| `ech_compensate.py` + `power_waveform_picker.html` | ECH-HVPS (HBRIDGE, `HVPS` mode) | built from real shots (2026-11-17); used to fire on 2026-11-17/18 (flat 15 ms pulses, two- and three-pulse shots, staircases, a two-step pulse), each within 1-4 % of target after one or two corrections |
| `ech_meter_calibration.py` | ECH forward-power meters | the factor for `ech_compensate.py --power-scale`; tested on synthetic shots, not yet on a real calibration set |

The tools talk to **MDSplus (read only) and the terminal that runs them, and to nothing else**: no controller, no hub, no other
network service. Everything else they need comes from their CSV and command line, so they run on any machine that can read MDS (or
offline on exported data). Uploading the corrected profile and firing it stay with the operator. `tests/test_isolation.py` fails if a
tool ever imports a network or process module or opens MDS for writing.

## Limiter: why

In LIMITER mode the output is roughly `Vout = s · Vcap(t) · (2D − 1)`, where D is the duty cycle and s = ±1 depends on how the output
is wired and measured. The operator programs D(t) as if the capacitor bank held its voltage. In fact the bank droops during the shot,
and the amount depends on the shot itself (the more power the output delivers, the faster it droops). The output therefore falls short
of the operator's intent, more and more as the shot goes on.

The controller's closed loop (`TARGet:*`, per-phase PID) would correct this during the shot, but it cannot run in the lab yet. This
tool closes the loop between shots instead:

```
operator's demand  ->  shot  ->  limiter_compensate.py  ->  corrected profile  ->  shot  ->  limiter_compensate.py  ->  ...
```

Correcting the duty changes the power drawn, and therefore the droop, so a single pass is not exact. Each pass uses the droop of the
latest shot, and the error shrinks quickly. On the simulated bank, a 15 % droop went from 4.6 % RMS error to 0.7 % after one corrected
shot, and to 0.3 % (the noise floor) after two.

## Limiter: how to use it

1. **Write the demand CSV.** Use the idealized duty profile, with no thought of droop:

   ```
   time_us,carrier_hz,demand_duty_pct
   0,100000,85
   3000,100000,85
   6000,100000,20
   7000,0,0
   8000,100000,75
   12000,100000,75
   ```

   Each row is one `SHOT:STEP <time_us> <carrier_hz> <duty>`, with the same units and rules (section 4.2 of
   `docs/labview_commands.md`). `carrier_hz = 0` is an output-off gap. For per-phase duties use
   `demand_duty_U_pct,demand_duty_V_pct,demand_duty_W_pct`. A plain `duty_pct` column is also accepted as the demand.
   `examples/limiter_example.csv` is a starting point.

2. **Upload it and take the shot.** On this first shot the commanded profile is the demand.

3. **Run the tool on the CSV:**

   ```
   python limiter_compensate.py my_shot.csv                    # the latest WHAM shot in MDS
   python limiter_compensate.py my_shot.csv --shot 250117042   # or a given shot
   ```

   It writes `my_shot_iter1.csv` and `my_shot_iter1.png` beside the input, and prints one line that LabVIEW can parse:

   ```
   RESULT status=ok shot=250117042 iteration=1 out=my_shot_iter1.csv t0_ms=-0.838 sign=+1 v0=895.6 droop_pct=14.69 rms_pct=4.63 setpoints=12 match=1.00
   ```

   On failure it prints `RESULT status=error message="..."`, writes nothing and exits nonzero. Warnings go to stderr, for example a
   demand the bank cannot reach, where the duty had to be clamped to 5–95 %.

4. **Upload the new file's `commanded_*` columns and take the next shot.** The new file holds both the unchanged `demand_*` columns
   and the corrected `commanded_*` columns:

   ```
   # iteration=1
   # from_shot=250117042
   # t0_ms=-0.8382
   # sign=+1
   # v0_volts=895.6
   # droop_pct=14.69
   ...
   time_us,carrier_hz,demand_duty_pct,commanded_duty_pct
   0,100000,85.00,84.93
   1000,100000,85.00,85.69
   ...
   ```

   Each row becomes `SHOT:STEP <time_us> <carrier_hz> <commanded_duty_pct>`, or the three `commanded_duty_U/V/W_pct`.

5. **Run the tool on `my_shot_iter1.csv`.** It writes `my_shot_iter2.csv`, and so on. Stop when `rms_pct` stops improving.

### From LabVIEW

Call it with System Exec, for example
`python "C:\...\compensation\limiter_compensate.py" "C:\shots\my_shot.csv" --shot 250117042`. Read the `RESULT` line from standard
output: `out=` names the file to upload and `status=` says whether it worked. Lines starting with `WARNING`/`ERROR` on standard error
are worth showing to the operator.

### Options (the defaults are meant to be right)

| Option | Default | What it does |
|---|---|---|
| `--shot N` | latest shot | which MDS shot to analyse |
| `--out FILE` | `<csv>_iter<N>.csv` | where to write the corrected profile (the plot goes beside it as `.png`) |
| `--node`, `--tree` | `bias.bias_supply.V_LIM.filtered`, `wham` | the measured signal |
| `--data FILE` | (MDS) | offline: a `time_ms,volts` CSV instead of MDS |
| `--t0-ms X` | found from the data | the measurement time of the shot's t = 0; skips the alignment |
| `--t0-search LO HI` | whole record | limit the alignment's search window (ms) |
| `--sign auto\|+1\|-1` | `auto` | the output's polarity; `auto` finds it from the data and reports it |
| `--gain G` | `1.0` | apply only a fraction G of the correction, if iterations overshoot or oscillate |
| `--duty-offset P` | `0` | dead-time duty bias in percent, for example `-1.9` (the measured duty reads about 1.9 points low) |
| `--tol-pct P` | `0.1` | how closely the setpoints must follow the corrected duty curve |
| `--knot-ms K` | `0.5` | time resolution of the bank-voltage fit |
| `--no-plot` | | skip the PNG |
| `--selftest` | | run on a simulated bank and check that it converges |

## Limiter: the method

1. **Check the profile.** The CSV is checked against the controller's own LIMITER upload rules:
   - t = 0 first, times strictly increasing, at most 1 s;
   - 2 to 500 setpoints;
   - the first setpoint on, the last two on;
   - carrier 75–125 kHz, duty 5–95 %.

   These limits are copied from `firmware/hbridge/src/config/ctrlr_config.h` and `src/app/control/shot_profile.c`; keep them in step.
   The duty is evaluated the way the firmware plays it: linear between setpoints, an on setpoint followed by an off one holding its
   value until the gap, nothing in a gap, and the shot over at the last setpoint's time.
2. **Read the measurement.** V_LIM is read from MDS and resampled to a 10 µs grid. Samples finer than the grid are averaged, not
   picked.
3. **Align.** The measured output is cross-correlated with (2D − 1), taken as zero outside the shot and in gaps. The peak gives the
   shot's start in MDS time (`t0_ms`) and the polarity s (`sign`); `match` is the normalized correlation. Below 0.5 the tool refuses
   (wrong shot? wrong CSV?), and below 0.8 it warns. The baseline before the shot is subtracted.
4. **Estimate the bank.** A smooth, piecewise-linear effective bank voltage Vc(t) is fitted by least squares to
   `V = s · Vc(t) · (2(D + δ) − 1)`. Only samples where |2D − 1| ≥ 0.1 are used, because near 50 % duty the output carries almost no
   information about the bank. A short window around each switch-on and switch-off is also skipped. Vc stays flat where there is no
   information. `V0` is Vc in the first 0.2 ms, and `droop_pct` is how far Vc fell by the end.
5. **Correct.** The target is what the demand gives on an undrooped bank: `Vt = s · V0 · (2(Ddemand + δ) − 1)`. The duty that gives Vt
   on the measured bank is `D* = ½ (1 + (2(Ddemand + δ) − 1) · V0 / Vc) − δ`. The new commanded duty is
   `D_cmd + gain · (D* − D_cmd)`, clamped to 5–95 %.
6. **Re-express as setpoints.** The corrected duty is sampled every 10 µs and reduced to as few setpoints as keep it within `--tol-pct`
   (Douglas–Peucker). The demand's own corners and gaps are always kept, and the total stays at 500 or fewer. Points added by earlier
   iterations are not carried over, so the count does not grow.

Per-phase duties: the measured output is compared with the mean of the three phases' duties, and the same Vc(t) scales every phase.
The droop is the bank's, common to all three. If V_LIM follows one phase rather than the mean, this needs revisiting.

## Limiter: limitations

- The model is only `Vcap · (2D − 1)`. Anything else in the output (load dynamics, the output filter, current limiting) shows up as
  error that the tool attributes to the bank.
- The fit assumes the bank voltage changes smoothly (at `--knot-ms` resolution). Fast events within a shot are averaged out.
- Stretches near 50 % duty say little about the bank. The correction there relies on the fit's interpolation, but the correction there
  is also small.
- It corrects only what repeats from shot to shot. A different bank charge from one shot to the next changes V0. The target scales with
  V0, so the profile's shape is still corrected, but the absolute level follows the bank.
- It is not a replacement for closed-loop control. It is a stopgap until that can run in the lab.

## ECH-HVPS: why

The gyrotron's high-voltage supply is driven by a frequency profile (`SHOT:STEP <t_us> <hz> 50`). Lowering the frequency
towards resonance (near 18.8 kHz) raises the output; the capacitor bank droops while a pulse runs; the output rises
slowly (~1.1 ms to 90 %); and the gyrotron's forward power is a steep function of its voltage that also falls by itself
during a pulse. Getting a square power pulse by hand took many shots on 2026-11-16/17. `ech_compensate.py` does the
same correction between shots, within hard limits.

## ECH-HVPS: the tools

| Tool | What it does | Needs |
|---|---|---|
| `power_waveform_picker.html` | draw a forward-power demand and save it as a CSV | a browser (no network) |
| `ech_compensate.py` | turn a demand CSV into a frequency profile, then correct it after every shot; train its supply model | Python 3, numpy; matplotlib for plots; MDSplus to read shots |
| `ech_meter_calibration.py` | measure the ratio between the dummy-load and experimental forward-power meters (`--power-scale`) | the same |
| `analysis/figures/shot_<N>/plot_shot.py` (repository root) | a publication figure of one shot: forward power, gyrotron voltage and current | numpy, matplotlib; MDSplus once, to fetch |

Every tool reads MDSplus (read only) and local files, writes local files, and talks to nothing else: **no controller,
no hub, no other network service**. Uploading a profile, arming and firing stay with the operator, and so does
confirming what was uploaded (below): the tools take the profile that fired from their CSV. Run them on a machine with MDSplus (the analysis server, or the LabVIEW PC if it has
the MDSplus Python package), or anywhere offline with exported data (see "Exporting a shot" below). Every tool prints
one `RESULT status=ok ...` or `RESULT status=error message="..."` line on standard output, and warnings on standard
error, so LabVIEW's System Exec can call it and parse the result.

## ECH-HVPS: running each tool from a terminal

All commands below run from the `analysis/compensation/` folder (`cd analysis/compensation`); on Windows use `python` for `python3`.

### `power_waveform_picker.html`: draw a demand

1. Open the file in any browser.
2. Click to add points, drag to move them, right-click to delete, click a segment to make it an off gap. The grey band
   after each pulse start is the rise, which the tool does not shape. Up to 15 ms.
3. Set the target (dummy load or vessel) and the bank charge, then **Save CSV**:

   ```
   # mode=power
   # target=dummy-load
   # bank_volts=530
   time_us,demand_kw
   0,200.0
   4000,200.0
   4010,off
   9000,200.0
   13000,200.0
   ```

The same file can be written by hand. Each row is a corner of the demand; `off` starts a gap that lasts until the next
row; the power is linear between rows. `examples/` has three: `ech_flat_200kw_15ms.csv`, `ech_three_pulse_staircase.csv`
(133/166/200 kW in three 4 ms pulses) and `ech_two_step.csv` (150 kW then 200 kW, one 10 ms pulse).

A frequency profile works too: `time_us,demand_hz`, where 0 Hz is a gap. It means "these frequencies, as if the bank
never drooped". The tool then holds the gyrotron voltage that profile implies. Do not give it a profile that is already
hand-corrected for droop: it would read the hand-made ramp as a request for more voltage, and it warns when it sees one.
Use a power target for that instead.

### `ech_compensate.py`: first shot, corrections, training

**First-shot profile** (the CSV has no `carrier_hz` column yet):

```
python3 ech_compensate.py examples/ech_two_step.csv
RESULT status=ok kind=initial mode=power out=examples/ech_two_step_iter0.csv setpoints=7 duration_ms=10 min_freq_hz=19421
```

The frequencies come from the supply model (`ech_plant.json`). **Check them before firing**, above all `min_freq_hz`:
the model was fitted on 2026-11-17, and a gyrotron that has drifted since can need much less drive than it predicts (on
2026-11-18 it put a flat 200 kW, 15 ms pulse at 18.78 kHz at the end, where the real shots needed about 19.0 kHz). When
a measured profile for a similar shot exists, start from that instead (step 1 of the refining procedure below).

**Correction** (the CSV is an `_iterN` file with frequencies, and the shot has been taken):

```
python3 ech_compensate.py my_shot_iter3.csv                    # the latest WHAM shot in MDS
python3 ech_compensate.py my_shot_iter3.csv --shot 261118012    # a given shot
RESULT status=ok kind=correction mode=power shot=261118012 iteration=4 out=my_shot_iter4.csv rms_pct=1.77 max_v_pred_kv=67.2 largest_move_hz=21 setpoints=16 limited_by_step=0 limited_by_vmax=0 at_floor=0 at_ceiling=0 held_by_guard=0 learned_from_previous=yes overshoot_pct=2.7 rise_raise_max_hz=38
```

It writes `my_shot_iter4.csv` and `my_shot_iter4.png` (measured, target and predicted power; measured and predicted
voltage; this shot's and the next shot's frequencies). In the `RESULT` line:
- `rms_pct`: each pulse's error after its rise, one number per pulse;
- `max_v_pred_kv`: the predicted peak gyrotron voltage for the next shot;
- `largest_move_hz`: the biggest change from the profile just fired;
- `limited_by_*`, `at_floor`, `at_ceiling`, `held_by_guard`: how many points a safety limit held back (the `WARNING`
  lines on standard error give the times);
- `learned_from_previous`: whether the run learned the frequency sensitivity from this shot and the one before.

**Training** the supply model from recorded shots: `python3 ech_compensate.py --train corpus/shots.csv` (see the
training procedure below).

**All options** (the defaults are meant to be right; the ones marked *kept* are written into the output CSV and apply to
later iterations without being repeated):

| Option | Default | What it does |
|---|---|---|
| `--target vessel\|dummy-load` | from the CSV | which power meter to read (kept) |
| `--bank-volts V` | from the CSV | the bank charge of the analysed shot (LabVIEW setting) |
| `--next-bank-volts V` | same | the bank charge planned for the next shot: frequencies rescaled with the model's bank offset |
| `--shot N` | latest | the MDS shot to analyse |
| `--data FILE`, `--prev-data FILE` | MDS | offline: this shot's / the previous shot's `time_ms,gyrotron_kv,power_kw` |
| `--out FILE` | `<csv>_iter<N+1>.csv` | the output CSV (the plot goes beside it) |
| `--power-scale K` | 1 | experimental-meter kW per dummy-load-meter kW; the demand is then in experimental-meter kW on either target (kept) |
| `--flatten` | | aim at the shot's own flat-top level instead of the demand's (written into the CSV) |
| `--fixed-times-ms 0:15:1` | | setpoints at exactly these times (kept) |
| `--max-setpoints N` | 500 | at most N rows; the fitting tolerance widens until the profile fits (kept) |
| `--tol-hz F` | 5 | how closely the rows follow the computed frequency curve (smaller = more rows) |
| `--gain G` / `--gain-measured G` | 0.7 / 0.9 | fraction of the correction applied (where the sensitivity is from the model / was measured) |
| `--max-step-hz F` | 100 | most a point may move towards resonance in one shot |
| `--max-kv V` | 70 | gyrotron voltage ceiling |
| `--settle-ms T` | 2 | each pulse's rise window, not corrected on its own |
| `--no-rise-shaping`, `--rise-max-hz F` | on, 300 | the raise-only shaping of each rise against the overshoot |
| `--guard-pct P` | 2 | no further lowering where 100 Hz lower bought less than P % |
| `--plant FILE` | `ech_plant.json` beside the CSV | the supply model |
| `--no-learn` | | use the model, do not update it from this shot |
| `--train MANIFEST`, `--train-append` | | rebuild (or extend) the model from recorded shots |
| `--t0-ms T` | 0 | MDS time of the shot's t = 0 |
| `--ignore-mds-target` | | do not refuse when MDS records the other target |
| `--no-plot`, `--selftest` | | skip the plot; run the loop against a simulated supply |

### `ech_meter_calibration.py`: the two power meters

```
python3 ech_meter_calibration.py 261120010 261120011 261120012 261120013        # shots from MDS (targets read there)
python3 ech_meter_calibration.py data/*.csv --out cal_2026-11-20                  # offline, exported shots
RESULT status=ok power_scale=1.0340 uncertainty=0.0060 per_shot_spread=0.0040 fit_slope=1.0410 fit_offset_kw=-1.3 v_mismatch_pct=0.21 i_mismatch_pct=0.35 lag_ms=0.010 dummy_shots=2 vessel_shots=2 samples=2318
use: python ech_compensate.py <csv> --power-scale 1.0340   (ech_meter_calibration.json, ech_meter_calibration.png)
```

(The numbers above are illustrative.) It writes `<out>.json` and `<out>.png`. `--save-data DIR` also writes each shot
as a CSV, so the calibration can be rerun offline. See the calibration procedure below for how to take the shots and
read the result.

### `analysis/figures/shot_<N>/plot_shot.py`: a figure of one shot

Copy an existing `analysis/figures/shot_<N>/` folder, edit `SHOT`, `SUBTITLE` (and `XLIM_MS` for a short shot) at the top of
`plot_shot.py`, then:

```
python3 plot_shot.py --fetch     # once, where MDSplus is: writes shot_<N>.csv
python3 plot_shot.py             # anywhere: shot_<N>.png/.pdf (3 panels) and shot_<N>_power.png/.pdf
```

### Confirming what the controller actually ran (the operator's job)

The tools cannot see what was uploaded: they take the profile that fired from their CSV, so the CSV must hold what the
controller really received. What was uploaded is not always what was meant. On 2026-11-18, hand entry in LabVIEW dropped
the decimals of every frequency, once shifted every frequency one row early, and once typed 19018 for 20018 Hz.

- **Upload from the CSV, not by hand** where possible: take the `carrier_hz,duty_pct` columns of the `_iterN.csv` the tool
  wrote, as whole hertz.
- **Check the controller's replies as you upload.** Every `SHOT:STEP` answers `OK`, and `SHOT:END` answers
  `OK <rows> <duration_us>`: the row count and duration must match the CSV. To compare the frequencies themselves, read
  them back from the controller in the program that uploaded them (`SHOT:POINt? <n>`, `docs/labview_commands.md`).
- **Keep the record:** keep the uploaded CSV unchanged, and note the shot number and bank charge (the THOR logbook entry
  is a good place). If what was uploaded differs from the CSV, correct the CSV's `carrier_hz` column to what was
  uploaded before running the tool on it: the correction must start from what really fired.

### Exporting a shot (for offline runs and the training corpus)

```python
# export_shot.py -- python3 export_shot.py 261118012 dummy-load   ->  meas_261118012.csv
import sys, numpy as np, MDSplus
shot, target = int(sys.argv[1]), sys.argv[2]
node = {"dummy-load": "ECH_PROC.DL_MONITOR_F:FILTERED", "vessel": "ECH_PROC.WG_MONITOR_F:FILTERED"}[target]
t = MDSplus.Tree("ech", shot, "readonly")
def sig(path):
    n = t.getNode(path); return np.asarray(n.dim_of().data(), float) * 1e3, np.asarray(n.data(), float)
tv, v = sig("ECH_PROC.GYROTRON_V:FILTERED"); tp, p = sig(node)
np.savetxt(f"meas_{shot}.csv", np.column_stack([tv, v / 1e3, np.interp(tv, tp, p)]), delimiter=",",
           header="time_ms,gyrotron_kv,power_kw", comments="", fmt="%.6g")
```

`ech_compensate.py --data meas_261118012.csv` then runs without MDSplus. The `:FILTERED` nodes are written by
post-processing a few seconds after the shot; when they stay empty (the processing computer crashed for shots 011 and
016 on 2026-11-18), that shot cannot be analysed: take it again.

**Required inputs**, from the command line or the CSV's `#` lines (the command line wins):

- `--target vessel|dummy-load` picks the power signal:
  - vessel: `ECH_PROC.WG_MONITOR_F:FILTERED` ("Forward Power (filtered)");
  - dummy load: `ECH_PROC.DL_MONITOR_F:FILTERED` ("ECH Power DL Forward (filtered)").

  If MDS's `ECH_PARAMS:ECH_TARGET` names the other one, the tool refuses (`--ignore-mds-target` to override).
- `--bank-volts` is the bank charge the operator set in LabVIEW for the analysed shot. MDS `ECH_PARAMS:HVPS_V` is
  compared with it, and a difference only warns.
- `--next-bank-volts`, when the next shot uses a different charge. The tool then rescales the frequencies to keep the
  output, with the bank offset the model learned (output proportional to bank minus about 160-175 V). That is less
  certain than a correction at the same charge.

**Safety limits (absolute):**

| Limit | Value | Option |
|---|---|---|
| Frequency | 18.7–21.0 kHz everywhere (the ceiling was 20 kHz until 2026-11-17) | fixed |
| Move per shot towards resonance | 100 Hz at any point; a rescale for a different bank charge is applied on top | `--max-step-hz` |
| Predicted gyrotron voltage | 70 kV; checked after each pulse's rise, against the measured voltage | `--max-kv` |
| Shot length | 15 ms | fixed |
| The rise | the first 2 ms of each pulse is never corrected on its own: it follows the flat top's correction, so the start is never pushed closer to resonance to speed the rise | `--settle-ms` |
| Correction size | 70 % of the computed correction per shot | `--gain` |

Moves away from resonance (raising a frequency) are not limited per shot: they lower the output.

## ECH-HVPS: procedure, refining a shot's demand profile

The loop is: demand CSV → profile → upload → shot → check → correct → upload → ... Each pass reads one shot and writes the
next profile; the CSV chain (`_iter0`, `_iter1`, ...) is the record of what was fired and why.

1. **Write the demand.** Draw it in the picker, copy an example, or write it by hand. Say where it fires and the bank
   charge in its `#` lines.
   - *Starting from scratch:* run the tool on it to get `_iter0.csv` from the model, and check `min_freq_hz` and the plot.
   - *Starting from a measured profile (preferred when one exists):* copy the last `_iterN.csv` of a similar shot, keep
     its `carrier_hz,duty_pct` columns and edit only `demand_kw` (and, if the timing changes, the rows). The first
     correction then starts from frequencies that are known to work. For a new pulse pattern on the same bank, the
     bank only droops while the output is on: a later pulse behaves like the continuous shot at the same accumulated
     on-time, so its frequencies can be taken from a continuous profile at that on-time (this is how the two-pulse,
     three-pulse and two-step profiles of 2026-11-18 were seeded, each pulse within 1-12 % of its target on its first shot).
2. **Upload it** (LabVIEW or the operator UI). Enter whole hertz (`round`), not truncated decimals. Each row is
   `SHOT:STEP <time_us> <carrier_hz> <duty_pct>`, `0 0` is the off row of a gap, and `SHOT:END` must reply
   `OK <rows> <duration_us>`.
3. **Check the upload** before arming: `SHOT:END`'s row count and duration match the CSV, and, if anything was entered
   by hand, the frequencies read back match it (above). Correct the CSV if the controller holds something else.
4. **Take the shot.** Note its number and the bank charge.
5. **Check the shot:** MDS has its `:FILTERED` data, its `ECH_TARGET` is the CSV's target, and nothing else was uploaded
   between your upload and its `ARM`.
6. **Correct:** `python3 ech_compensate.py my_shot_iterN.csv --shot <number>`. Read the `RESULT` line, the warnings
   and the plot:
   - the green predicted power should be flatter than the measured one, and the predicted voltage below the ceiling;
   - where the prediction is *worse* than the measurement, the run probably learned a noisy sensitivity from a small
     frequency change: rerun with `--no-learn` and compare;
   - look at the rows: two rows 10 µs apart with a step between them (the end of a rise window), or rows that zig-zag by
     a few Hz, can be deleted by hand; the controller draws straight lines between the rows that remain.
7. **Upload the new file and repeat from 3.** Stop when `rms_pct` stops falling. The floor is set by shot-to-shot
   repeatability: two shots with the same profile differed by 2-3 kW rms at 200 kW on 2026-11-18, so a flat top within
   about 1.5 % is as good as a single shot shows.

**Example 1: a flat 200 kW pulse, 15 ms, 16 setpoints at whole milliseconds.**

```
python3 ech_compensate.py examples/ech_flat_200kw_15ms.csv --fixed-times-ms 0:15:1    # -> _iter0.csv (check it)
#   upload, fire 261118012 at 520 V
python3 ech_compensate.py examples/ech_flat_200kw_15ms_iter0.csv --shot 261118012
#   -> _iter1.csv, still 16 rows at 0,1,...,15 ms (the setting is kept in the CSV)
```

**Example 2: as flat as possible at whatever level it reached.** When the voltage the nominal power needs is too close to
the ceiling, flatten at the level the last shot reached instead; the level is written into the CSV and kept:

```
python3 ech_compensate.py my_shot_iter5.csv --shot 261118012 --flatten
flatten: demand rescaled to the measured flat-top mean of shot 261118012: 185.8 kW
```

More setpoints help only where the error has structure the supply can follow (about 1 ms and slower):
`--max-setpoints 60 --tol-hz 0.5` gave 54-60 rows on 2026-11-18. To average out shot-to-shot scatter, correct from the
mean of two shots fired with the same profile (average their exported CSVs and pass `--data`).

**Example 3: a three-pulse staircase (133, 166, 200 kW).** Each pulse is corrected from its own error, towards its own
demand:

Start from a measured three-pulse profile (the `_iterN.csv` that fired a flat three-pulse shot), set its `demand_kw` to
133, 166 and 200 in the three pulses, fire it, then:

```
python3 ech_compensate.py stair_iter0.csv --shot 261118021
RESULT ... rms_pct=4.32/5.29/1.71 ...                   # one number per pulse
```

Lowering an early pulse leaves more charge in the bank for the later ones; the first shot of a new staircase should
account for it (on 2026-11-18: 133 kW instead of 200 kW in pulse 1 left the bank about 0.5 ms of full-power on-time
fresher for pulse 3, about +50 Hz). After that the per-pulse correction absorbs it.

**Example 4: two steps in one pulse (150 kW for 5 ms, then 200 kW).** Put the demand's step at the time you want the
power to step (`4990,150` then `5000,200`). The output lags the frequency by about 0.5-0.65 ms, so the frequency step must
come early (4.45-4.55 ms on 2026-11-18); the correction then moves and sharpens the edge itself: the second shot had its
50 % point at exactly 5.00 ms and a 0.56 ms rise (10-90 %).

**Changing the bank charge** for the next shot: `--next-bank-volts 540`. Prefer corrections at a fixed charge; a rescale
is less certain than a correction.

## ECH-HVPS: procedure, building a training corpus

The model needs shots that show how the output responds to frequency and to bank charge. A corpus is a folder with the
profiles that actually fired, the measured data, and a manifest that ties them together:

```
corpus/2026-11-18/
  shots.csv                       # the manifest
  profiles/flat16_written.csv    # profiles as the controller received them (carrier_hz column)
  profiles/iter4_written.csv
  data/meas_261118005.csv         # exported shots (optional: without them, --train reads MDS)
  ...
```

1. **Choose the shots.** Useful for training:
   - *Pairs*: two consecutive shots at the same bank charge whose frequencies differ by 20 Hz or more somewhere (the
     sensitivity is learned only where they differ; moves of 50-100 Hz teach the most). Every correction run makes
     such a pair.
   - *Charge sweeps*: one profile fired at three or more bank charges within 60 V of the highest (the bank offset and
     the power slope dP/dV are learned from these).
   - The same target and the same gyrotron settings (filament, gun coil, magnet) throughout. The gyrotron drifts from
     day to day (on 2026-11-18 its beam current was 6 % lower than on 11-17 at the same voltage): build one corpus per
     day or per gyrotron condition, not one for everything.
   - Leave out shots with no MDS data, faults or arcs, and shots whose upload you cannot confirm.
2. **Collect each shot's profile** from the operator's records: the `_iterN.csv` that was uploaded (corrected for any
   difference noticed at upload time, above), or the profile noted in the logbook. Write it as a profile CSV with the
   frequencies the controller received: `time_us,demand_kw,carrier_hz,duty_pct` (the `demand_kw` column may simply repeat the target),
   with a `# target=` line. One file can serve every shot that fired it.
3. **Export each shot's data** (`export_shot.py` above) into `data/`, or leave `data_csv` empty to read MDS at training
   time.
4. **Write the manifest** in firing order, with the bank charge from LabVIEW (cross-check `ECH_PARAMS:HVPS_V`):

   ```
   shot,profile_csv,bank_volts,data_csv
   261118005,profiles/flat16_written.csv,490,data/meas_261118005.csv
   261118006,profiles/flat16_written.csv,500,data/meas_261118006.csv
   261118007,profiles/flat16_written.csv,520,data/meas_261118007.csv
   261118008,profiles/flat16_written.csv,540,data/meas_261118008.csv
   261118009,profiles/iter4_written.csv,540,data/meas_261118009.csv
   261118010,profiles/iter4_written.csv,520,data/meas_261118010.csv
   ```

   Here 005-008 are a charge sweep with one profile (offset and dP/dV), 008/009 a pair at 540 V (sensitivity), and 010
   starts a new charge. Paths are relative to the manifest.

## ECH-HVPS: procedure, training the model

1. **Train** into a new model file, so the current one stays as it is until the new one is checked:

   ```
   python3 ech_compensate.py --train corpus/2026-11-18/shots.csv --plant corpus/2026-11-18/ech_plant.json
   RESULT status=ok kind=train shots=6 pairs=1 pair_samples=641 bank_groups=1 bank_bins=29 dpdv_points=87 plant=corpus/2026-11-18/ech_plant.json
   ```

   `--train-append` adds the corpus to an existing model instead of starting from the starting values (for example a
   second day of the same gyrotron condition). `WARNING skipped ...` lines name shots that taught nothing and why.
   Vessel shots with an experimental meter that differs from the dummy-load one: add `--power-scale K`, so the model
   stays in dummy-load-meter kW like the rest.
2. **Look at what it learned:**

   ```python
   # show_plant.py -- python3 show_plant.py corpus/2026-11-18/ech_plant.json
   import json, sys
   d = json.load(open(sys.argv[1]))
   print("kappa, %/100 Hz:", ", ".join(f"{f/1000:.1f} kHz {1e4*k:.1f}" for f, k, w in zip(d["f_hz"], d["kappa"], d["kappa_weight"]) if w > 0))
   print("bank offset, V: ", ", ".join(f"{t:g} ms {b:.0f}" for t, b, w in zip(d["t_ms"], d["bank_offset_v"], d["bank_offset_weight"]) if w > 0))
   print("dP/dV, kW/kV:   ", ", ".join(f"{v:g} kV {p:.1f}" for v, p, w in zip(d["v_kv"], d["dpdv"], d["dpdv_weight"]) if w > 0))
   ```

   Only learned points are printed. From the corpus above (2026-11-18): kappa 3.5-3.9 %/100 Hz at 19.4-20.0 kHz,
   falling to 1.8 at 19.2 and 0.3-0.7 at 19.0-19.1 kHz (the gain peak); bank offset 150-173 V over 1-15 ms; dP/dV
   16-21 kW/kV at 54-68 kV (single points beyond the data, like 27 at 69 kV, carry little weight).

   The sensitivity (`kappa`) should fall towards resonance (about 4 %/100 Hz near 19.5-20 kHz, 1.5-2 % near 19.0 kHz on
   2026-11-17); the bank offset should be smooth in time; dP/dV near 15-20 kW/kV above 56 kV.
3. **Check it on shots it was not trained on.** Take two consecutive shots from another session; run the correction on
   the first with the new model and without learning, and compare its predicted power and voltage (green in the plot)
   with what the second shot measured:

   ```
   python3 ech_compensate.py held_out_iter0.csv --data meas_A.csv --plant corpus/2026-11-18/ech_plant.json --no-learn --out check.csv
   ```

4. **Use it:** copy it next to the CSVs as `ech_plant.json` (or pass `--plant`). Keep the old file. Every correction run
   keeps refining the model from its own shot pairs; `--no-learn` stops that for a run.

## ECH-HVPS: procedure, calibrating the power meters (`--power-scale`)

Dummy-load shots are measured by the dummy-load meter; experimental shots into the vessel by the waveguide forward-power
meter. The two never see the same pulse, and they do not read the same for the same power. The power scale K is
**experimental-meter reading / dummy-load-meter reading** for the same gyrotron output. Comparing shots from different
days does not measure it: the gyrotron's own power-voltage curve moves by more (on 2026-11-13 vessel shots and
2026-11-18 dummy-load shots at the same voltage and current, the ratio came out anywhere from 0.89 to 1.06).

1. **Take the calibration shots** in one session, with the same profile, bank charge and gyrotron settings, alternating
   the target: dummy load, vessel, dummy load, vessel (two of each at least; more if the shots scatter). A staircase
   profile (three power levels in one shot) also shows whether K depends on power.
2. **Run the calibration:**

   ```
   python3 ech_meter_calibration.py 261120010 261120011 261120012 261120013 --save-data cal_2026-11-20
   ```

3. **Read the result:**
   - `v_mismatch_pct` and `i_mismatch_pct` must be small (the script refuses above 1 %): they say the gyrotron ran the
     same way into both targets, so the ratio is the meters'. If they are large, the vessel itself changes how the
     gyrotron runs: find out why before using any factor.
   - `uncertainty` combines the shot-to-shot spread with the sample scatter; with two shots of each it should be well
     under 1 %.
   - `fit_offset_kw` near 0 means one factor fits every power level. A large offset (warned) means the ratio changes
     with power: the plot's lower-left panel shows how. K is then the average over the levels in the shots: calibrate at
     the power you will use.
   - `lag_ms` is the meters' timing difference, removed before the ratio.
4. **Use it:**

   ```
   python3 ech_compensate.py my_shot_iter3.csv --power-scale 1.034
   ```

   From then on the demand (`demand_kw`) is in **experimental-meter kW** on either target: on the dummy load the tool
   aims at demand / K on the dummy-load meter; on the vessel it compares the experimental meter with the demand
   directly. The plots show experimental-meter kW. The factor is written into the CSV (`# power_scale=`) and kept. A
   profile tuned on the dummy load this way lands at the right experimental power on its first vessel shot; after that
   `--target vessel` corrects against the experimental meter itself.
5. **Repeat the calibration** when a meter, a coupler, an attenuator or the waveguide changes, and now and then.

## ECH-HVPS: learning from shot to shot (added 2026-11-17)

The tool keeps a model of the supply in **`ech_plant.json`**, next to the CSVs (`--plant FILE` for another one). It holds two
things:
- the output's sensitivity to frequency (`kappa`, the fraction the output rises per Hz lower), on a 100 Hz grid from 18.5 to
  21.5 kHz;
- the gyrotron's forward-power slope (`dpdv`, kW per kV), on a 1 kV grid.

Every frequency calculation uses the model: the correction, the bank-charge rescale, the voltage-ceiling prediction and the
first-shot profile. Each CSV the tool writes records the shot and the CSV it came from (`from_shot`, `from_csv`). So on the
next run the tool also reads **the previous shot** (from MDS; offline with `--prev-data`). Wherever the frequency changed
between the two shots by at least 20 Hz, the measured voltage change is the real sensitivity there:

- **That correction uses it**, with `--gain-measured` (0.9) instead of the cautious `--gain` (0.7) used where nothing was
  measured.
- **The model learns it.** Each grid point is a weighted average of what was measured there. The weights are capped, so
  the model keeps following a gyrotron that drifts. Where nothing has been measured, it falls back to the starting values.
- **Guard.** Where 100 Hz lower bought less than `--guard-pct` (2 %), the supply is near its gain peak. Nothing at or below
  that frequency is lowered any further, and the tool says that more output there needs more bank charge.

A pair at two different bank charges teaches nothing, and the tool says so. Use `--no-learn` to use the model without
updating it. Copy `ech_plant.json` to keep what has been learned (for example to the LabVIEW PC). Delete it to start again
from the starting values: 4 %/100 Hz at 19.5-20 kHz, falling to 1.7 %/100 Hz at 19.0 kHz, both measured 2026-11-17.

On the simulated supply, learning cut the error after the first pair by 2-4 times compared with the fixed model. The
exception is a supply whose sensitivity sits close to the guard level: there the guard holds it back, on the safe side.
On 2026-11-17's shots, the model learned 4.3-5.9 %/100 Hz at 19.8-19.9 kHz, 2.6-3.6 % at 19.2-19.5 kHz and 1.5-2.2 % at
18.9-19.1 kHz. The power slope was 13-20 kW/kV at 56-61 kV.

### Retraining the model from recorded shots (`--train`, added 2026-11-18)

```
python ech_compensate.py --train shots.csv              # rebuilds ech_plant.json next to shots.csv
```

`shots.csv` lists the shots in firing order: `shot,profile_csv,bank_volts[,data_csv]`. `profile_csv` is the profile that
actually fired (with `carrier_hz`). The data comes from MDS unless `data_csv` (`time_ms,gyrotron_kv,power_kw`) is given.
Paths are relative to `shots.csv`. Training starts from the starting values (`--train-append` adds to the existing model
instead) and learns three things:

- **Frequency sensitivity:** from every two consecutive shots at the same bank charge, as a correction run does.
- **Bank-charge offset:** from every profile fired at three or more charges. The output scales as `bank - offset(t)`,
  fitted for each 0.5 ms of the shot on the charges within 60 V of the highest one. It is used whenever
  `--next-bank-volts` rescales a profile. Before 2026-11-18 it was a fixed 151 V.
- **Power slope (dP/dV):** from the same charge sweeps, between neighbouring charges.

Trained on 2026-11-17's 40 dummy-load shots, the model gave:
- an offset of 158-176 V over 1-14 ms (output rises more per volt of charge than the fixed 151 V said, most of all late in
  the pulse);
- a power slope of a steady 17-19 kW/kV above 56 kV;
- frequency sensitivity unchanged (it comes from the same shot pairs).

## ECH-HVPS: shaping the rise against the overshoot (added 2026-11-18)

The forward power overshoots by 6-8 % near 1.6 ms on every charge, then dips before it settles. Early in a pulse the
gyrotron gives more power for the same voltage, and that excess fades over the first couple of milliseconds. Lowering the
voltage a little during the rise cancels it.

Inside each pulse's rise window (`--settle-ms`, 2 ms) the frequency is the flat top's starting frequency **plus a raise
r(t) >= 0**. The raise is learned from how far the power ran above its target, looked up one output lag (0.65 ms) ahead.
- It only ever adds frequency on top of the flat top's start, so the rise can never be pushed closer to resonance than
  the flat top itself. A faster rise would need exactly that, and the tool does not attempt it.
- It shrinks back towards 0 where the power is below its target. An overcorrection is therefore undone, not kept.
- It is capped at `--rise-max-hz` (300 Hz) and at the usual per-shot step.

`--no-rise-shaping` turns it off. The RESULT line reports `overshoot_pct` and `rise_raise_max_hz`. On a simulated supply
that overshoots like the real one, it took the overshoot from about 16 % to 1 % in 3-4 shots, with a raise of about 70 Hz.

## ECH-HVPS: setpoints at fixed times (`--fixed-times-ms`, added 2026-11-18)

`--fixed-times-ms 0:15:1` puts the setpoints at exactly those times (here 0, 1, 2, ... 15 ms: 16 rows) and nowhere else.
A comma list also works (`0,1,2.5,...`). The setting is stored in the CSV, so later iterations keep it.

- **How the 16 frequencies are chosen.** They are fitted by least squares, so that the controller's straight lines between
  them follow the corrected frequency curve as closely as possible.
- **Hard limits.** Each point is held at or above its lower bound: the floor, the voltage ceiling, the gain-peak guard,
  the per-shot step and, in the rise, the flat top's start.
- **Between points.** A straight line may dip at most 15 Hz (`FIXED_DIP_TOL_HZ`) below the bound, because 1 ms spacing
  cannot follow every kink of the previous profile. The floor still holds exactly. The voltage ceiling is applied with the
  same 15 Hz margin. The RESULT line reports `fixed_times_max_dip_hz`.
- **Demand corners.** A corner or gap of the demand that falls between the fixed times is refused.
- **Rise shaping.** At 1 ms spacing the rise raise becomes one raised point at 1 ms.

Since 2026-11-18 the plot's "next shot" frequency line and the predictions (power, voltage, maximum voltage) are computed
from the setpoints actually written, not from the ideal curve they approximate.

## ECH-HVPS: flattening at the level the shot reached (`--flatten`, added 2026-11-18)

`--flatten` aims at the analysed shot's own flat-top mean instead of the demand's level, so the run corrects only the
shape: the overshoot, the dips and the sag. The mean is taken per pulse over the same window the correction uses (after the
2 ms rise, up to 0.1 ms before the pulse's end). The demand is rescaled to that level and written into the output CSV
(with `flatten_from_shot` and `flatten_level`), so later iterations hold the same level without the option instead of
chasing a mean that moves from shot to shot. This applies to power demands only.

Use it when a flat pulse matters more than the exact power, for example when the voltage needed for the nominal power is
too close to the ceiling. To go back to a power target, start again from the picker CSV.

**What a pair can teach.** The tool learns frequency sensitivity only where the frequency moved by at least 12 Hz between
the two shots, and gives a point more weight the bigger the move. A move of a few tens of Hz against about 2 % of
shot-to-shot noise can still produce a near-zero estimate. A
correction built on it then moves far, and its own predicted power shows the overshoot. If the prediction looks worse than
the measurement somewhere, rerun with `--no-learn` and compare (2026-11-18, shot 261118012 at 13 ms).

## ECH-HVPS: the numbers it starts from

These come from 25 dummy-load shots on 2026-11-17 (261117004–028; filament 41.0 A, gun coil 2.1):
- a charge sweep with one frequency ramp;
- a two-pulse profile at eight charges.

| Quantity | Value | Used for |
|---|---|---|
| Frequency effect | **+4.0 % output per 100 Hz lower** near 19.5–20 kHz (±0.2 % across 8 charges) | the correction's step size |
| Output against bank charge | about 0.164 kV per V of bank above 151 V, at 20 kHz, settled | first-shot profile, bank rescale |
| Droop at constant frequency | about −1 % per ms at 1.1 MW | first-shot profile |
| Pulse 2 after a 5 ms gap | starts 6–7 % lower than this simple droop model says; with the trained model (2026-11-18) a later pulse matches the continuous shot at the same on-time within 1-2 % | first-shot profile (5 %); later shots learn each pulse's own error |
| Rise | ~1.1 ms to 90 %, a ~0.65 ms lag; the peak is near 1.8–2.5 ms | settle window, phase lead |
| Forward power against voltage | ~0 below 45 kV; 100 kW at 57 kV; 200 kW near 61–62 kV; 280 kW at 66 kV | power-target sensitivity |
| Power at constant voltage | falls by itself during a pulse (by half within 5 ms at 500 V); holding power flat needed the voltage to rise ~0.8 % per ms | first-shot profile |

Each correction uses the measured error of each pulse, so a wrong starting number costs a shot or two, not safety.
Because the power-target correction raises the voltage to make up the gyrotron's own decay, it is bounded by the
voltage ceiling.

## Tests

```
python3 -m unittest discover -s analysis/compensation/tests
python3 analysis/compensation/limiter_compensate.py --selftest
python3 analysis/compensation/ech_compensate.py --selftest
python3 analysis/compensation/ech_meter_calibration.py --selftest
```

The tests use a simulated bank: a capacitor driving a resistive load, so the droop depends on the profile, as on the real supply.
Measurement noise, a filter like the `.filtered` node's, an unknown start time, an offset and either polarity are included. They check
the alignment, the bank fit, convergence over iterations (with gaps and per-phase duties), clamping, the controller's rules and the
CSV round trip. They need numpy; the plot needs matplotlib; MDSplus is needed only to read real shots.
