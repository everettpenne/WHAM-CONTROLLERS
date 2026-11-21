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

**To stop it:** the **⋮** menu (top right), **Quit the console**. That disconnects from every controller, closes the tunnel the console opened, and stops the program; the page then says so and
the tab can be closed. Closing the black window does the same.

### On another computer

The console runs on any computer that has Python 3 (3.9 or later; checked on 3.9 and 3.11) and an ssh tunnel to the lab host. It needs nothing installed with `pip` (only the USB-cable connection needs `pyserial`).

1. Copy the repository's **`host` folder** to that computer (or clone the repository). The console uses nothing outside that folder.
2. Start it with the launcher for that computer (table above), or `python3 host/operator_ui/wham_ui.py`.
3. **The tunnel.** If a tunnel to the host is already open on that computer (you ran `ssh -N -L 5101:127.0.0.1:5101 -L 5102:127.0.0.1:5102 -L 5103:127.0.0.1:5103 andrew` in a terminal, or `wham_connect.py` did),
   the page finds it by its ports and uses it: nothing else to do. Otherwise press **Open tunnel**; that needs ssh on the computer and a key that logs in to the host without a password.
   If the host has another ssh name there, start the console with `--tunnel-host user@address` (or set `WHAM_TUNNEL_HOST`).
4. The three saved controllers expect the tunnel on local ports 5101 (XREX), 5102 (Limiter) and 5103 (ECH). If your tunnel forwards other local ports, press **Edit** on a controller's card and change its port.

What stays on each computer: its own saved list (`controllers.json`) and its own recorded shots (`shots/`). Nothing is shared between computers except what the host logs.

## 2. Connecting

The start screen lists your saved controllers; a dot on each card says whether its address answers (for the controllers behind the tunnel: whether the tunnel's port is open). Click **Connect**; or **+ Connect** (top right) for one that is not listed. **Edit** changes a saved controller's name, address or note; **Remove** takes it off the list. Three ways, in order of preference:

1. **Through the host (recommended).** The lab's host service shares the controller with other people, keeps a log and shows who is connected. You need the host's address and the
   controller's port (see `docs/operator_hub_connection.md`; from outside the lab that guide's ssh tunnel comes first, then the address is `127.0.0.1` and the port is the tunnel's).
2. **Directly over the network**, when the host is not available. The controller's network bridge accepts **one** connection: nobody else, and not the host, can be connected to it at the same time. Nothing is logged except what you save from the Console.
3. **USB-serial cable** from your computer to the controller. Needs `pyserial` (`python3 -m pip install pyserial`).

Tick **Add it to the saved list** to keep it on the start screen. The list is stored in `host/operator_ui/controllers.json` on your computer.

After connecting, the page asks the controller who it is (`*IDN?`, `SYS:ID?`) and shows its **label, board serial, firmware and chip ID**. **Check that this is the controller you meant** before doing anything:
the tab name and the identity card are the proof, not the name you gave it. If the device does not identify itself as a WHAM controller the page offers only the Console and says so.
You can have several controllers open at once, one tab each; the coloured dot on a tab is the link (green: answering, amber: waiting, red: closed) and the word next to it is the controller's state.
The browser tab's title shows the state and name of the controller you are looking at.

**Disconnecting.** Three places do it: the **×** on a controller's tab, the **Disconnect** button on its identity card, and **Disconnect** / **Disconnect all (n)** at the top right, which closes every
connection at once. Disconnecting closes this page's connection only: **what the controller is doing keeps going**, and the page says so (and warns when a controller is ARMED or FIRING, or a recording is waiting).

## 3. The screen

- **The tunnel** (always visible at the top right, and as a card on the start screen). From outside the lab the saved controllers are reached through an ssh tunnel to the host (`andrew`). The pill at the top
  shows whether it is open, on every screen; **Open tunnel** starts it (ssh to the host, one forwarded port per saved "through the host" controller: 5101-5103 by default) and **Close tunnel** ends it.
  The card on the start screen shows each port (green when it answers) and ssh's own message if it could not log in (it needs a key: `docs/operator_hub_connection.md`).
  - **Close tunnel disconnects first.** Controllers connected through the tunnel are listed in the question and disconnected before the tunnel closes.
  - **A tunnel you opened yourself** (ssh in a terminal, `wham_connect.py`) is recognised by its ports and used as it is. **Close tunnel** can end that one too: the page names the ssh process, asks, and
    stops it. That ends the tunnel for every program on the computer that uses it. A port held by a program that is **not** ssh is named and left alone.
  - **Partly open**: some of the ports answer (a tunnel for one controller only). The controllers on those ports can be used; to get all of them, close it and open it again from the page.
  - The tunnel the page opened is a child of the console: quitting the console closes it. Another host: `--tunnel-host <ssh name or user@address>` (or `WHAM_TUNNEL_HOST`).
- **The ⋮ menu** (top right): the **command flowcharts** for both products (the order of commands for a shot and the way out of a fault; they open in a new tab), *Using this console on another computer*, and **Quit the console**.

- **Left: identity and state.** The big badge is the controller's state: **IDLE** (nothing is output), **ARMED** (ready: FIRE, or a trigger edge, starts the shot), **FIRING** (a shot is running),
  **FAULT** (output stopped; the reason is shown, and on HBRIDGE which source latched it and which inputs are in fault now, with a link to the fault inputs). The buttons are enabled only when they make sense: ARM in IDLE; DISARM and FIRE (START SHOT on XREX) in ARMED; Clear fault in FAULT.
  Through the host, a box lists **who else is connected**: agree with them before you arm or fire.
- **Right, Operate tab:** the controls for this product (below). **Console tab:** the raw conversation (blue: what you sent, green: replies, red: errors, purple: what other people sent). Type any command;
  Up/Down recall earlier ones; the chips fill in common queries; **Save log** writes the whole conversation to a text file.
- **Right, Host log tab** (only when connected *through the host*): what the host service recorded for this controller, from **everyone** using it (LabVIEW, other people, scripts), not only this page: commands,
  replies, the controller's events, restarts and the host's own notes (who joined or left, "the controller is not connected", refused commands). Use it to judge recent activity or to find out what happened
  while you were away. A row of counters at the top answers "was anything wrong?" at a glance (ERR replies, restarts, connection problems). Choose how far back (15 minutes to everything the host keeps), which
  kinds of rows, who sent them, or type text to find (not case-sensitive); **problems only** keeps just restarts, ERR replies, refused commands and connection trouble; **Load earlier rows** goes further back;
  **Save as text** writes what you see to a file. The host's own *IDN? heartbeats and handshake queries are hidden unless you tick *include the host's automatic queries*. It refreshes every 5 s while the tab is open.
  A host that predates this feature answers "too old to show its log": update `wham_hub` on the host (`!hub log` is the command behind this tab and works from any hub client too).

The Operate tab is a column of **sections that fold**. A folded section still shows its one-line summary (the supply and its limits, the phases, how many setpoints the table and the controller hold, inputs in fault, slots used),
and the page remembers which ones you left open.

### HBRIDGE and ECH (H-bridge supply)
1. **Choose the supply**: Limiter or HVPS (only while IDLE). The limits (frequency and duty) are shown and used to check your numbers.
2. **Which phases drive**: U, V, W. Click the phases, then **Apply**: that asks for confirmation.
3. **Build and send the shot**: a table of setpoints (time in ms, frequency in kHz, duty in %; tick *a different duty for each phase* for separate U, V, W columns). The controller moves frequency and duty in straight lines between setpoints.
   - **What the controller already holds** is stated under the table ("The controller holds a shot of 54 setpoints, 15 ms"). **Show it in the table** reads it (one query per setpoint, so the page does it only
     when you ask), and a green tick appears once the table and the controller are known to hold the same shot. The tick goes away as soon as either changes.
   - **Load file…** reads a profile instead of typing it: the `_iterN.csv` written by `analysis/compensation/ech_compensate.py`, the file written by `analysis/compensation/limiter_compensate.py` (its *commanded* duty is loaded),
     any CSV with a time, a frequency and a duty column, or a text file of `SHOT:STEP` lines. **Paste…** takes the same from the clipboard (for example the commands in a logbook entry). The page says what it loaded
     ("Loaded, not sent yet: …, iteration 3, corrected from shot …, for a bank charge of 520 V") and nothing is sent until you press Send. *Typing a profile by hand is where shots go wrong; load the file.*
   - **Off** (the tick box on a row) makes an **output-off setpoint**: the output is off from that time until the next setpoint, and the setpoint before it holds its value up to the gap. This is how multi-pulse shots are
     built. The first setpoint cannot be off and the last two must be on.
   - Values outside the supply's limits are marked and cannot be sent. The chart under the table shows frequency and duty over time, with the gaps shaded.
   - **Send shot to controller** uploads it and then **reads it back from the controller and compares**; a green message appears only when what the controller holds is what is in the table. If the controller already holds
     a shot the page asks before replacing it.
   - **Compare with controller** lists every setpoint that differs between the table and the controller, without sending anything. **Read from controller** puts the controller's shot into the table.
     **Save as CSV** and **Copy as commands** give you the table as a file or as `SHOT:BEGin` / `SHOT:STEP` / `SHOT:END` lines for the logbook.
4. **Run**: ARM, then FIRE, in the State box. The **external trigger** switch shows whether a rising edge on the trigger input will start the shot while ARMED; the page warns when it is ARMED with the trigger on.
   After a shot the section shows the **start latency** the controller measured (`EXTernal:TRIGger:LATency?`). Before ARM the page warns if the controller holds no shot, if the table has not been compared with the controller's shot since
   one of them changed (the controller fires what **it** holds, whatever the table shows), if a fault input is switched off, or if the fault bypass is on.
- **Protection: fault inputs.** Which fault sources are latched (`FAULT:SOURces?`), the twelve **gate-driver status inputs** as cards (level, name, healthy / FAULT / switched off) and the **spark-gap input**.
  **Name** gives an input a label. The **On/Off** switch takes an input out of the protection: switching one **off asks for confirmation**, is refused while ARMED or FIRING, and is shown in the section's summary
  ("1 switched off") and in the question before ARM. The polarity in force is shown; changing it is left to the Console (`GDS:POLarity`, also confirmed). A red banner appears if the **fault bypass** is on, with a button to turn it off.
- **Saved profiles.** The eight slots in the controller's own memory. **Save here / Replace** stores everything set now (supply, shot, phases, dead time, fault-input settings) under a name; **Compare** lists what a recall
  would change; **Recall** shows that list again, with the lines that change a protection, the dead time or the phases **in red**, and asks; afterwards it reports refused values and how many fault inputs the profile left switched off.
  **Erase** empties a slot. A slot whose saved data no longer checks out is shown as **damaged**: it cannot be recalled, only saved over or erased. All but Compare work only while IDLE. Nothing is ever recalled by itself.
- **Settings: dead time, control loop.** The dead time (50 to 3000 ns; the value in force is read back, in steps of 5.9 ns) is confirmed before it is sent. The per-phase open/closed loop switches are here.
- **Capacitor charger (CHARGE)**: a 50 % square wave (1 Hz to 100 kHz) for the capacitor-bank charger, independent of the state. Start/change it, or press **OFF**. It stays on through a fault until you turn it off. (Needs firmware v0.11 or later; older firmware says so.)

A controller with older firmware simply lacks some of this (no spark-gap input, no per-input switches, a fixed dead time): the page says so in that section and the rest works.

### XREX (magnet supplies)
1. **Shot timing**: ramp up, flat top, ramp down (seconds). 2. **Channels** (four cards): whether the channel is in the shot, its enable line and contactor, its loop mode and gains, its peak current, the raw levels of its fault inputs, and its live setpoint/measured/output frequency.
3. **Run**: ARM, then START SHOT. ARM is refused unless every channel in the shot has its enable line and contactor on. A red banner appears here if the fault bypass is on.
- **Saved profiles** and **Settings: dead time** work as on HBRIDGE (an XREX profile does not keep channel enables, the ready lines or the dead time; a recall that changes a fault polarity, a current or frequency limit, or a board enable output shows that line in red).

The readings refresh every few seconds (choose Off, 2, 5, 10 or 30 s at the top of the Operate tab, or **Refresh now**).

## 4. Recording a shot (data, plots, report)

Both products can log what the control loop did during a shot, and the page will collect, save, plot and summarise it for you.

1. In the State box on the left, tick **Record data for the next shot**. (Check the **Shot data** tab once: the channels or phases to record and the **expected shot length** are filled in from the shot you set up; change them if needed. The controller's log holds about 1000 samples, so a longer shot is recorded at a lower sample rate. Give the length a little long rather than short.)
2. Press **ARM** and confirm. The line under the checkbox changes to "Recording armed". Then start the shot as usual (FIRE, START SHOT, or the external trigger).
3. When the shot ends, the page reads the log back from the controller (a progress figure shows), saves it, and opens the **Shot data** tab on the new recording.

What you get for each channel (XREX) or phase (HBRIDGE): a plot of **setpoint, measured feedback and output** against time, and the figures an operator reads off a shot: peak setpoint and measured peak, rms and largest error, overshoot, the flat-top mean and ripple, and the output range. XREX plots can be shown in **amps or hertz** (amps are estimated from the controller's linear frequency-to-current calibration, which is a placeholder until it is measured on a real Transrex: the hertz values are exact). HBRIDGE shows target and measured in volts and the output (kHz for HVPS, % duty for Limiter) in a second panel.

Every recording is also **saved automatically** on your computer, in `host/operator_ui/shots/<board serial>/<date-time>/` (or the folder given with `--shots-dir`): `shot.csv` (the data, for Excel or Python), `report.html` (a self-contained report with the plots: open it in any browser or attach it to an e-mail), `report.md`, and `shot.json` (everything, including what was commanded). The same four files can be downloaded from the page. Recordings of a controller stay listed on its Shot data tab.

### Recording the PFM inputs (Limiter and HVPS): carrier and duty, open loop too

The controller can also measure the signal fed back to its **PFM inputs** (inputs 1 to 3 carry phases U, V and W on the bench), cycle by cycle. This does not need a closed loop, so it is how you record an ordinary open-loop shot. On the **Shot data** tab, under "Record the next shot", tick **PFM input capture** and choose:

- the **inputs** (default: the phases that are on);
- **Both edges: period and duty** (default) or **Rising edges: period only**. Rising-only gives the carrier frequency of every cycle; both edges also gives the duty of every cycle (the high time as the controller sees it, after the fibre and receiver);
- the **cycles** to capture. Leave it empty and the page sizes it from the shot (the highest carrier in the programmed shot times the expected shot length). The controller holds at most 3800 cycles, about 38 ms at 100 kHz: a longer shot is captured from its start and the page says the end is missing.

Then tick **Record data for the next shot** and press **ARM** on this page. With the PFM capture ticked the page arms the recording *before* it sends ARM, because the capture modes can only be changed while the controller is IDLE. If the controller is already ARMED (LabVIEW, another console), the page refuses to change a mode and says to DISARM first; an input that is already in the wanted mode needs no change. You may also tick **Waveform log** phases to record both at once, or untick them all to record only the PFM capture. After the shot the page reads each input back (100 values per reply, each checked and asked again when a character was lost), saves it, and **puts the capture modes back** as they were.

The recording shows, for each input, the cycle count, the carrier (mean, minimum, maximum), the duty range (both edges) and the missed captures (OVERCAP); plots of duty and of carrier over the shot; and, when the programmed shot can be read back from the controller (`SHOT?`, up to 100 setpoints), the **programmed value dashed under the measured one** with the mean and spread of the difference. The comparison assumes input *n* is wired to phase *n* (U, V, W for 1, 2, 3). Extra file: `pfm.csv` (one row per cycle, all inputs, with the programmed values), also in the report. On the bench the measured duty reads about 2 percentage points below programmed, constant over the shot (probably dead time; not confirmed), so expect that offset, not a match.

What the numbers do not tell you: the first captured cycle is marked *startup* (the capture begins just before the output) and a cycle whose period is more than 25 % from the median is marked *odd period* (an edge was lost in the record: a missing edge shifts every later rising/falling assignment); both are left out of the lines and the statistics, but kept in `pfm.csv`. A wrong-looking duty on one input only usually means that input's fibre is not connected or is patched to another phase.

Good to know:
- **HBRIDGE's waveform log records only closed-loop runs.** A shot played open-loop (the table of setpoints you build in step 3 of the Operate tab) is not logged by the controller (use the PFM input capture above for those); the page warns when every selected phase is open-loop and says "nothing recorded" afterwards. Closing a loop needs a target (`PID:LOOPMODE <phase> 1` and `TARGet:*` in the Console): there is no screen for it yet.
- A shot that ends in a **fault** is still recorded as far as the controller's loop ran (the fault ramp-down is included) and is marked "fault".
- If you disarm before the shot, the page reports "nothing recorded". If the page was closed or the link dropped during a shot, **Read the controller's log now** (Shot data tab) fetches whatever the controller still holds: it keeps only the latest log, and a new ARM-with-recording replaces it.
- Only one recording can wait for a shot at a time; **Cancel recording** gives it up.
- Reading the data takes a few seconds on XREX (one long reply per channel) and a little longer on HBRIDGE (40 samples per reply, each checked with a check sum and asked again when a character was lost).

## 5. Confirmation questions

Commands that **arm, start output, change which phases drive, start the charger, turn on the external trigger, clear a fault, switch a fault input off, change the fault polarity, turn the fault bypass on, or change the dead time** (the project's dangerous-command list) always ask *"Are you sure?"* in plain words, show the exact command,
and say how many other people are connected. **Cancel is the default.** This check is made by the page's server, not only on the screen: a command on that list is refused unless you confirmed it.
If the host is set to hold dangerous commands, the page answers the host's question itself after you confirmed on the page.

The page also asks before things that are not on that list but can surprise: **recalling or erasing a saved profile** (with what would change), **replacing a shot** the controller already holds,
**disconnecting** from a controller that is ARMED or FIRING, **closing a tunnel** that controllers are connected through or that the page did not open, and **quitting**.

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
| Load file: "this file has no frequencies yet" | The file is a demand only (power or duty wanted). Run the compensation tool on it and load the `_iterN.csv` it writes. |
| Tunnel "partly open" | Only some ports answer: a tunnel for one controller was opened in a terminal. Use it for that controller, or Close tunnel and open it again from the page. |
| "This page cannot close that tunnel" | The ports are held by a program that is not ssh, or by another user's process. Close it where it was started. |
| "The console program on this computer is not answering" | The console program on this computer was stopped (its window was closed). Start it again and reload the page. |

## 7. Limits of this page

It does not lock out other people (the host has no lock either), has no passwords, and does not update firmware (use `python/fw_update.py` in the firmware project). It listens on **your computer only**; do not start it with `--listen` on a shared network, because anyone who can
reach it could command the controllers you have open. Developers: `host/operator_ui/wham_ui.py` (server), `shot_file.py` (reads a shot from a file or pasted text), `recorder.py` (shot recording), `shot_data.py` (units, statistics, plots, files), `static/` (page; `static/help/` holds the command flowcharts, generated by `firmware/*/python/gen_command_flow.py`: do not edit them there), `wham_sim.py` (the simulated controller: shots with gaps, fault inputs, saved profiles, dead time, synthetic shot logs), tests in `host/tests/test_operator_ui.py`, `test_shot_file.py` and `test_shot_data.py`.
