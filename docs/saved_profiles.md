# Saved profiles (external flash)

How the controller keeps settings and tables across power cycles and firmware updates, and how this
compares with WHAM-XREX-PFMG474, which this was ported from. Added 2026-10-05, branch `flash-profiles`.

## What the operator sees

Eight numbered slots. Commands and replies are the same as XREX's (`docs/command_reference.md`):

```
*SAV <slot> [label]            save the live settings and tables         (IDLE only)
*RCL <slot>                    apply a saved profile                      (IDLE only)
CONFig:PROFile:CATalog?        OK 1=<label>/<fw> 2=empty 3=corrupt ...
CONFig:PROFile? <slot>         preview every saved value; nothing applied
CONFig:PROFile:DIFF? <slot>    what a recall would change (or SAME); nothing applied
CONFig:PROFile:CLEar <slot>    erase a slot                               (IDLE only)
QSPI:SELFtest                  bench test of the flash driver (scratch sector only)
```

Loading is always explicit: **nothing is applied at boot** (same rule as XREX). Saved data lives in the
external W25Q128 chip, so it survives power cycles, resets, firmware updates and rollbacks -- the two internal
flash banks belong to the dual-bank firmware update and are never used for settings.

## Layers

```
cmd_profile.c        *SAV *RCL CONFig:PROFile:*   (src/app/commands)
settings.c           capture / apply the live values; stream the three tables to and from flash
settings_codec.c     the scalar snapshot, the tagged-record encoding, CRC, text formatting (pure, host-tested)
profile_store.c      slots, alternating copies, header + CRC, streaming writer and reader (pure, host-tested)
nvstore.c            byte store on the W25Q128: read, 4 KiB sector erase, page program, verify (src/bsp/stm32g4)
qspi_test.c          QUADSPI bring-up, shared handle
```

`nvstore` and `profile_store` are the same code as XREX's apart from the generalizations listed below.

## What is stored

Everything is a **tagged record** `[field id u8][channel u8][length u8][value]`, little-endian, in the payload
of one slot. Field ids are a frozen contract (`settings_codec.h`): unknown ids are skipped and fields a save
does not have are left alone on recall, so an older and a newer firmware can read each other's saves.

| Group | Fields (ids) | Notes |
|---|---|---|
| System scalars | supply mode (1), PIDRate (2), ADC:AVERage (3), ADC:SAMPle x10 (4), SLEWRate (5), MAXVolts (6), feedback channel / SCALE / OFFSet / FILTer / CLIPTime (7-11), PID:LOOPMODE (12) | in loop units (process volts); see `command_reference.md` |
| Closed-loop plant | HVPS duty (13), Limiter carrier (14), resonance (15), VPEAK (16), VSCALE (17), BRANCH (18) | |
| Per plant (channel byte = plant: 0 HVPS, 1 Limiter) | kp, ki, kd (32-34) | both plants are saved whichever mode is selected |
| Shot table | count (64), points (65) | 10 bytes per point (t_us, freq_mHz, duty_hundredths), 24 points per record |
| Target table | count (66), points (67) | 8 bytes per point (t_us, millivolts), 30 per record |
| HVPS curve | count (68), points (69) | 8 bytes per point (Hz, volts as floats), 30 per record; count 0 = the linear default |

**Not saved**, on purpose: ARM/FIRE/SOURce run state and `SOURce:SETpoint`, which phases are enabled (`PHASe:ENAble`:
a recall must never change which legs of the bridge are driven), the DAC outputs, and every
bench-simulation knob (`CONFig:FEEDback:SOURce`, `DACRatio`, `SIM:PLANT:*`). A profile that put the loop onto a
simulated feedback by itself would be a hazard on a real supply.

## Recall

`*RCL` first checks the slot is valid (an empty or corrupt slot leaves everything live untouched), drops the
legacy `TABle:`, the shot and the target and any upload in progress, then applies in a fixed order:

1. supply mode (the tables are checked against it);
2. ADC sample time, scans and PIDRate -- checked against each other, so a value refused against the others'
   current values is retried in the other order (as XREX does for turn-on/maximum frequency);
3. feedback calibration, slew, MAXVolts, closed-loop plant parameters, gains per plant, loop mode;
4. the tables, each through its own validated append function (`ShotProfile_Append` checks the supply mode's
   limits, `Target_Append` MAXVolts, `HvpsCurve_Append` the shape).

Every value goes through the same setter the serial command uses, so every limit still applies. A value a setter
rejects is counted and named (`OK APPLIED=23 FAILED=1 SHOT`) and the rest still applies. A **table that fails
part way is left empty**, never half loaded (a shot or target missing its tail must not be runnable); a curve
that fails validation leaves the live curve untouched. A successful shot recall selects the PROFILE source,
exactly as `SHOT:END` does, so `FIRE` plays it.

## Flash layout and safety

```
0 .. 64 KiB      XREX's slots (8 slots x 2 copies x 4 KiB)            -- not touched by this firmware
64 KiB .. 320 KiB   HBRIDGE's slots (8 slots x 2 copies x 16 KiB)
last 4 KiB       QSPI:SELFtest scratch sector
```

* **Alternating copies.** A save never touches the copy holding the newest valid data: it erases and writes the
  other copy, and only a copy whose CRC checks out and whose sequence number is higher counts.
* **Header last.** The 48-byte header (magic, format version, length, sequence, label, firmware, CRC-32 of the
  header and payload) is programmed after the payload. Until then the copy reads as empty; a half-written header
  fails its CRC. A power cut at any point of a save leaves the old profile exactly intact (host-tested by
  injecting a cut at every step of small saves and at a spread of steps of a 9 KB multi-sector save).
* **Product magic.** HBRIDGE writes `HSPF`, XREX `WSPF`. The two controllers share this board and chip; each
  reads the other's data as "corrupt" in its catalog and never decodes it (the field ids overlap).
* **Streaming.** A full profile is about 5.5 KB with a 500-point shot and about 9.5 KB with both tables full,
  and free RAM is about 14 KB, so tables are never copied: they are encoded straight from the live table into
  flash (a counting pass first, because the header needs the length) and decoded straight from flash into the
  live table, with the whole payload's CRC verified before anything is applied.

Measured on the simulator board (real W25Q128): `*SAV` of a 500-point shot + target + curve 175-200 ms, `*RCL`
65-175 ms, `CONFig:PROFile:CATalog?` 45-80 ms. Sector erase 59 ms, 300-byte program 1 ms (`QSPI:SELFtest`).

## Differences from XREX, and how to bring them into harmony

| | XREX | HBRIDGE | Harmonizing |
|---|---|---|---|
| What a profile holds | 32 scalars: per-channel PID gains, loop mode, max/demand current, nickname, system frequencies, fault polarities, timing | 18 scalars + gains per plant + three tables (shot, target, HVPS curve) | The record framing is identical; only the field table and `settings.c` differ. Keep ids product-specific and frozen. |
| Meaning of the channel byte | Transrex channel 0..3 | plant 0 HVPS / 1 Limiter | Document per product (done). If XREX ever gains a plant concept it can reuse the field. |
| Payload size | 1 KiB, one 4 KiB sector per copy | up to 16,336 bytes, four sectors per copy | **Done**: `PROFILE_COPY_SECTORS`, `PROFILE_STORE_BASE`, `PROFILE_MAGIC_BYTES` in `ctrlr_config.h`; defaults reproduce XREX's layout, so XREX's existing slots stay readable. |
| Writing | was: buffer in RAM, erase, header, then payload | streaming writer, payload then header last | **Done (2026-10-05)**: carried into XREX (`shared-profile-store` branch); the buffered `ProfileStore_Write` is kept as a wrapper. |
| Reading | whole payload into a 512-byte buffer | streaming reader, CRC pass then apply pass | Same: XREX can keep its buffer or move to the stream API. |
| Tables | none (demand is two scalars per channel) | three tables as chunked records | If XREX adds a time-series demand it can use the same chunk records. |
| Shared flash chip | slots from 0 | slots from 64 KiB, different magic | **Done**: separate regions plus separate magic, so a board that ran both keeps both products' profiles. A short region/magic registry belongs in both repos' docs. |
| Float text in `PROFile?` | was `snprintf("%g")` -- needs `-u _printf_float` at link | own formatter (`SettingsCodec_FormatFloat`), no printf floats | **Done (2026-10-05)**: in XREX too. XREX keeps the link flag, other replies still use `%f`. |
| Setters | in `pid.c` | moved from `cmd_control.c` into `control_runtime.c` so commands and profiles share them | Same principle in both: one setter per value, used by the command and by recall. |
| Recall side effects | none beyond the setters | selects the PROFILE shot source; drops table/shot/target and upload flags first | Specific to HBRIDGE's shot model. |
| Same in both | 8 slots, 1-based, `*SAV/*RCL/CONFig:PROFile:*`, `ERR 18`, IDLE-only writes, explicit load, label rules, tagged records, CRC-32, alternating copies, `nvstore` driver, `QSPI:SELFtest` | | |

**Update 2026-10-05:** `nvstore`, `profile_store`, the profile-store tests and the float formatter are now one
shared piece of code in both repos, with `python/check_shared.py` guarding against drift and
`docs/saved_profiles_shared.md` holding the flash map and magic registry. Remaining suggestions: (1) move the
record framing and helpers (`EmitRecord`, `NextRecord`, CRC, little-endian helpers) of `settings_codec` into the
shared set too, which means XREX adopting them; (2) split `settings_codec` into a generic record layer and
a product field table, which is how it is already organized internally; (3) decide together whether either
product should offer a boot-time slot (neither does today).
