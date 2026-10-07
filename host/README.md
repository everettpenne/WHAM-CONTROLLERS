# Host software

Python 3.8+, standard library only (PyYAML is used for configs if installed, but is not needed).

- **`wham_hub.py`** -- the per-controller service. One instance talks to one controller: it is the only client of that
  controller's serial-over-TCP bridge, shares it with any number of local clients (the same raw protocol as the firmware
  projects' `python/net_broker.py`, so `net_client.py`, `wham_console.py` and `fw_update.py` work through it unchanged),
  asks the board who it is (`SYS:ID?`) on connect and after every reboot, and logs every line with that identity to
  `host/logs/<serial>/YYYY-MM-DD.jsonl`. With `expect_serial` set it refuses to forward commands to a different board.
  `!hub status` and `!hub identify` talk to the service itself. Design: `docs/device_identity_and_host_service.md`.
  There is no authentication and the default listen address is all interfaces: anyone who can reach the port can send
  commands. Unit tests: `python3 -m unittest discover -s host/tests`.
- **`provision_board.py`** -- gives a controller its serial number (`SYS:SERial`), reads it back, and keeps the fleet registry
  `devices.csv`. `--check` compares a board with the registry without changing anything; `--list` prints the registry.
- `config/` -- one YAML file per controller (`example.yaml`); `deploy/wham-hub@.service` -- the systemd template unit.
- Logs go to `host/logs/` and the registry to `host/devices.csv`, next to the scripts, unless the config says otherwise.

## Running

```
python3 host/wham_hub.py --config host/config/magnet2.yaml                   # one controller
python3 host/wham_hub.py --host 192.168.130.138 --listen-port 5101 --name magnet2
python3 firmware/hbridge/python/net_client.py --port 5101 --name me          # a client: type SCPI lines
python3 host/provision_board.py --url socket://localhost:5101 --check         # through the service
```

On a Linux server, run one instance per controller under systemd: `deploy/wham-hub@.service` (`systemctl enable --now wham-hub@magnet2`).
Today's other host tools are in each firmware project's `python/` directory.
