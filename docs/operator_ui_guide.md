# The operator console (no terminal needed): guide for operators

A web page, running on your own computer, for working with any WHAM controller (XREX magnet-supply, HBRIDGE Limiter/HVPS, ECH gyrotron
controller). You click instead of typing commands. Everything you can do on the page you can also do by typing, in the **Console** tab; the page
adds the screens, the checks and the confirmation questions.

## 1. Starting it

Install **Python 3** once (https://www.python.org/downloads/; on macOS it is often already there). Then double-click the launcher in `host/operator_ui/`:

| Computer | Double-click |
|---|---|
| macOS | `start_wham_ui.command` (the first time: right-click, Open) |
| Windows | `start_wham_ui.bat` |
| Linux | `start_wham_ui.sh` |

A black window opens (leave it open while you work) and your browser shows the page. Closing that window closes the page and disconnects everything;
**anything a controller is doing keeps going**. If the page does not open by itself, open the address the black window prints (`http://127.0.0.1:8765/`).

To practise without hardware, start it with `--demo` (in a terminal: `python3 host/operator_ui/wham_ui.py --demo`): the start screen then offers simulated controllers.

## 2. Connecting

The start screen lists your saved controllers. Click **Connect**; or **Connect to a controller** (top right) for one that is not listed. Three ways, in order of preference:

1. **Through the host (recommended).** The lab's host service shares the controller with other people, keeps a log and shows who is connected. You need the host's address and the
   controller's port (see `docs/operator_hub_connection.md`; from outside the lab that guide's ssh tunnel comes first, then the address is `127.0.0.1` and the port is the tunnel's).
2. **Directly over the network**, when the host is not available. The controller's network bridge accepts **one** connection: nobody else, and not the host, can be connected to it at the same time. Nothing is logged except what you save from the Console.
3. **USB-serial cable** from your computer to the controller. Needs `pyserial` (`python3 -m pip install pyserial`).

Tick **Add it to the saved list** to keep it on the start screen. The list is stored in `host/operator_ui/controllers.json` on your computer.

After connecting, the page asks the controller who it is (`*IDN?`, `SYS:ID?`) and shows its **label, board serial, firmware and chip ID**. **Check that this is the controller you meant** before doing anything:
the tab name and the identity card are the proof, not the name you gave it. If the device does not identify itself as a WHAM controller the page offers only the Console and says so.
You can have several controllers open at once, one tab each; the coloured dot on a tab is the link (green: answering, amber: waiting, red: closed) and the word next to it is the controller's state.

## 3. The screen

- **Left: identity and state.** The big badge is the controller's state: **IDLE** (nothing is output), **ARMED** (ready: FIRE, or a trigger edge, starts the shot), **FIRING** (a shot is running),
  **FAULT** (output stopped; the reason is shown). The buttons are enabled only when they make sense: ARM in IDLE; DISARM and FIRE (START SHOT on XREX) in ARMED; Clear fault in FAULT.
  Through the host, a box lists **who else is connected**: agree with them before you arm or fire.
- **Right, Operate tab:** the controls for this product (below). **Console tab:** the raw conversation (blue: what you sent, green: replies, red: errors, purple: what other people sent). Type any command;
  Up/Down recall earlier ones; the chips fill in common queries; **Save log** writes the whole conversation to a text file.

### HBRIDGE and ECH (H-bridge supply)
1. **Choose the supply**: Limiter or HVPS (only while IDLE). The limits (frequency and duty) are shown and used to check your numbers.
2. **Which phases drive**: U, V, W. Changing it asks for confirmation.
3. **Build and send the shot**: a table of setpoints (time in ms, frequency in kHz, duty U/V/W in %). The controller moves frequency and duty in straight lines between setpoints. Values outside the limits are marked and cannot be sent. **Send shot** uploads
   it and then **reads it back from the controller and compares**; a green message appears only when what the controller holds is what you built.
4. **Run**: ARM, then FIRE, in the State box. The **external trigger** switch shows whether a rising edge on the trigger input will start the shot while ARMED; the page warns when it is ARMED with the trigger on.
- **Capacitor charger (CHARGE)**: a 50 % square wave (1 Hz to 100 kHz) for the capacitor-bank charger, independent of the state. Start/change it, or press **OFF**. It stays on through a fault until you turn it off. (Needs firmware v0.11 or later; older firmware says so.)

### XREX (magnet supplies)
1. **Shot timing**: ramp up, flat top, ramp down (seconds). 2. **Channels** (four cards): whether the channel is in the shot, its enable line and contactor, its peak current, the raw levels of its fault inputs, and its live setpoint/measured/output frequency.
3. **Run**: ARM, then START SHOT. ARM is refused unless every channel in the shot has its enable line and contactor on.

The readings refresh every few seconds (choose Off, 2, 5, 10 or 30 s at the top of the Operate tab, or **Refresh now**).

## 4. Recording a shot (data, plots, report)

Both products can log what the control loop did during a shot, and the page will collect, save, plot and summarise it for you.

1. In the State box on the left, tick **Record data for the next shot**. (Check the **Shot data** tab once: the channels or phases to record and the **expected shot length** are filled in from the shot you set up; change them if needed. The controller's log holds about 1000 samples, so a longer shot is recorded at a lower sample rate. Give the length a little long rather than short.)
2. Press **ARM** and confirm. The line under the checkbox changes to "Recording armed". Then start the shot as usual (FIRE, START SHOT, or the external trigger).
3. When the shot ends, the page reads the log back from the controller (a progress figure shows), saves it, and opens the **Shot data** tab on the new recording.

What you get for each channel (XREX) or phase (HBRIDGE): a plot of **setpoint, measured feedback and output** against time, and the figures an operator reads off a shot: peak setpoint and measured peak, rms and largest error, overshoot, the flat-top mean and ripple, and the output range. XREX plots can be shown in **amps or hertz** (amps are estimated from the controller's linear frequency-to-current calibration, which is a placeholder until it is measured on a real Transrex: the hertz values are exact). HBRIDGE shows target and measured in volts and the output (kHz for HVPS, % duty for Limiter) in a second panel.

Every recording is also **saved automatically** on your computer, in `host/operator_ui/shots/<board serial>/<date-time>/` (or the folder given with `--shots-dir`): `shot.csv` (the data, for Excel or Python), `report.html` (a self-contained report with the plots: open it in any browser or attach it to an e-mail), `report.md`, and `shot.json` (everything, including what was commanded). The same four files can be downloaded from the page. Recordings of a controller stay listed on its Shot data tab.

Good to know:
- **HBRIDGE records only closed-loop runs.** A shot played open-loop (the table of setpoints you build in step 3 of the Operate tab) is not logged by the controller; the page warns when every selected phase is open-loop and says "nothing recorded" afterwards. Closing a loop needs a target (`PID:LOOPMODE <phase> 1` and `TARGet:*` in the Console): there is no screen for it yet.
- A shot that ends in a **fault** is still recorded as far as the controller's loop ran (the fault ramp-down is included) and is marked "fault".
- If you disarm before the shot, the page reports "nothing recorded". If the page was closed or the link dropped during a shot, **Read the controller's log now** (Shot data tab) fetches whatever the controller still holds: it keeps only the latest log, and a new ARM-with-recording replaces it.
- Only one recording can wait for a shot at a time; **Cancel recording** gives it up.
- Reading the data takes a few seconds on XREX (one long reply per channel) and a little longer on HBRIDGE (40 samples per reply, each checked with a check sum and asked again when a character was lost).

## 5. Confirmation questions

Commands that **arm, start output, change which phases drive, start the charger, turn on the external trigger, clear a fault** (the project's dangerous-command list) always ask *"Are you sure?"* in plain words, show the exact command,
and say how many other people are connected. **Cancel is the default.** This check is made by the page's server, not only on the screen: a command on that list is refused unless you confirmed it.
If the host is set to hold dangerous commands, the page answers the host's question itself after you confirmed on the page.

## 6. When something is wrong

| You see | Meaning and what to do |
|---|---|
| "Could not connect ... nobody is listening there" | Wrong address or port, the ssh tunnel is not running, or the host service is down. Check `docs/operator_hub_connection.md`. |
| "...the bridge accepts one client" / connection closes at once (direct) | Someone else (or the host) is connected to that bridge. Use the host instead, or ask them to disconnect. |
| Amber "controller is not answering" | The link is up but the controller does not reply: power, cable, or it is busy. Readings may be old. The page reconnects by itself every few seconds. |
| Red "host cannot reach the controller" | The host is fine, the controller is not reachable from it (power or network cable). |
| Controls missing, only Console | The device did not identify itself as a WHAM controller. Do not send commands unless you know what it is. |
| A red message with `ERR n` | The controller refused the command; the message says why in plain words. `ERR 13` is usually "wrong state" (for example ARM when not IDLE). The Console shows the exact reply. |
| Send shot: "Read-back does not match" | Do not arm. Send it again; if it repeats, tell whoever maintains the firmware. |

## 7. Limits of this page

It does not lock out other people (the host has no lock either), has no passwords, and does not update firmware (use `python/fw_update.py` in the firmware project). It listens on **your computer only**; do not start it with `--listen` on a shared network, because anyone who can
reach it could command the controllers you have open. Developers: `host/operator_ui/wham_ui.py` (server), `recorder.py` (shot recording), `shot_data.py` (units, statistics, plots, files), `static/` (page), `wham_sim.py` (the simulated controller, which also produces synthetic shot logs), tests in `host/tests/test_operator_ui.py` and `test_shot_data.py`.
