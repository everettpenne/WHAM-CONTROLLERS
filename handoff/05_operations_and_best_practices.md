# 5. Operations and best practices

Snapshot: 2026-11-16, `main` after `666fd35`. Step-by-step operator procedures: `firmware/<product>/docs/operators_guide.md` and the SOP.

## 5.1 Golden rules

1. **Identify before you send.** First command on any connection: `*IDN?`. Check the product (`WHAM-HBRIDGE-PFMG474` or
   `WHAM-XREX-PFMG474`) and the serial (`SN=WB-...`). The lab subnet also has an **unrelated high-voltage controller** listening on the same
   TCP port as our bridges (it answers `*IDN?` with a different product string); never send it anything else.
2. **Read the state before and after.** `STATE?` and `FAULT?`. Know whether you are in `IDLE`, `ARMED`, `FIRING` or `FAULT` and why.
3. **Never `ARM` or fire without the owner's OK for that board.** The ECH-HVPS (WB-0001-8) is installed in its supply.
4. **Read back what you set.** Several setters round or clamp (dead time rounds to 5.88 ns steps; XREX clamps setpoints). The `?` query
   is the truth.
5. **Know that the external trigger is armed by default on HBRIDGE.** `EXTernal:TRIGger?` is `1` after every boot, so an `ARMED`
   controller fires on a PF15 rising edge with no command. Send `EXTernal:TRIGger 0` if you do not want that.
6. **After `*RCL`, check the reply and the result.** `FAILED=0` means everything applied. `GDSOFF=n` means the profile left `n` fault
   inputs switched off. A recall can change the supply mode, the phases, the dead time and the GDS polarity: read them back.

## 5.2 Connecting

**Through the lab hub (normal).** Each board has a hub instance on the lab server `andrew`: port 5101 XREX-CTRL, 5102 LMTR-CTRL, 5103
ECH-HVPS. From your computer, open an ssh tunnel (use local ports that nothing else uses; the operator UI may already hold 510x):

```bash
ssh -f -N -o ServerAliveInterval=30 -o ExitOnForwardFailure=yes -L 15101:127.0.0.1:5101 -L 15102:127.0.0.1:5102 -L 15103:127.0.0.1:5103 andrew
```

Then talk to a board, for example the Limiter: `python3 host/net_client.py --port 15102 --name <you>`, or any tool with
`--url socket://localhost:15102`. `!hub status` asks the hub itself (look for `connected=1`). If the hub says "the controller is not
connected", the hub has lost the board's network bridge: check from `andrew` whether the bridge answers ping (see `docs/bench_devices.md` for
its address). The tunnel dies if your network drops; reopen it.

**Directly on the bench.** USB-serial at 115200 8N1. The macOS device name (`/dev/cu.usbserial-NNNN`) changes when you replug, and two
boards look identical: identify by `*IDN?`. A freshly enumerated port can ignore the first command; wait about 1.5 s or retry.

**Only one client per bridge.** The WIZnet module accepts one TCP connection. If a hub is running, connect through the hub, never directly to
the bridge.

## 5.3 Updating firmware (normal path, over the network)

1. Get the owner's OK for this board and this image, and confirm the board is safe (on a bench, or its supply de-energized).
2. Build from a clean tree: `python3 python/wham_build.py` (4.2). Note the commit.
3. Bring the board to `IDLE`. A bench board with nothing wired boots in `FAULT`:
   * HBRIDGE: `GDS:ENAble 0 0` then `FAULT:CLEar` then `STATE?` (these are RAM-only; the reboot after the update restores the defaults).
   * XREX: `EXTernal:ENAble 0` then `FAULT:CLEar` then `STATE?`.
   * Do not do this on a board connected to a supply without knowing why its interlocks are unsatisfied.
4. Run the update through the hub:
   ```bash
   python3 python/fw_update.py --url socket://localhost:15102 --yes
   ```
   It erases the inactive bank, streams the image (about 1 minute for 113 KB), verifies the CRC, swaps and waits for the `!BOOT` banner.
   Success ends with `[ok] now running <version> <commit> from bank <n>`.
5. Check: `*IDN?` shows the new commit; `STATE?`; the settings you care about are at their defaults (nothing is restored at boot).
6. Something wrong? `python3 python/fw_update.py --url ... --rollback` boots the previous image (still in the other bank).

`fw_update.py --status` shows the running bank. An HBRIDGE board refuses an XREX image and the reverse.

## 5.4 Recovering a board with the ST-Link

Use when the serial link is dead, the board is stuck in the ROM bootloader, or the product must change.

* Wire **SWDIO, SWCLK, GND and 3V3 only**. Leave the ST-Link's NRST wire **off** (see `06_known_hardware_issues.md`, NRST). The ST-Link
  must read about 3.3 V target voltage; 0 V means the board is unpowered or the cable is off.
* Flash with openocd:
  ```bash
  openocd -f interface/stlink.cfg -c "transport select hla_swd" -f target/stm32g4x.cfg -c "reset_config none" -c "adapter speed 480" \
          -c "init" -c "reset halt" -c "program build/WHAM-HBRIDGE-PFMG474.bin 0x08000000 verify" -c "reset run" -c "exit"
  ```
  (`adapter speed` must come after the target file.)
* `st-flash` is not bank-swap aware: only use it when `FWUPdate:STATus?` shows `BANK=1 BFB2=0`. Otherwise you write the inactive bank.
* Before changing option bytes, read and record them (`DIAGnostic:OPTBytes?`), and change only the intended bit with a mask.

## 5.5 Running an HBRIDGE shot (with the owner's OK)

```
*IDN?                         check product and serial
STATE?                        -> OK IDLE
CONFig:MODE HVPS              (or LIMiter; discards any old shot)
CONFig:LIMits?                the ranges that apply
PHASe:SELect U                Limiter today: one electrode, on U (HVPS: PHASe:SELect UVW)
PHASe:ENAble?                 which phases will drive
EXTernal:TRIGger 0            unless you want the PF15 trigger live
SHOT:BEGin
SHOT:STEP 0 20000 50          time us, frequency Hz, duty % (or three duties: dU dV dW)
SHOT:STEP 2000 20000 50
SHOT:STEP 5000 19820 50
SHOT:END                      -> OK <points> <duration_us>
SHOT?                         read back
ARM                           (owner's OK)
FIRE                          (owner's OK)
STATE?                        FIRING, then IDLE when it ends
```

Units trip people: frequency is in **Hz** (`19000`, not `19.000`), time in **microseconds**. A setpoint outside the mode's range is refused
at upload (`ERR 10`). A `0` frequency makes an output-off gap.

**Measuring a shot without a scope.** With the outputs wired to the PFM inputs (U, V, W, UN on inputs 1-4), send `PFMIN:CAPTURE <n>` before
`FIRE` and read the periods with `PFMIN:DATA?` (paged; the bridge drops characters in long replies, so check each page's `SUM=`).
`python/pfm_shot_test.py` does all of this and plots. This gives the rising-edge periods only; falling edges, duty and phase between channels
need a logic analyzer.

**With the DSLogic.** Close DSView before using the headless capture tool (DSView holds the USB device). On the bench the wiring used was
U, UN, V, VN, W, WN on DSLogic channels 0-5 (each phase and its complement on adjacent channels). DSView `.dsl` files are zip archives;
`host/dsl_viewer.py` reads them.

## 5.6 Saved profiles in practice

* `CONFig:PROFile:CATalog?` lists the slots. Slots 1 (`bench`) and 2 (`regress`) on the Limiter belong to earlier test work.
* Before a recall: `CONFig:PROFile:DIFF? <slot>` shows exactly what would change. Show it to whoever owns the board.
* `*SAV` and `*RCL` work only in `IDLE`. The link is silent for a fraction of a second while flash is written.
* After any reset, nothing is loaded: recall explicitly.

## 5.7 Safe state at the end of a session

* `STATE?` is `IDLE` (or `FAULT` on a bench board whose inputs are unwired, which is its normal boot state).
* No test values left behind: dead time back to 100 ns, all phases enabled, GDS inputs enabled, test profile slots cleared.
* `CHARGE:FREQuency 0` unless the owner wants the charger running.
* Close tunnels you opened. Tell the owner what you changed on any board, and what you left changed.

## 5.8 Things that have bitten people

| What happened | Lesson |
|---|---|
| A blind `y` piped into a confirmation prompt fired a real shot (2026-09-14). | Confirm interactively, step by step. |
| A "should be refused" `BOOT` sent during a short shot arrived after the shot ended and entered the ROM bootloader (needed a power cycle). | Network round trips are ~65 ms. Use long shots for gate tests. |
| A stale `.bin` was flashed and "verified". | Build with `wham_build.py`. |
| Frequency typed in kHz (`19.000`) was refused as out of range. | Units are Hz. |
| A board went silent after a firmware swap or power-up until the ST-Link was plugged in. | Missing NRST pull-up (06). |
| The simulator board appeared to receive nothing, randomly. | Its firmware was running the controller's fault logic on pins that mean something else there (fixed). Check the *other* board's `STATE?` too. |
| An ECH network bridge vanished mid-test (2026-11-16). | Unexplained; power, cable or network. If a board stops answering, check the hub's `connected=` and ping before blaming firmware. |
