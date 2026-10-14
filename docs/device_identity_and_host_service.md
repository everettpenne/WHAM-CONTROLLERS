# Device identity and the per-controller host service

Status: design, 2026-10-07. Step 1 of section 6 (read-only identity in the firmware) is implemented in both products and verified
on the simulator board; everything else is planned. Commands live under `SYS:` (XREX already has `SYS:TIME?` and friends).

## 1. Goal

Several copies of the same board will be deployed, each controlling a different system. Each board must carry a unique identifier
that survives power cycles and firmware updates and that every message it produces can be traced to. A host service on a lab server
must carry all communication with each board (SCPI commands and replies, unsolicited events, telemetry) and record which board it
is talking to, without depending on a port name or an IP address staying the same.

Not a goal: access control inside the firmware, or anything that changes how a controller behaves. Identity is reported, never
enforced; a mismatch can never fault or inhibit a supply.

## 2. Two kinds of identity

| | Hardware ID | Board serial |
|---|---|---|
| What | proof of which physical board this is | the name on the label, e.g. `WB-0007` |
| Source | STM32 factory 96-bit unique ID; W25Q128 factory 64-bit unique ID | written to flash once, at provisioning |
| Changes | never | rarely (explicit clear, then set) |
| Needs provisioning | no | yes |

The hardware ID makes every board unique from the first day. The serial is for people, logs and plots. When the stored serial was
written for different silicon than the board now has (a cloned or swapped flash chip), the firmware reports `verified=0`.

**Where a board is used** ("this controller runs magnet system 2") is host configuration, not board data: `systems.yaml` maps
serial to system. Replacing a board then means editing one file. The board may carry an optional text `label` as an offline
convenience, but the host file is the authority.

Hardware-ID notes (found on hardware, 2026-10-07): the MCU UID is read with `HAL_GetUIDw0/1/2`. The W25Q128's unique-ID command (`0x4B`) sends four
dummy bytes (32 clocks) before the 64-bit ID, which the chip then repeats; the QUADSPI's dummy-cycle field holds only 0..31, so the firmware reads
12 bytes with no dummy cycles and drops the first four. (A first version used 32 dummy cycles, which wrapped to 0: the first four "ID" bytes changed
from boot to boot.) The simulator board's chip reads `E4653C34CB172E22`.

## 3. Storage

- A reserved identity region at the top of the 16 MiB chip: two 4 KiB sectors, `0xFFD000` and `0xFFE000` (the last sector, `0xFFF000`, is `NVSTORE_SCRATCH_ADDR`, the `QSPI:SELFtest` scratch sector that the bench test erases freely), alternating so that a power
  cut during an update loses nothing. Recorded in each product's flash map (`docs/saved_profiles_shared.md`); today XREX uses
  0-64 KiB and HBRIDGE 64-320 KiB.
- Record (88 bytes): magic `WID1`, version, sequence number (the newer copy wins), serial (3-16 characters, `A-Z 0-9 -`), label (1-32 characters of
  `A-Z a-z 0-9 . _ -`, no spaces so `SYS:ID?` stays parseable), the MCU and flash unique IDs the serial was written against, CRC-32.
  The firmware checks only the serial's format; the `WB-NNNN-C` scheme with its Luhn check digit is the provisioning tool's.
- Independent of everything else: profile `*CLEar`, firmware updates and the other product's firmware never touch it. `nvstore` refuses
  any erase or program of those sectors (`NVSTORE_ERR_RESERVED`) except while the identity module has opened it for its own write, and
  `profile_store.h` refuses to build if the profile slots reach them.
- Shared code, byte-identical in both products (`check_shared.py`), like `nvstore` and `profile_store`.
- Provisioning is write-once by default: `SYS:SERial <sn>` is refused when a serial exists (`ERR 19`) or the state is not IDLE (`ERR 13`); changing it
  takes `SYS:ID:CLEar CONFIRM` first. `host/provision_board.py` is the way to do it (section 6, step 2). Burning the serial into the chip's one-time-programmable security registers is possible later and is
  deliberately not done now (irreversible).

## 4. What the board says

- `SYS:ID?` -> `OK serial=WB-0007 uid=<24 hex> flashuid=<16 hex> product=HBRIDGE hw=REVA fw=v0.9 commit=<hash> label=<text|-> verified=<0|1> identity=<unset|ok|corrupt|nochip>`
  (`serial=UNSET` and `verified=0` until provisioned).
- `*IDN?` and the `!BOOT` banner gain the identity at the end of the line; existing parsers read the fields they know.
- The board does NOT prefix every line with its identity: the host knows which connection a line came from and tags it. This keeps
  the one-line-per-packet framing of `firmware/xrex/docs/telemetry.md`.
- A `!BOOT` line marks a new epoch. The host counts epochs per device, so `!EVT <tick_ms>` timestamps stay unambiguous after a reset.
  No boot counter is kept in flash.

## 5. The host service

Decision (lab networking guidance): ONE script that takes an IP address or a YAML file, run as a service on the lab server, one
service instance per controller. Chosen because it isolates failures (a hung board stalls only its own instance), keeps the
script small (one board, one session), matches the bridge (each board's serial-over-TCP bridge accepts exactly one client, so
the service is that client and everything else connects to the service) and lets the server restart and log it.

```
 clients (wham_console, LLM console, loggers, dashboards)
        |  local port per instance, newline-delimited text or JSON
 +------+-----------------------------------------------+
 | service instance  (script + one YAML)                  |
 |  handshake: SYS:ID? -> bind to a serial             |
 |  one command in flight, timeouts, !BOOT -> new epoch   |
 |  JSONL log: logs/<serial>/YYYY-MM-DD.jsonl             |
 |  who is connected + dangerous-command list              |
 +------+-----------------------------------------------+
        |  TCP bridge or serial port
     controller (serial WB-0007)
```

YAML (per instance): endpoint (host and port, or a serial device), `expect_serial` (optional; a mismatch raises an alarm and
refuses commands, which catches swapped cables), a name, the listen port, the log directory.

- Identity is learned by handshake, never configured: a moved IP or a renamed serial port cannot silently talk to the wrong system.
- The protocol has no request IDs, so the service sends one command at a time and gives each reply to the command's sender (section 5.1 has
  the framing, including the two multi-line replies and the per-command timeouts).
- Several operators may be connected to one controller at once (decision 2026-10-07: no write lease for now, so a bench computer can run
  tests and reflash in parallel with the operator's, with in-person guardrails for safety). To keep that safe in practice the service gives
  everyone visibility: joins and leaves are announced with the client's name and address, `!hub clients` lists who is connected, how long,
  how recently they sent something and what, and every interactive client sees other clients' commands and replies (a quiet client sees only its own). A real lease (one writer, many observers) stays
  possible later without changing the clients.
- Dangerous commands (`host/dangerous_commands.py`): the commands that start or enable output (`FIRE`, `SOURce:RUN`, `SHOT:STARt`, an enable or
  DAC output turned on, ...), `ARM`, switch a protection off or change how faults are read or limited, force or clear a fault, reflash or reset the
  controller (`FWUPdate:BEGin/SWAP/ROLLback`, `BOOT`), or erase the identity. The list matches commands exactly as the firmware's parser does
  (short/long forms), never flags queries or the safe direction (an output turned off), and counts a missing or garbled direction as dangerous.
  The service logs each with its category, announces it to every OTHER client (`--- hub: DANGEROUS [output] bob@10.0.0.5: FIRE ---`) and counts it
  per client. It does not block by default; with `confirm_dangerous` on, the command is held until its sender types `!hub confirm` (or dropped by
  `!hub cancel`, or expired) -- off by default because scripts and `fw_update.py` cannot confirm. Tests check the list against both products'
  real command tables and against `wham_console.py`'s older, narrower `_is_dangerous()`, of which it is a superset.
- Several instances can publish to a shared message broker (for example MQTT, `wham/<serial>/...`) for one stream across all
  devices, without a process in the middle. Optional, later.
- The protocol has no authentication, so a service port must be reachable only from the host itself or an access-restricted lab
  network (`allow_from`, `listen_host`, the firewall); remote operators go through ssh or whatever the lab already trusts. This matters because the commands include ARM and
  FIRE on a high-voltage supply. The ssh tunnel used from a laptop today is not part of this design; a service inside the lab
  network reaches the bridges directly.
- On the server: with Linux, a systemd template unit `wham-hub@<name>.service` reading `/etc/wham/<name>.yaml`; containers (one per
  controller) are equivalent.

### 5.1 Service v1 as built

`host/wham_hub.py` (stdlib only; `host/README.md`; every config key is in `host/config/example.yaml`). Per instance: the controller's
bridge address (fixed IP, from the lab) and a listen port; defaults written next to the script (`host/logs/<serial>/YYYY-MM-DD.jsonl`,
`host/devices.csv`).

**Access.** There is no authentication. The default listen address is all interfaces, as agreed for now. `allow_from` (a list of IPs
and CIDRs) refuses every other source address, and each refusal is logged. With a non-loopback `listen_host` and no `allow_from` the
service prints a warning at startup. `listen_host: 127.0.0.1` keeps it to the host itself. A deployed config lists each client
machine by address (the LabVIEW PC, a bench laptop), never a whole subnet; the example allows only 127.0.0.1.

**Clients and the controller.** Clients use the raw protocol of `net_broker.py` (optional first line `@name x`, one command per line), so
`net_client.py`, `wham_console.py` and `fw_update.py` work through the service unchanged, including a firmware update with its reboot
(`host/tests/test_hub_with_tools.py` runs the real tools). `!hub status` / `!hub identify` / `!hub clients` / `!hub quiet` ask the service
itself; its answers start with `#`.

- One command is in flight to the controller at a time, in arrival order across all clients. The service's own queries (the handshake, the
  heartbeat) use the same queue and go first. Every command a client sends gets exactly one reply line, to that client:
  the controller's line, or `ERR hub ...` when there is none: `timeout` (`command_timeout_s`, 5 s; `long_command_timeout_s`, 30 s for the
  commands in `REPLY_TABLE`), `the controller restarted before it replied`, `the connection to the controller was lost`, `identity mismatch`,
  `queue full` (`queue_max`), and for a quiet client also `the controller is not connected` and a dangerous command held for confirmation.
  A blank line is passed on and not counted as a command, because the firmware ignores it. A client's line ends at CR, LF or CR LF, as in
  the firmware, so `A\rB\r\n` is two commands with a reply each, and a client that ends lines with CR alone is served. A
  line cut off before its end by the client closing the connection is not sent (the firmware would not run it either) and is logged as
  `refused`. A complete line is a command issued: it runs even if its client disconnects first, and a client that only
  stops sending (`printf ... | nc`) gets every reply before the service closes the connection.
- Reply framing is the firmware's. Each non-blank command gets one `OK`/`ERR` line ending in CR LF; unsolicited lines start with `!`
  (`!EVT`, `!BOOT`). While a command is in flight, the next line that does not start with `!` is its reply, even a garbled one (a byte lost on
  the bridge turns `OK` into `O`; `fw_update.py` resynchronizes on that). `LOG:DATA?`, `PFMIN:DATA?`, `PFMIN:RAW?` and `PFM:GAPLOG?` are one long line. Two
  replies span lines, and the queue is held until their last line arrives (or the long timeout passes): `SYS:EVLOG?` is `OK <n>` and
  then n `!EVT` lines, which are also shown to every client as events; `CONFig:PROFile:CATalog?` ends its OK line short and follows it with
  an `ERR` line when a slot cannot be read. A `!BOOT` banner ends the command in flight with an error reply (a dump under way is only cut
  short, its sender already has its reply), answers each command still queued or held by a handshake with `ERR hub the controller restarted before it was sent`
  instead of sending it to the board that came back, and holds new commands until the handshake after the banner is done. After a timeout the queue waits for a resync query (`*IDN?`, or `SYS:ID?` when the command that
  timed out was `*IDN?`) whose reply cannot be mistaken for the late one, so every line before it, the late reply included, belongs to
  nobody; no answer to it closes the connection and reconnects. The service's own queries take only their own reply (`*IDN?` an
  `OK WHAM-...` line, `SYS:ID?` `OK serial=...` or `ERR`), so stray text is never taken for the identity. A garbled reply to one
  of them counts as stray too: the heartbeat then reconnects, and a failed handshake leaves the board unidentified (refused when
  `expect_serial` is set) and is retried every `heartbeat_s` until it succeeds; a banner, reconnect or `!hub identify` also retries it. A line that belongs to nobody is
  logged and shown to the interactive clients only, never to a quiet one.
- Interactive clients (the default) see everything: the controller's lines, their own replies bare, other clients' commands as
  `[name] > cmd` and their replies as `[name] < reply`, joins and leaves, and hub notes (`--- hub: ... ---`). A quiet client (first line
  `@quiet x`, or `!hub quiet on` at any time, answered `# hub: quiet on`; `!hub quiet off` ends it) gets no greeting, echoes or notes: only
  the replies to its own commands, the unsolicited `!` lines (`!EVT`, `!BOOT`) and the `#` answers to its own `!hub`
  commands. It learns of a dead link from `ERR hub` replies or from `!hub status` (`connected=`, `rx_age=`, `inflight=`, `queued=`).
- Link liveness. The bridge socket has TCP keepalive (`keepalive_idle_s` 10, `keepalive_interval_s` 3, `keepalive_count` 3; the Linux
  options, guarded by `hasattr`). When nothing has been received for `heartbeat_s` (15 s) the service sends `*IDN?` (answered by both
  products); no answer within `heartbeat_timeout_s` (5 s) closes the connection, with a hub note, and reconnects. A board reset during
  the query does not stop the heartbeat. A talkative link is never probed.
- Output buffering. A client with more than `client_buffer_max` (1 MiB) bytes waiting to be written is disconnected with a hub note.

**Identity.** `*IDN?` and `SYS:ID?` at connect and after each `!BOOT WHAM-...` line (a stray byte in front of the banner is tolerated);
the log key is the serial, or `UNSET-<last 8 of the MCU ID>` for a board with none. Records written before the first handshake settles
are held and written under the identity found. `expect_serial` mismatch, or the registry holding that serial for different hardware, makes
the service answer `ERR hub identity mismatch: ...` instead of forwarding.

**Log.** Record `{t, ts, dev, serial, name, epoch, session, kind, line[, client][, danger][, truncated]}`; kinds `cmd`, `reply` (`client` names
the sender when the reply answers one of its commands), `event` (`!EVT`), `boot` (`!BOOT`), `data` (anything else), `refused`,
`hub`/`hub-cmd`/`hub-reply` (the service's own, heartbeat included). `epoch` counts `!BOOT` banners, `session` counts connections. Lines are
cut at 400 characters; `FWUPdate:DATA` keeps only its first 24. `log_keep_days` deletes day files older than that many days when a new
day file opens (0 keeps everything).

**Visibility and dangerous commands**: see the bullets in section 5 above; `!hub status` shows `who=` and `confirm=`. The echo and the
announcement are sent when the command goes to the controller, so every observer sees them in the order the controller does.

**Reconnect and deployment.** Reconnects forever (`reconnect_s`, 3 s). Commands sent while the controller is away, and commands queued
when it goes, are answered `--- hub: the controller is not connected ---` (`ERR hub ...` to a quiet client).
`host/deploy/wham-hub@.service` is a systemd template (`systemctl enable --now wham-hub@<name>` reads `host/config/<name>.yaml`). Its
`User` and install path are placeholders to set for the server, and `log_dir` belongs outside the install tree (`StateDirectory=wham/hub`
provides `/var/lib/wham/hub`, leaving any other `/var/lib/wham` content alone). Written but not yet run on a server. Flashing through the ROM bootloader (`net_flash.py`) is not a line protocol and
needs the service stopped.

## 6. Build order

1. Read-only identity, no flash writes: UID and flash-unique-ID readers, `SYS:ID?`, identity in `*IDN?` and `!BOOT`. (Done 2026-10-07.)
2. (Done 2026-10-07.) Persistent serial: identity region, `SYS:SERial`/`LABel`/`ID:CLEar`, host tests (CRC, two-copy recovery, bind check, serial
   validation), `provision_board.py` and a `devices.csv` registry (serial, UIDs, product, date, firmware).
3. Service v1 (`host/wham_hub.py`): one controller per instance, handshake binding, JSONL log, raw passthrough so today's tools keep working. (Done 2026-10-07; see section 5.1.)
4. Leases, permissions, firmware update through the service.
5. Telemetry schema: self-delimited dumps, a time-series store (the pending phases of `telemetry.md`), the optional broker.

## 7. Repository layout

`WHAM-CONTROLLERS`: `firmware/xrex`, `firmware/hbridge` (complete projects with their histories, imported 2026-10-07), `hardware/`,
`host/`, `docs/`. Shared firmware code is still copied and guarded by `check_shared.py`; moving it into a `firmware/platform/`
library is a separate, later decision (see each project's AGENTS.md).
