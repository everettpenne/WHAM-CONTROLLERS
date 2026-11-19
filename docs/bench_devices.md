# Bench devices and their hubs

The three controllers on the bench network, the serial-to-Ethernet module (WIZnet) each one sits behind, and the hub
(`host/wham_hub.py`) that serves it on `andrew`. The operator UI (`host/operator_ui/`) lists the same three by name.

| Name (UI card) | Firmware | Bridge MAC | Bridge IP | Hub instance | Hub port | Status |
|---|---|---|---|---|---|---|
| WHAM-PFM-G474-XREX-CTRL | XREX | 00:08:DC:74:8A:9E | 192.168.130.138 | `xrex-ctrl` | 5101 | running |
| WHAM-PFM-G474-LMTR-CTRL | HBRIDGE (Limiter) | 00:08:DC:74:8B:D7 | 192.168.130.253 | `lmtr-ctrl` | 5102 | running |
| WHAM-PFM-G474-ECH-HVPS | HBRIDGE (ECH gyrotron HVPS, board WB-0001-8) | 00:08:DC:74:8D:BB | 192.168.130.250 | `ech-hvps` | 5103 | running |

The hub configs are `host/config/xrex-ctrl.yaml`, `lmtr-ctrl.yaml` and `ech-hvps.yaml`. On 2026-11-04 the two running hubs used
hand-made, untracked files on `andrew` (`bench2.yaml`, `xrexsim.yaml`) with the same addresses and ports; the tracked files
replace them and only change the instance names, so ports and the operators' ssh tunnels stay the same.

The Limiter's bridge is a WIZnet WIZ5XXSR-RP (firmware 1.0.5): static IP, TCP server mode on port 5000, 8 data bits, no parity,
1 stop bit, no flow control. A WIZnet server accepts ONE client, so only the hub may be connected to it.

## How the hubs run on `andrew`

Three systemd services, `wham-hub@xrex-ctrl`, `wham-hub@lmtr-ctrl` and `wham-hub@ech-hvps` (unit file
`host/deploy/wham-hub@.service`: `User=whamdata`, `Restart=always`), run `/opt/WHAM-CONTROLLERS/host/wham_hub.py` with
`host/config/<name>.yaml`. The renaming from the hand-made `bench2`/`xrexsim` services and the start of the ECH hub were done
by the administrator; the three services have run since 2026-11-09.

**This site's own settings** (the extra allowed client addresses, `expect_serial`) go in `host/config/<name>.local.yaml` next
to the tracked file: git ignores those files, and a key there replaces the same key in the tracked file. Never edit the
tracked `.yaml` files on the server: an edit there can block an update.

## Updates: the hubs follow the `deploy` branch

The server's checkout updates itself, with no sudo, from the `whamdata` crontab (`host/deploy/hub_update.py`, whose header
has the full description):

- **03:30 every day, `hub_update.py update`:** fetches the `deploy` branch of `everettpenne/WHAM-CONTROLLERS`. If it moved,
  the new commit is tested first in a temporary folder: the hub tests, and every running hub's config loaded by the new code
  (`wham_hub.py --check`). Only if both pass does the checkout switch to it. A commit that fails is logged and not tried
  again until `deploy` moves.
- **Every 15 minutes, `hub_update.py restart`:** a hub still running the older code is restarted (SIGTERM; systemd starts it
  again on the new code) **only when it is quiet**: no client other than the updater, and its controller not ARMED or
  FIRING. Otherwise it waits for the next run. Restarting a hub never touches the controller: a shot in progress carries on,
  and only the hub's connections drop (clients reconnect).
- If a restarted hub does not listen and answer `!hub status` within 60 s, the checkout goes back to the previous commit and
  the commit is marked bad.

Releasing a change to the server is a deliberate step: pushing to `main` does nothing there.

```
git push origin <commit>:deploy          # the server picks it up at the next 03:30
```

On the server, as `whamdata`:

```
python3 /opt/WHAM-CONTROLLERS/host/deploy/hub_update.py status      # commit, deploy, each hub, pending restarts, local edits
python3 /opt/WHAM-CONTROLLERS/host/deploy/hub_update.py update      # now instead of at 03:30
python3 /opt/WHAM-CONTROLLERS/host/deploy/hub_update.py rollback    # back to the commit before the last update
python3 /opt/WHAM-CONTROLLERS/host/deploy/hub_update.py install-cron # or uninstall-cron
```

The log is `/var/lib/wham/hub/update/update.log` (cron's own output: `cron.log` beside it).

## Open items

- The ECH-HVPS also reports `CONFig:MODE? = NONE` (no mode selected yet). The Limiter board reports `CONFig:MODE? = NONE`: no Limiter/HVPS mode has been selected on it yet.
- Both bench boards still report `SN=UNSET`; `python3 host/provision_board.py` gives them serials, and recordings are filed
  under the serial (until then under "unassigned-...").
- The ECH-HVPS bridge was first given as 00:08:DC:74:8A:95 (swapped out of service) and then mistyped as …8D:BF; the module in
  use is 00:08:DC:74:8D:BB. Only one client can be connected to it at a time: do not leave a terminal on 192.168.130.250:5000
  once the hub is running.
