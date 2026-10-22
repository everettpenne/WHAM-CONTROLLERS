# Connecting to a controller through the hub: guide for operators

For a person at a computer who needs to talk to a WHAM controller (the XREX magnet-supply controller or the HBRIDGE Limiter/HVPS
controller) by typing commands. It covers getting connected, checking you reached the right controller, working alongside other
people, switching between controllers, and leaving. It does **not** say what to type once connected: for that, read the operators'
guide for your product (`firmware/xrex/docs/operators_guide.md`, `firmware/hbridge/docs/operators_guide.md`). Programs (LabVIEW and
others) connect differently: see `docs/labview_hub_guide.md`.

## 1. How the connection works

Each controller is reached through a **hub**, a small service running on a server in the lab. The hub is the only thing that
talks to the controller's serial link; you and everyone else talk to the hub over the network. One hub serves one controller, on its own **port number**.

```
your computer  --(ssh tunnel, if remote)-->  hub server:  port A  -> hub -> controller 1
                                                          port B  -> hub -> controller 2
```

Consequences you should know:

- **The port number, not anything you type, selects the controller.** To work with another controller you connect to another port.
- Several people can be connected to the same controller at once. **There is no lock**: anyone connected can send any command.
- The hub writes a log of everything sent and received. It announces dangerous commands (anything that arms or starts output, or weakens a protection) to everyone connected.
- Closing your connection does **not** stop a shot or change what the controller is doing.

## 2. What you need

1. **The hub server's name and the port for each controller.** Ask the hub administrator; write them in section 9 of this guide so you have them next time. Ports are assigned per controller and may change; never rely on memory.
2. **This repository on your computer**, and Python 3. The client is `firmware/hbridge/python/net_client.py` (an identical copy is in `firmware/xrex/python/`); it needs nothing installed.
3. **If you are not on the lab network: ssh access** to the hub server.

## 3. Step 1 (remote computers only): open the tunnel

The tunnel makes the hub's port appear on your own computer. Use a terminal window that you leave open:

```bash
ssh -N -o ServerAliveInterval=30 -L <local-port>:127.0.0.1:<hub-port> <server>
```

- `<hub-port>` is the controller's port on the server. `<local-port>` is the port you will use on your own computer; using the **same number** keeps it simple.
- Several controllers in one tunnel: repeat the `-L` option, once per controller, for example `-L 5101:127.0.0.1:5101 -L 5102:127.0.0.1:5102`. **Check each pair: the number after the second colon is what selects the controller.**
- It asks for your password (or key phrase) and then **prints nothing**. That is normal; it is working. Leave the window open.
- To close the tunnel, press Ctrl-C in its window.
- If it prints `Operation timed out` or `Connection closed`, your network dropped: run it again. A tunnel that was open when your computer slept or changed network is dead even if its window is still open: close it and open a new one.

On the lab network, skip this step and use the hub's address directly (your administrator will say whether that is permitted from your computer).

## 4. Step 2: connect

In a second terminal window, from the repository folder:

```bash
python3 firmware/hbridge/python/net_client.py --port <local-port> --name <your-name>
```

`<your-name>` is how you appear to others (`!hub clients` shows it): use your own name. The client connects to the port on **your** computer, which is the tunnel.

On success it prints nothing at first and waits for you (the people already connected see `--- <your-name> (<address>) joined ---`). A reply to `!hub status` is the quickest proof the connection works. **Every line it prints starts with the time**, `[HH:MM:SS.mmm]`, in the hub's local time: useful if you need to tell someone when something happened. (`--no-timestamps` turns that off.)

Type a command and press Enter. Each command you send gets one reply line, in order:

- `OK ...`: it worked (`OK` alone, or `OK` followed by a value).
- `ERR <number> <message>`: the controller refused it. Read the message.
- `ERR hub ...`: the **hub**, not the controller, is telling you something (section 8).
- Lines beginning `#` answer a `!hub` command. Lines beginning `!` are unsolicited news (a controller reset, another person's activity).
- Lines starting `[name] > ...` are **another person's** commands, shown so you can see what they are doing.

## 5. Step 3: make sure you reached the controller you meant to

Do this every time you connect, before anything else:

```
*IDN?
STATE?
!hub status
!hub clients
```

| Check | What it should show | If not |
|---|---|---|
| `*IDN?` | begins `OK WHAM-XREX-PFMG474` or `OK WHAM-HBRIDGE-PFMG474`, and it is the product you intended | **Wrong controller: disconnect** (Ctrl-D) and check the port. The two use the same hardware but different commands; a command that is harmless on one can start output on the other. |
| `STATE?` | `OK IDLE` before you begin | Read the state's meaning in your operators' guide. `ARMED` or `FIRING` that you did not cause means someone else did: find out who before you touch anything. |
| `!hub status` | `connected=1` | `connected=0` means the hub has lost the controller (power, cable): nothing you type will reach it. |
| `!hub clients` | the list of everyone connected | If anyone else is listed, **agree with them who is operating** (section 6). |

`*IDN?` also reports the board's serial number and firmware version, which is worth noting in your log. `SN=UNSET` means the board has not had a serial assigned yet.

## 6. Working with other people on the same controller

The hub lets several people and programs connect on purpose (for example you at a terminal while a LabVIEW front-end is also attached). It does not arbitrate between you.

- **Look at `!hub clients` before you arm or fire.** A program counts as someone.
- **Say out loud (or in your lab channel) who is in charge** of the controller before anything that starts output. In-person coordination is the safety mechanism.
- You see other people's commands as `[name] > command`. If you see one you did not expect, stop and ask.
- Dangerous commands are announced to everyone with a category. Depending on how the hub is set up, you may be held: the hub answers `ERR hub DANGEROUS [<category>] not sent yet: type !hub confirm within <n> s ...`. That is **a prompt to you**: the command was not sent. If you meant it, type `!hub confirm` within the time shown; if not, type `!hub cancel` or do nothing. Do not confirm a command you did not mean to send, and never confirm because someone told you to from another window.
- The state can change under you: someone else can `ARM` or `DISARM`. Re-read `STATE?` before acting.

## 7. Switching between controllers, and leaving

**Each connection is one controller.** To move to another controller you do not type anything: end this connection and open another on the other port.

1. Press **Ctrl-D** in the client window. (Ctrl-C also ends the client, but Ctrl-D is the clean way: it tells the hub you are leaving.) Closing it does not stop a shot or change the controller's state.
2. Run the client again with the other controller's port:

```bash
python3 firmware/hbridge/python/net_client.py --port <other-port> --name <your-name>
```

3. Run the checks in section 5 again: **never assume which controller a port reaches.**

You can have two client windows open at once, one per controller. Label them (the terminal's title) so you cannot mistake which is which, and run section 5's `*IDN?` in each. When you are finished with the controllers, close the client windows, then close the tunnel with Ctrl-C.

## 8. If something goes wrong

| What you see | Likely cause | What to do |
|---|---|---|
| Tunnel window: `Operation timed out` / `Connection refused` | the network, or the server name is wrong | check you are on the network or VPN, check the server name, run the tunnel again |
| Tunnel connects but the client says `Connection refused` | the tunnel is not running, you used the wrong local port, or the hub is down | check the tunnel window is open and the `-L` numbers match what you give `--port`; if both are right, tell the hub administrator |
| The client connects and closes at once | your address is not on the hub's allowed list | ask the hub administrator to add it |
| Nothing prints after the greeting when you type | the hub, not the controller, is slow or the tunnel has died | type `!hub status`: if there is no answer, the tunnel is dead; close and reopen it |
| `ERR hub timeout` | the controller did not answer in time; **the command may or may not have run** | read back with a query (`STATE?`) before doing anything else. **Never resend a command that starts output** because of a timeout |
| `ERR hub the controller is not connected` | the hub lost the controller | check the controller's power and network cable; `!hub status` shows `connected=0` until it returns |
| `ERR hub ... restarted ...` or a `!BOOT` line | the controller reset (power dip, update, watchdog) | treat as a reset: read `STATE?`, `FAULT?`, and re-check your settings; nothing is armed |
| `*IDN?` shows the wrong product | wrong port | disconnect, and see section 7 |
| `!hub status` shows `connected=0` | the hub cannot reach the controller | power, network cable, serial bridge; ask the maintainer |

A shot already running keeps running to its end on its own if you lose your connection or the tunnel drops.

## 9. Fill in for your installation

| Item | Value |
|---|---|
| Hub server (ssh name) | |
| Controller 1: name / product / local port | |
| Controller 2: name / product / local port | |
| Hub administrator (who to ask) | |
| Your name to use in `--name` | |

## 10. Quick reference

| I want to | Do this |
|---|---|
| open the tunnel | `ssh -N -o ServerAliveInterval=30 -L <p>:127.0.0.1:<p> <server>` |
| connect | `python3 firmware/hbridge/python/net_client.py --port <p> --name <me>` |
| check where I am | `*IDN?`, `STATE?`, `!hub status`, `!hub clients` |
| see who else is connected | `!hub clients` |
| switch controllers | Ctrl-D, then connect to the other port |
| leave | Ctrl-D (the controller keeps doing whatever it was doing) |
| close the tunnel | Ctrl-C in the tunnel window |
| release a held dangerous command | `!hub confirm` (only if you meant it) / `!hub cancel` |
| turn the timestamps off | start the client with `--no-timestamps` |
