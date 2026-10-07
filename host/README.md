# Host software

- `provision_board.py` -- gives a controller its serial number (`SYS:SERial`), reads it back, and keeps the fleet registry
  `devices.csv` (created on the first real provisioning; commit it). `--check` compares a board with the registry without
  changing anything; `--list` prints the registry. See its docstring and section 3 of
  `docs/device_identity_and_host_service.md`. Unit tests: `python3 -m unittest discover -s host/tests`.
- The per-controller service (section 5 of the design doc) will live here too. Today's other host tools are in each firmware
  project's `python/` directory (`net_broker.py`, `net_client.py`, `fw_update.py`, `wham_console.py`).
