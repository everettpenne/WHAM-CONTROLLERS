# Host software

Python 3.8+, standard library only (PyYAML is used for configs if installed, but is not needed).

- **`wham_hub.py`** -- the per-controller service. One instance talks to one controller: it is the only client of that
  controller's serial-over-TCP bridge, shares it with any number of local clients (the same raw protocol as the firmware
  projects' `python/net_broker.py`, so `net_client.py`, `wham_console.py` and `fw_update.py` work through it unchanged),
  asks the board who it is (`SYS:ID?`) on connect and after every reboot, and logs every line with that identity to
  `<log_dir>/<serial>/YYYY-MM-DD.jsonl` (`host/logs/` by default). It sends one command at a time to the controller and gives
  each reply to the client that sent the command; a command that gets no reply is answered `ERR hub ...` after a timeout.
  It notices a dead link (TCP keepalive, and an `*IDN?` heartbeat when the controller is silent), reconnects, and disconnects a
  client that stops reading. With `expect_serial` set it refuses to forward commands to a different board.
  `!hub status`, `!hub clients`, `!hub identify`, `!hub quiet [on|off]`, `!hub confirm` and `!hub cancel` talk to the service itself.
  Several operators can be connected to one controller; interactive clients see who joins and what others send and receive, and a
  dangerous command (`dangerous_commands.py`) is announced to the other clients and logged with its category (optionally held until
  its sender confirms: `confirm_dangerous`). Design and framing details: `docs/device_identity_and_host_service.md`, section 5.1.
  There is no authentication and the default listen address is all interfaces: anyone who can reach the port can send
  commands, including ARM and FIRE. Set `allow_from` (IPs and CIDRs) or `listen_host: 127.0.0.1`; the service warns at startup
  when it listens beyond loopback without `allow_from`. Unit tests: `python3 -m unittest discover -s host/tests`.
- **`provision_board.py`** -- gives a controller its serial number (`SYS:SERial`), reads it back, and keeps the fleet registry
  `devices.csv`. `--check` compares a board with the registry without changing anything; `--list` prints the registry.
- **`dangerous_commands.py`** -- the list of dangerous commands (what starts output, weakens a protection, reflashes, ...), matched the way the firmware matches.
- `config/` -- one YAML file per controller (`example.yaml`); `deploy/wham-hub@.service` -- the systemd template unit.
- Logs go to `host/logs/` and the registry to `host/devices.csv`, next to the scripts, unless the config says otherwise. On a server set `log_dir`
  outside the install tree; `log_keep_days` deletes day files older than that many days.

## Running

```
python3 host/wham_hub.py --config host/config/magnet2.yaml                   # one controller
python3 host/wham_hub.py --host 192.168.130.138 --listen-port 5101 --name magnet2
python3 firmware/hbridge/python/net_client.py --port 5101 --name me          # a client: type SCPI lines
python3 host/provision_board.py --url socket://localhost:5101 --check         # through the service
```

On a Linux server, run one instance per controller under systemd: `deploy/wham-hub@.service` (`systemctl enable --now wham-hub@magnet2`).
Edit its `User` (an account that exists on the server) and install path first, and put `log_dir` outside the install tree.
Today's other host tools are in each firmware project's `python/` directory.

## Writing a client (LabVIEW, scripts): quiet mode

A program should connect in quiet mode so that it gets a clean stream. Send `@quiet <name>` as the first line (the name is optional) (or `!hub quiet on` later;
the service answers `# hub: quiet on`), then one command per line ending in CR LF (a lone CR also ends a command). In quiet mode the service sends only:

- the reply to each of your commands, in the order you sent them: one line, `OK ...` or `ERR ...`. Your command may wait behind other
  clients' commands (the controller handles one at a time), so allow for it in your read timeout;
- unsolicited controller lines, which start with `!` (`!EVT ...`, `!BOOT ...`) and can arrive at any time, even between a command and its
  reply: skip them (or use them), they are never a reply;
- `#` lines, which answer your own `!hub ...` commands and are never a reply to a controller command.

`ERR hub ...` is the service speaking, not the controller: `timeout` (no reply in time; the command may or may not have run),
`the controller restarted before it replied` (or `before it was sent`), `the connection to the controller was lost`, `the controller is not connected`,
`identity mismatch`, `queue full`, or a dangerous command held for confirmation. Two controller commands reply over several lines:
`SYS:EVLOG?` (`OK <n>`, then n `!EVT` lines) and `CONFig:PROFile:CATalog?` when a slot cannot be read (a short `OK` line, then an `ERR` line).
A restart (`!BOOT`) part way through such a reply ends it with no marker: count on fewer lines then (the service log notes how many were missing).
A one-shot client may stop sending after its last command (`printf ... | nc`): it still gets every reply, then the service
closes the connection. A complete line runs even if its client disconnects before the reply.
A quiet client gets no notice when the link drops: it sees `ERR hub ...` for its next command, or `!hub status` (`connected=`, `rx_age=`).
A reply that arrives after its command timed out never reaches a quiet client: the service holds the queue until a resync query shows
the late line has passed, and reconnects when the controller stays silent. After `ERR hub timeout` the command may or may not have run.
