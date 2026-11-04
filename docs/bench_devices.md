# Bench devices and their hubs

The three controllers on the bench network, the serial-to-Ethernet module (WIZnet) each one sits behind, and the hub
(`host/wham_hub.py`) that serves it on `andrew`. The operator UI (`host/operator_ui/`) lists the same three by name.

| Name (UI card) | Firmware | Bridge MAC | Bridge IP | Hub instance | Hub port | Status |
|---|---|---|---|---|---|---|
| WHAM-PFM-G474-XREX-CTRL | XREX | 00:08:DC:74:8A:9E | 192.168.130.138 | `xrex-ctrl` | 5101 | running (as `bench2`) |
| WHAM-PFM-G474-LMTR-CTRL | HBRIDGE (Limiter) | 00:08:DC:74:8B:D7 | 192.168.130.253 | `lmtr-ctrl` | 5102 | running (as `xrexsim`) |
| WHAM-PFM-G474-ECH-HVPS | HBRIDGE (ECH gyrotron HVPS, board WB-0001-8) | 00:08:DC:74:8D:BB | 192.168.130.250 | `ech-hvps` | 5103 | bridge and board answer (2026-11-04); hub not started yet |

The hub configs are `host/config/xrex-ctrl.yaml`, `lmtr-ctrl.yaml` and `ech-hvps.yaml`. On 2026-11-04 the two running hubs used
hand-made, untracked files on `andrew` (`bench2.yaml`, `xrexsim.yaml`) with the same addresses and ports; the tracked files
replace them and only change the instance names, so ports and the operators' ssh tunnels stay the same.

The Limiter's bridge is a WIZnet WIZ5XXSR-RP (firmware 1.0.5): static IP, TCP server mode on port 5000, 8 data bits, no parity,
1 stop bit, no flow control. A WIZnet server accepts ONE client, so only the hub may be connected to it.

## What the administrator needs to do on `andrew`

The `whamdata` account has no sudo, so these steps need an account that has it. Nothing here changes the controllers.

1. **Update the checkout** `/opt/WHAM-CONTROLLERS` to the current `main` of `everettpenne/WHAM-CONTROLLERS` (it holds
   `host/config/xrex-ctrl.yaml`, `lmtr-ctrl.yaml`, `ech-hvps.yaml`). Today it is at `e533f3a` with `bench2.yaml` and `xrexsim.yaml`
   untracked in `host/config/`; leave them in place until step 3 is done.
2. **Check that the unit file** `/etc/systemd/system/wham-hub@.service` is unchanged (`User=whamdata`, paths under
   `/opt/WHAM-CONTROLLERS`). `host/deploy/wham-hub@.service` is the reference; no edit is needed for the new names.
3. **Rename the two running hubs.** Each moves the controller's one bridge connection from the old service to the new one,
   so do one at a time, and expect a few seconds with no hub for that controller. Nobody should be mid-shot:

   ```
   sudo systemctl disable --now wham-hub@bench2
   sudo systemctl enable --now  wham-hub@xrex-ctrl
   sudo systemctl disable --now wham-hub@xrexsim
   sudo systemctl enable --now  wham-hub@lmtr-ctrl
   ```

   Stop the old service BEFORE starting the new one: the bridge accepts one client, so a second hub is refused until the first
   lets go. Then check `systemctl status wham-hub@xrex-ctrl wham-hub@lmtr-ctrl` and ask each hub `!hub status` (it should say
   `connected=1`). Restarting also loads whatever hub code the updated checkout contains; the running hubs were started on
   2026-10-26.
4. **Remove the old configs** once the new services work: `host/config/bench2.yaml` and `host/config/xrexsim.yaml` (keep a
   copy if wanted; they are untracked).
5. **Start the ECH-HVPS hub.** Its bridge (WIZnet WIZ5XXSR-RP, MAC 00:08:DC:74:8D:BB) is already on the network at
   192.168.130.250 with the same settings as the Limiter's (static IP, TCP server on port 5000, 8N1, no flow control), and on
   2026-11-04 the board behind it answered `*IDN?` as WHAM-HBRIDGE-PFMG474 v0.10, SN=WB-0001-8, label ECH-HVPS. `host/config/ech-hvps.yaml`
   already points at it, so:
   - `sudo systemctl enable --now wham-hub@ech-hvps`;
   - confirm with `!hub status` on port 5103: `connected=1`, `product=WHAM-HBRIDGE-PFMG474`, `serial=WB-0001-8`.
6. **Operators' tunnels** keep working for 5101 and 5102. To use the ECH hub from a laptop, add `-L 5103:localhost:5103` to the
   ssh command.

## Open items

- The ECH-HVPS also reports `CONFig:MODE? = NONE` (no mode selected yet). The Limiter board reports `CONFig:MODE? = NONE`: no Limiter/HVPS mode has been selected on it yet.
- Both bench boards still report `SN=UNSET`; `python3 host/provision_board.py` gives them serials, and recordings are filed
  under the serial (until then under "unassigned-...").
- The ECH-HVPS bridge was first given as 00:08:DC:74:8A:95 (swapped out of service) and then mistyped as …8D:BF; the module in
  use is 00:08:DC:74:8D:BB. Only one client can be connected to it at a time: do not leave a terminal on 192.168.130.250:5000
  once the hub is running.
