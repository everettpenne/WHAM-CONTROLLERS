# 8. Open items: unfinished and unverified

Snapshot: 2026-11-16, `main` after `666fd35`. Grouped by product, then by kind. "Unverified" means written and host-tested (and usually flashed)
but never confirmed on real hardware or a real supply. Check the changelog before starting one: some may have moved since this snapshot.

## 8.1 Before either product controls a real supply in service

* **Commissioning on the real supplies.** Neither product has closed a loop on its real plant. XREX has run only against loopbacks and the
  simulator board; HBRIDGE closed loop only against firmware plant models and the frequency loopback.
* **Network security.** The hub has no authentication; any machine that reaches its port can fire a shot. Decide on `allow_from`, a
  firewall, or real authentication. Leases ("one writer at a time") were deferred, not rejected.
* **Hardware fixes from section 6.12** on every board in service, at least the NRST pull-up.

## 8.2 XREX

**Calibration placeholders** (in `src/config/ctrlr_config.h`; must be measured on each real Transrex, not inferred from the controller's own feedback):
* Maximum current per channel (all four `5000 A` placeholders): drive each supply at the maximum frequency and measure its real output current.
* Turn-on and maximum frequencies (shared, believed to be properties of the modulation scheme; unconfirmed).
* The amps-to-hertz mapping is **linear**; the real relation may follow SCR firing-angle physics. Replace `AmpsToHz()` only.
* Output slew limit (`2000 Hz/tick`, chosen on a bench loopback) and fault ramp-down time (`1.0 s` first guess).
* PID gains: only bench-loopback tuning exists. A gain sweep (2026-09-22) showed output jitter is driven by **Ki**, and LOWER Ki is smoother;
  the sweep only went down to Ki = 250. Next: sweep Ki in {25, 50, 100, 250} on the real plant. Kp in 0-0.5 had almost no effect on jitter.

**Unverified on hardware:**
* The PF13 external-enable interlock against a real signal (moved from PF15 on 2026-09-17; nothing wired to PF13 on the bench since).
* `ARM` refusal and fault-on-loss for a genuinely enabled channel's ENA_OUT/CONTACT_OUT (enabling a channel on the bench trips overcurrent first).
* A real PC10 (hardware fault input) trip, and the general-fault ramp triggered by a real GateDriverStatus/PC10 signal (only software-injected so far).
* The overcurrent pin mapping for each channel was "to be defined later" when overcurrent was built; confirm it against the Transrex wiring.

**Unfinished:**
* The XREX external trigger is **polled** at 1 kHz; the owner suspects it should be interrupt-driven like HBRIDGE.
* XREX does not save `CONFig:DEADtime` in its profiles (HBRIDGE does). Its profile code is separate, so this needs its own change.
* The simulator build still links the whole controller stack (`sim_transrex.h` "KNOWN LIMITATION"); `XrexIo_EnableOutputsReadyToArm()` is not
  guarded for the simulator. The real fix is to make the controller-only polling controller-only.
* The state-machine "revisit" list from 2026-09-13: main-loop fault-poll cadence is not bounded by a timer; `PID:START` (the non-profile start)
  bypasses the state machine; confirm which of the original stubs are now filled (2026-09-15 and 2026-09-23 entries).
* `wham_console.py` still uses one current constant for its plot axis instead of the per-channel values.
* `HRTIM_NUM_CHANNELS` of 4 and 5 were proven at the command and build level only; no waveform capture of the added channels.
* The old `wham_llm_console.py` side project (a local-LLM front end for the console): low priority, and its answers were shown to be unreliable.

## 8.3 HBRIDGE

**Unverified on hardware:**
* **GDS polarity flip** (`GDS:POLarity`): fully checked on the Limiter; on the ECH only the first replies were seen before its network bridge
  dropped. The flip-and-clear check on the ECH was not repeated because the board is installed.
* **Dead time** at the pins: the command reads back correctly on all three boards; no logic-analyzer capture at, for example, 150 ns versus 100 ns.
* **Dead time and phase choice in profiles**: save, change, recall checked on the Limiter with real flash; not on the ECH.
* **CHARGE output** (PG0): never seen on a scope; the charger's input pull-down not confirmed.
* **External trigger** (PF15): never driven by a real rising edge (command layer only). Open questions for the owner: default on or off; noise
  immunity of the edge.
* **Limiter 100-125 kHz** and output-off gaps at Limiter frequencies; long gaps' interrupt load.
* **Closed loop on real analog inputs**; the three per-phase loops on real feedback.
* `DEBUG:FAULT:BYPASS` on hardware.

**Unfinished:**
* The **PF13 external-enable interlock** is not ported from XREX.
* **Telemetry events** (the XREX `!EVT` stream, port "phase 5") are not ported.
* The hub does not treat `*RCL` as dangerous, although a recall can now enable phases, change the GDS polarity and switch fault inputs off.
* Per-pin GDS polarity was offered, not requested; currently one polarity for all 12.
* The operator UI has no screens for closing loops or editing targets; HBRIDGE logs only closed-loop runs, so open-loop shot recording relies
  on the PFM input capture.
* `uart.c`/`uart.h` differ from XREX's since 2026-11-09 (`check_shared.py` reports it): reconcile or document as intentional.

## 8.4 Host, infrastructure and repository

* **Hub deployment.** The bench hubs run on `andrew` (instances for 5101/5102/5103); the plan in `docs/bench_devices.md` to rename the old
  hand-made instances needs an administrator with sudo. Running the hubs on the lab server "Jack" (the original plan,
  `docs/device_identity_and_host_service.md`) has not been done. Not started: a JSON API with reply matching, serial upstreams, a telemetry schema.
* **Uncommitted work in the tree at this snapshot** (other sessions): operator UI changes (`host/operator_ui/*`, `pfm_capture.py`), `host/wham_connect.py`
  (one command for tunnel plus hub session), hub changes and tests, three docs (`docs/device_identity_and_host_service.md`,
  `operator_hub_connection.md`, `operator_ui_guide.md`), and XREX changelog entries for them. The operator UI has been tested against
  simulated controllers and the hub, **not against real hardware**. Review and commit or discard deliberately.
* `firmware/hbridge/build/revert/` is untracked (an old image kept by hand); decide whether to keep it.
* The CubeIDE projects have not been re-imported from the monorepo location; the old standalone repos still exist and are behind.
* **Licensing:** documentation and hardware designs are unlicensed ("all rights reserved"); suggested CC BY-SA 4.0 and CERN-OHL-S-2.0, not chosen.
  Open: whether an employer or university owns the copyright; per-file SPDX headers; provenance of `firmware/xrex/docs/Transrex/`.
* `hardware/PFM-Controller_V4/PFMV4_Prelim_Files-main/` and the 391 MB third-party zip are deliberately untracked; decide.
* The pin map should move to `hardware/` with product columns and a board-revision changelog.
* The VTOR/MEMRMP boot fix was never ported to the older sibling projects (V3/V4 repositories); deliberately deferred.

## 8.5 Suggested order for a new engineer

1. Read `01`, `05` and the HBRIDGE operator's guide. Connect to the **Limiter** (bench, safe) through the hub and practise the read-only
   commands, then a firmware update with the owner present.
2. Fit or confirm the NRST pull-ups (6.1) on every board; record the option bytes of each board.
3. Close the cheap verification gaps with a logic analyzer on the Limiter: dead time, CHARGE, a real PF15 edge, 100-125 kHz.
4. Decide hub security (8.1) before the boards are used by more people.
5. Plan commissioning on the real supplies with the owner, starting with the calibration list (8.2).
