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

UID address `0x1FFF7590` (RM0440) and the W25Q128JV unique-ID command `0x4B` (four dummy bytes, then 64 bits) are written from
memory. The UID read and `SYS:ID?` work on hardware; the flash chip's ID read returned `00000000E4653C34` on the first board (top four bytes
zero), which is to be compared with a second chip before it is relied on as a 64-bit key.

## 3. Storage

- A reserved identity region at the top of the 16 MiB chip: two 4 KiB sectors, `0xFFD000` and `0xFFE000` (the last sector, `0xFFF000`, is `NVSTORE_SCRATCH_ADDR`, the `QSPI:SELFtest` scratch sector that the bench test erases freely), alternating so that a power
  cut during an update loses nothing. Recorded in each product's flash map (`docs/saved_profiles_shared.md`); today XREX uses
  0-64 KiB and HBRIDGE 64-320 KiB.
- Record: magic `WID1`, version, serial (at most 16 characters, `A-Z 0-9 -`, with a check character), label (at most 32),
  the hardware IDs the serial was written against, CRC-32.
- Independent of everything else: profile `*CLEar`, firmware updates and the other product's firmware never touch it. `nvstore`
  gets a range guard so no other caller can erase or program those sectors.
- Shared code, byte-identical in both products (`check_shared.py`), like `nvstore` and `profile_store`.
- Provisioning is write-once by default: `SYS:SERial <sn>` is refused when a serial exists or the state is not IDLE; changing it
  takes `SYS:ID:CLEar` first. Burning the serial into the chip's one-time-programmable security registers is possible later and is
  deliberately not done now (irreversible).

## 4. What the board says

- `SYS:ID?` -> `OK serial=WB-0007 uid=<24 hex> flashuid=<16 hex> product=HBRIDGE hw=REVA fw=v0.9 commit=<hash> label=<text> verified=1`
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
 |  write lease + dangerous-command list                  |
 +------+-----------------------------------------------+
        |  TCP bridge or serial port
     controller (serial WB-0007)
```

YAML (per instance): endpoint (host and port, or a serial device), `expect_serial` (optional; a mismatch raises an alarm and
refuses commands, which catches swapped cables), a name, the listen port, the log directory.

- Identity is learned by handshake, never configured: a moved IP or a renamed serial port cannot silently talk to the wrong system.
- Replies match commands by order (the protocol has no request IDs). Multi-line dumps (`LOG:DATA?`, `PFMIN:DATA?`) end according to a
  small per-command table until the firmware frames them itself (pending telemetry phases).
- One write lease per instance: one writer, any number of observers. Dangerous commands (`ARM`, `SHOT:STARt`, `FIRE`, `BOOT`,
  `FWUPdate:*`) need the lease and come from the same list `wham_console.py` already uses (one list, not two).
- Several instances can publish to a shared message broker (for example MQTT, `wham/<serial>/...`) for one stream across all
  devices, without a process in the middle. Optional, later.
- The protocol has no authentication, so a service port must be reachable only from the host itself or an access-restricted lab
  network; remote operators go through ssh or whatever the lab already trusts. This matters because the commands include ARM and
  FIRE on a high-voltage supply. The ssh tunnel used from a laptop today is not part of this design; a service inside the lab
  network reaches the bridges directly.
- On the server: with Linux, a systemd template unit `wham-hub@<name>.service` reading `/etc/wham/<name>.yaml`; containers (one per
  controller) are equivalent.

## 6. Build order

1. Read-only identity, no flash writes: UID and flash-unique-ID readers, `SYS:ID?`, identity in `*IDN?` and `!BOOT`. (Done 2026-10-07.)
2. Persistent serial: identity region, `SYS:SERial`/`LABel`/`ID:CLEar`, host tests (CRC, two-copy recovery, bind check, serial
   validation), `provision_board.py` and a `devices.csv` registry (serial, UIDs, product, date, firmware).
3. Service v1 (`host/`): one controller per instance, handshake binding, JSONL log, raw passthrough so today's tools keep working.
4. Leases, permissions, firmware update through the service.
5. Telemetry schema: self-delimited dumps, a time-series store (the pending phases of `telemetry.md`), the optional broker.

## 7. Repository layout

`WHAM-CONTROLLERS`: `firmware/xrex`, `firmware/hbridge` (complete projects with their histories, imported 2026-10-07), `hardware/`,
`host/`, `docs/`. Shared firmware code is still copied and guarded by `check_shared.py`; moving it into a `firmware/platform/`
library is a separate, later decision (see each project's AGENTS.md).
