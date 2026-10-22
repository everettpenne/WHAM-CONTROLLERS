# LabVIEW programmer's guide: talking to the controllers through the hub

For the person writing LabVIEW (or any other program) that monitors and commands the WHAM controllers.
This first part covers **how to connect to the hub and exchange commands with a controller through it**. Part 2
(`labview_state_and_faults.md`) covers the operating states and faults, and part 3 (`labview_commands.md`) the command sets; section 11
below lists them.

Nothing here depends on a particular controller, address or serial number. Those change; the hub's protocol does not. Where a
value must come from the lab (a host name, a port) the text says so.

---

## 1. The picture

```
 your LabVIEW program  --TCP-->  hub (one per controller)  --TCP-->  serial bridge  --UART-->  controller
 other programs/people --TCP-->        |
                                       +--> log of every line, in both directions
```

- Each controller has a **bridge** that turns its serial port (USART2) into a TCP socket. A bridge accepts **exactly one**
  connection, so nobody connects to a bridge directly any more.
- The **hub** is a small service that owns that one connection. It runs on a server, one instance per controller, each on its own
  TCP port. **You connect to the hub, never to the bridge.**
- The hub lets several programs and people be connected to the same controller at once, sends their commands to the controller
  one at a time, hands every reply back to the program that sent the command, and logs everything with the controller's identity.
- The controller and its hardware protections work on their own. If the hub is down you lose your command and monitoring path,
  nothing else; the hub is never the safety system.

You need two facts from the lab for each controller you talk to:

| Fact | Example form | Where it comes from |
|---|---|---|
| the hub's host name or IP | `hub-host.lab` | whoever runs the hub service |
| the hub's TCP port for that controller | `5101` | the same person; one port per controller |

Keep both in a configuration file or front-panel control, not hard-coded in the VI. Do not assume which port is which controller:
ask the controller who it is (section 6).

**Your machine must be allowed in.** The hub refuses connections from addresses it has not been told about, and logs each refusal.
If your connection opens and is closed again at once, your computer's address is probably not on the hub's `allow_from` list: ask
whoever runs the hub to add it. Also check that no firewall between you and the hub blocks the port.

There is **no password and no encryption**. Anyone allowed to connect can send any command, including the ones that start
output (section 8). Treat the connection like the front panel of the machine.

---

## 2. Connect: open a TCP connection and send `@quiet` first

The hub speaks plain text over a normal TCP socket. In LabVIEW use the **TCP** palette (`TCP Open Connection`, `TCP Write`,
`TCP Read`, `TCP Close Connection`), not VISA serial.

1. `TCP Open Connection` to the hub's host and port. Use a timeout of a few seconds.
2. **Immediately send the first line `@quiet <your-program-name>` ending in CR LF.** For example `@quiet bench-gui`.
3. Then send commands, one per line, each ending in CR LF.

```
@quiet bench-gui\r\n
*IDN?\r\n
```

What `@quiet` does: a program should always use it. Without it the hub treats you as a person at a console and sends you extra
text: a greeting, a copy of every command and reply from the other connected clients, and hub notices. A quiet client receives only
what a program needs (section 4). The name you give shows in the hub's client list and in its logs, so make it recognisable (the
VI or application name, plus the machine if there are several copies). The hub makes names unique by adding a `'` if one is
already taken. The name is optional but you should always give one.

Rules for the first line:

- It must be the very first thing sent on the connection. If your first line is anything else, the hub treats it as a command and
  you are an ordinary interactive client (noisy). If that happens by mistake you can still switch with the command `!hub quiet on`;
  the hub answers `# hub: quiet on`.
- Quiet mode gives **no acknowledgement** of the `@quiet` line itself. Do not wait for a reply to it; just send your first
  command.
- Do not use the characters `@` or `!` at the start of any controller command (no controller command starts with them).

**Closing.** `TCP Close Connection` when your program ends. Closing is clean; the hub notes that you left. A command you had
queued but not yet sent to the controller is dropped when you disconnect.

---

## 3. Framing rules

| Item | Rule |
|---|---|
| Encoding | plain ASCII text |
| End of a line you send | **CR LF** (`\r\n`). A lone CR or a lone LF is also accepted as an end of line, but always send CR LF |
| End of a line you receive | CR LF. To be safe, split on LF and strip a trailing CR |
| One command per line | no `;`-separated compound commands |
| Longest command | **127 characters** (before the line end). Anything longer is silently cut by the controller and the cut text is run, so keep commands short; long data (tables, firmware) is sent in pieces by commands that exist for it |
| Blank lines | ignored, never answered, do not count them as commands |
| Case | commands are case-insensitive. Send the full long form exactly as written in the command reference |
| Numbers | plain decimal, no hex or exponent unless the command says so. The whole token must be a number or the command is refused (`ERR 12 not a number: ...`) |

LabVIEW tips:

- `TCP Read` has a *mode*. **CRLF** mode returns one line at a time (it returns when a CR LF arrives, when the *bytes to read*
  count is reached, or at the timeout). Set *bytes to read* to at least **65536**, because a single reply line can be about 40
  kilobytes (section 5.3), and keep any bytes returned together with a timeout error: a long line can arrive in pieces, so a
  timeout may hand you the first part of a line, which you must prepend to the next read. If you prefer, use *Standard* or
  *Immediate* mode and split lines yourself in a string buffer, because several lines can arrive in one read, or half a line.
- Build every command string with an end-of-line constant: **Format Into String** `%s\r\n`, or the *End of Line* constant
  set to CR LF. Do not rely on the platform default end-of-line setting.
- Never assume that one read is one reply. Keep a string buffer between reads and split it on line ends (section 5).

---

## 4. What comes back: line types

Every line the hub sends you is one line of text. Classify each line by how it **starts**:

| Starts with | What it is | Rule for your code |
|---|---|---|
| `OK` | the controller's success reply to a command you sent. May have data after it: `OK 1.250 3 abc` | match it to the oldest command still waiting for a reply |
| `ERR <code> <message>` | the controller refused a command you sent. The numeric code is stable; the message is for humans | match it to the oldest waiting command; **branch on the code, never on the text** |
| `ERR hub <text>` | **the hub**, not the controller, is telling you it could not do your command (section 7) | match it to the oldest waiting command, like any other `ERR` |
| `!` (for example `!EVT ...`, `!BOOT ...`) | an **unsolicited** line from the controller: telemetry events, or the three-line banner it prints after every reset | **never a reply to your command.** It can arrive at any moment, even between a command and its reply. Route it to an event handler or ignore it |
| `#` | the hub answering one of **your** `!hub ...` commands (section 9) | not a controller reply; never counted as the reply to a controller command |

In quiet mode nothing else is ever sent to you: no echoes of other people's traffic, no join/leave notices, no greeting.

**Ordering guarantee.** The hub sends each of your commands to the controller in the order you sent it, and gives you the replies
in that same order, one reply line per command (the exceptions are the multi-line replies in section 5.3). So a simple first-in
first-out queue of "commands waiting for a reply" is all the matching you need. You do not tag commands or replies.

---

## 5. Recommended program structure

### 5.1 Three loops, one connection

Use one connection per program per controller, owned by a single communication loop, so that two parts of your program can never
write to the socket at the same moment and mix their lines. A producer/consumer arrangement works well:

1. **Command queue** (a LabVIEW Queue of strings). Every part of the program that wants to talk to the controller enqueues a
   command, together with a way to receive its reply (a notifier, a user event, or a reply queue created for that request).
2. **Writer loop.** Dequeues a command, appends CR LF, `TCP Write`, then pushes the request onto a *waiting* FIFO. Optionally limits
   how many commands may be waiting for a reply at once (see 5.2).
3. **Reader loop.** Reads lines continuously (`TCP Read`, CRLF mode, short timeout such as 100-250 ms; a timeout with no data is
   normal, not an error). For each line, apply the table in section 4: `OK`/`ERR` lines complete the **oldest** entry of the
   waiting FIFO; `!` lines go to the event handler; `#` lines go to whoever issued the `!hub` command.

Keep the reader loop fast: do no slow work (file writes, plotting) inside it, or replies and telemetry back up.

### 5.2 Do not flood the controller

The controller handles one command at a time over a 115200-baud serial line, and the hub queues commands behind those of every
other client. A short command takes a few milliseconds to a few tens of milliseconds. A poll loop that sends `STATE?` 100 times a
second just fills the hub's queue (it holds at most a few hundred commands and then answers `ERR hub queue full`). Guidelines:

- Poll status at a modest rate (a few times per second is plenty for a display), and have **at most one request outstanding per
  poll item**. Send the next poll only after the previous one has been answered.
- Prefer the controller's own telemetry events (the `!EVT` stream, described in a later part of this guide) over fast polling.
- When many commands must be sent in a row (uploading a shot profile, saving settings) you may send them back-to-back, since order
  is preserved, but a few at a time with replies checked is easier to debug than hundreds in a burst.

### 5.3 Replies that are not one line

Almost every command gets exactly one `OK`/`ERR` line. The exceptions:

| Command | Reply shape |
|---|---|
| `SYS:EVLOG?` | `OK <n>`, then exactly *n* `!EVT` lines (the controller's event history). The `!EVT` lines are also delivered to you as events |
| `CONFig:PROFile:CATalog?` | an `OK ...` line; if a saved slot cannot be read the `OK` line ends early and an `ERR` line follows |
| `LOG:DATA?`, `PFMIN:DATA?`, `PFMIN:RAW?`, `PFM:GAPLOG?` | **one very long line** (up to about 40 kilobytes), which takes seconds to arrive at serial speed. Your line buffer must be able to hold it, and your read timeout must allow for it |

A controller restart in the middle of a multi-line reply ends it with no end marker; you will see fewer lines than promised.

---

## 6. Identify the controller you reached

Never trust the port number to mean a particular controller; the lab's wiring and the hub's configuration can change. After you
connect, ask:

```
*IDN?
```
Reply: `OK <product> <hardware-revision> <firmware-version> <build>`, and on firmware with device identity, the serial number
and chip ID appended: `... SN=<serial> UID=<chip id>`. The first word after `OK` names the product (the two products have
different command sets), so **choose the command set from this reply**, not from the port.

```
SYS:ID?
```
Reply: `OK serial=<serial> uid=<chip id> flashuid=<id> product=<name> hw=<rev> fw=<version> commit=<hash> label=<text> verified=<0|1> identity=<ok|unset|...>`.
The `serial` is the controller's permanent label (it reads `UNSET` on a board that has not been given one). `verified=1` means
the stored serial matches this physical chip. Older firmware answers `ERR 1 Unknown command` to `SYS:ID?`; that is fine, use
`*IDN?`.

A program that must only ever talk to a specific unit should read the serial from `SYS:ID?` after every connect **and after every
`!BOOT` line**, and refuse to proceed if it is not the expected one. (The hub can also be configured to refuse to forward commands
to the wrong board, in which case you get `ERR hub identity mismatch`.)

---

## 7. When things go wrong: `ERR hub ...`

An `ERR hub` line is the hub speaking. Each one answers exactly one command of yours.

| Reply | Meaning | What to do |
|---|---|---|
| `ERR hub timeout` | the controller gave no reply within the hub's limit (5 s for ordinary commands, 30 s for flash erases, verifies and the long data dumps) | the command **may or may not have run**. Re-read the state with a query before deciding. **Never blindly resend a dangerous command** (section 8) |
| `ERR hub the controller restarted before it replied` / `... before it was sent` | the controller reset (a `!BOOT` banner appeared) | the command was lost or abandoned. Re-identify (section 6), re-read the state, then decide |
| `ERR hub the connection to the controller was lost` | the hub's link to the bridge dropped (cable, power, bridge) | the hub keeps retrying by itself. Poll `!hub status` until `connected=1`, then re-identify and re-read state |
| `ERR hub the controller is not connected` | you sent a command while the hub has no link to the controller | the command was not sent. Wait and retry once connected |
| `ERR hub identity mismatch: ...` | the hub is set to expect a particular board, and a different one answered | stop; tell the operator. Do not work around it |
| `ERR hub queue full` | too many commands are waiting | you are sending too fast (section 5.2). Slow down |
| `ERR hub DANGEROUS [<category>] not sent yet: type !hub confirm within <n> s ...` | the hub is set to hold dangerous commands for confirmation (section 8); your command was **not** sent | see section 8 |

Your own read timeout must be **longer than the hub's**, so that you see the hub's `ERR hub timeout` rather than giving up first.
Use about **10 seconds** for ordinary commands (the hub's 5 s, plus time spent waiting behind other people's commands), and
**40 seconds or more** for the long-reply commands in section 5.3 and for firmware-update commands.

A reply that arrives after its command already timed out is **never delivered** to a quiet client. The hub holds its queue until it
can tell that the late line has gone by, so your replies cannot slip out of step by one.

**If the hub itself goes away.** A restart of the hub service or a network break closes your TCP connection. `TCP Read` returns a
connection-closed error (or reads zero bytes with an error). Treat it like any other failure: close the reference, wait a couple
of seconds, reconnect, send `@quiet <name>` again as the first line, re-identify (section 6), and re-read the state. Retry with a
back-off, forever, with a visible "link down" indication; do not crash the application. Do not assume anything about the
controller's state across a hub or link outage: it kept running by itself, so read it back. Also drop everything in your waiting
FIFO when the connection closes (those commands will never get a reply on a new connection).

Quiet clients are **not told** that the controller link dropped; they learn it from `ERR hub` replies or from `!hub status`
(section 9). A program that only listens for events should poll `!hub status` every few seconds, and watch `rx_age` (seconds since
the controller last said anything).

---

## 8. Several people at once, and dangerous commands

The hub allows several clients on one controller on purpose, so that an operator display, a test script and a developer can all be
connected. There is currently **no lock or lease**: whoever sends a command, it is run. Agree among the people involved who is in
charge during a shot. In-person coordination is the safety guard.

What the hub does to make that visible:

- Interactive (non-quiet) clients see who joins and leaves and every command the others send.
- Commands on the hub's **dangerous-command list** are announced to every other connected client and written to the log with a
  category. The list covers anything that starts or enables output (`FIRE`, `SOURce:RUN`, `SHOT:STARt`, enabling a channel or phase,
  and so on), arms the controller (`ARM`), weakens a protection (switching an interlock off, `FAULT:CLEar`, changing fault
  polarity or limits), forces a fault, reflashes or resets the controller (`BOOT`, `FWUPdate:*`), or changes the board's
  identity. Queries (anything ending in `?`) are never dangerous, and turning an output **off** is never listed.
- The hub can be configured to **hold** a dangerous command until its sender types `!hub confirm` (and cancel it with `!hub cancel`).
  Whether that is on is shown by `confirm=1` in `!hub status`. In that mode a dangerous command is **not sent**: the hub answers it
  at once with `ERR hub DANGEROUS [<category>] not sent yet: type !hub confirm within <n> s ...` (15 s by default) and tells the
  other clients. Only if the same client then sends `!hub confirm` within the time limit is the command sent, and its real
  `OK`/`ERR` reply follows (a `!hub confirm` after the limit is answered `# hub: too late, not sent; send the command again`, and
  `!hub cancel` discards it). Scripted sequences normally run with the hold off. A program that may meet either setting should
  treat that `ERR hub DANGEROUS` reply as "waiting for a human", and send `!hub confirm` only after its own operator-confirmation
  step has been satisfied, never automatically.

Your program must therefore:

- never send a dangerous command because a timeout looked like a failure;
- show other people's activity if your application is an operator display (use `!hub clients`, section 9);
- expect that **another client may change the controller's state** under you (arm it, disarm it, change a setting). Do not cache
  state for long; re-read before acting, and treat unexpected state as information, not as an error in your program.

---

## 9. Talking to the hub itself: `!hub` commands

A line beginning `!hub` is answered by the hub, not sent to the controller. The answer is one or more lines starting with `#`.

| Command | Answer |
|---|---|
| `!hub status` | one `# hub ...` line of `key=value` fields: `name`, `connected` (1 = the hub has a live link to the controller), `serial`, `identity`, `verified`, `uid`, `product`, `fw`, `epoch` (counts controller restarts seen), `session` (counts hub-to-controller connections), `clients` (how many are connected), `who` (their names and addresses), `confirm` (1 = dangerous commands are held), `rx_age` (seconds since the controller last sent anything), `inflight` (the command now waiting for a reply), `queued` (commands waiting behind it), `log` (where the log file is) |
| `!hub clients` | `# hub clients: <n>` followed by one `#` line per client: name, address, how long connected, how long idle, how many commands and dangerous commands sent, and whether quiet |
| `!hub identify` | makes the hub ask the controller who it is again, and refreshes the fields in `!hub status` |
| `!hub quiet on` / `off` | switches quiet mode (`# hub: quiet on`) |
| `!hub timestamps on` / `off` | makes **every** line the hub sends you start with `[HH:MM:SS.mmm] ` (the hub's local time). Meant for a person at a terminal; **do not turn it on in a program**, unless you strip the prefix before classifying lines (section 4) |
| `!hub confirm` / `!hub cancel` | releases or discards **your own** held dangerous command (section 8) |

Parse `!hub status` by splitting on spaces and then at the first `=`; do not rely on field order, because fields can be added. Fields
may be missing or `?` when the hub has not yet identified the controller.

`#` lines are never replies to controller commands. Do not put them in the same FIFO as `OK`/`ERR`.

---

## 10. Your first session, step by step (test this before writing any VI)

Before wiring this into LabVIEW, prove the path with a plain text client. From a command prompt on the machine that will run the
VI, with `<host>` and `<port>` from the lab:

- Windows: `telnet <host> <port>` (enable the Telnet client first), or PuTTY in *Raw* mode
- Linux or macOS: `nc <host> <port>`

Type (each followed by Enter), watching what comes back:

```
@quiet manual-test
*IDN?
STATE?
!hub status
!hub clients
```

You should see an `OK ...` line for `*IDN?`, an `OK ...` line for `STATE?`, one long `# hub name=...` line, and a `# hub clients`
block that lists you as `manual-test ... quiet`. If the connection closes immediately, your address is not allowed (section 1);
if it never opens, the port is wrong or blocked.

Then reproduce the same in LabVIEW:

1. `TCP Open Connection` (host, port, timeout 5000 ms).
2. `TCP Write` `@quiet myvi\r\n`.
3. `TCP Write` `*IDN?\r\n`, then `TCP Read` in CRLF mode with a 10 s timeout. Expect a line starting `OK`.
4. Loop reading lines; print them, noticing that any line beginning with `!` can appear at any time.
5. `TCP Write` `!hub status\r\n` and confirm the `# hub` line shows `connected=1`.
6. `TCP Close Connection`.

Only after that, add the queues and loops from section 5. Use only **queries** (`...?`) while you are still testing the
connection logic; do not send commands that start output until your reply matching has been proven correct, and only with the
controller prepared for it by the people responsible.

---

## 11. Where the command lists are

This guide is part 1 of three. The other two are in this folder:

| Part | File | Contents |
|---|---|---|
| 2 | `docs/labview_state_and_faults.md` | the operating states (`IDLE`/`ARMED`/`FIRING`/`FAULT`), what makes each command refuse, resets, every fault type and how to recover, and a checklist for a front-end |
| 3 | `docs/labview_commands.md` | the command sets of both products with units and ranges, worked shot sequences, the error codes, and what a front-end should leave alone |

The two controller products have different command sets (the same words can mean different units), so use the one that matches the product
the controller named in `*IDN?`. The full per-product references are:

| Document (in this repository) | Contents |
|---|---|
| `firmware/xrex/docs/labview_interface.md` | the XREX (Transrex magnet supply) command contract for a LabVIEW front-end: shot configuration, status polling, error codes, a worked shot, the `!EVT` telemetry stream. Its section 1 describes the serial link directly; **ignore the baud rate, parity and VISA parts**, because the hub takes care of the serial side. Everything else (commands, replies, error codes) applies unchanged through the hub |
| `firmware/xrex/docs/command_reference.md` | every XREX command |
| `firmware/hbridge/docs/command_reference.md` | every HBRIDGE command |
| `firmware/xrex/docs/scpi_conventions.md` | the reply grammar, the numeric error codes, mnemonic matching and argument rules shared by both products |
| `docs/device_identity_and_host_service.md` | why the hub exists and its full design (section 5.1 is the detailed behaviour) |
| `host/README.md` | running and configuring the hub itself |

---

## 12. Quick reference

- Connect to the hub (not the bridge) over TCP, host and port from the lab.
- First line: `@quiet <name>` + CR LF. No acknowledgement.
- One command per line, CR LF, at most 127 characters.
- Read whole lines. Classify by first character: `OK`/`ERR` = reply (oldest waiting command), `ERR hub` = the hub's own failure,
  `!` = unsolicited, `#` = answer to a `!hub` command.
- One reply per command, in order; a FIFO of waiting commands is all you need. Exceptions: `SYS:EVLOG?`, the profile catalog, and
  the long data lines.
- Timeouts: yours longer than the hub's (about 10 s ordinary, 40 s or more for long commands).
- After a timeout, a restart or a lost connection: re-identify and re-read state; never blindly resend a dangerous command.
- Identify the controller with `*IDN?` / `SYS:ID?`, never by port number. Choose the command set from the product name.
- No authentication, no write lock: coordinate with the other people connected. `!hub clients` shows who they are.

*Still to come: operator procedures (pre-shot checklist, the dangerous actions, response to a fault, logs), a decoding guide for the XREX
event stream with LabVIEW examples, and the access policy for the hub.*
