# The Slack bot: a text terminal to the controllers, through the hubs

`host/slack_bot/wham_slack.py` runs next to the hubs on the lab server and lets the people you list talk to a controller
from Slack. It connects to Slack by an outgoing connection only (Socket Mode): nothing listens on the server and no port
is opened in the lab network.

In Slack the app is called **WHAM-PFM-Controller-Link** and the bot that answers is **Host-Link_PFM-Controller**
(workspace WHAM). Slack shows a new app in the sidebar only after you open it once: press Cmd+K (Ctrl+K), type
`WHAM-PFM`, pick the app, and open its **Messages** tab.

## Using it

Open a direct message with the bot (or use `/wham <words>`).

| You type | What happens |
|---|---|
| `list` | every controller: state, who else is connected |
| `connect ech` | opens a session to that controller through its hub (`xrex`, `limiter`, `ech`). The bot greets you by name, shows the controller's state and who else is connected, and warns if it is not IDLE or someone else is on it |
| `STATE?`, `SHOT?`, `CONFig:MODE HVPS`, ... | while connected, **every line you type is sent to the controller** and its reply is posted. Several lines in one message are sent in order (at most 20) |
| `.hublog on` / `.hublog off` | **hub-log mode**: everything the hub sees is shown, live: other clients' commands as `[name] > command`, their replies as `[name] < reply`, joins and leaves, the controller's events |
| `.log 20 since=1h grep=ARM` | the hub's *stored* log (what happened earlier); also `kind=cmd`, `client=<name>` |
| `.log file [today\|yesterday\|YYYY-MM-DD]` | **a whole day of that hub's log as a file** to download: one readable line per row (time, kind, client, line), without the hub's own heartbeat queries. Add `raw` for the day file exactly as the hub stored it (JSON lines, everything). Not connected: `log ech file`. `log all file` merges every hub's day in time order |
| `.log days` | which days of log the hub keeps, with their sizes (`log ech days`, `log all days` when not connected) |
| `!hub read shotnumber 261120015` | (in a session) the shot record filed under that number in this hub's log: the waveform and settings the controller held. `!hub shotnumber <number>` files one now. See `docs/device_identity_and_host_service.md` |
| `.who` | who is connected to that hub |
| `.disconnect` | closes the session (the controller keeps doing what it does) |
| `watch all` / `watch off` | **every controller at once**: the live traffic of all hubs in one stream, each line marked with its controller, for example `[ech] [labview] > STATE?`. It only watches: nothing you type is sent to a controller. It works beside a session (your own lines then show twice) |
| `log all 20 since=1h` | the *stored* logs of every hub, merged in time order (same filters as `.log`) |
| `diary write <text>`, `diary read [date\|all]`, `diary last`, `diary search <text>`, `diary index`, `diary file` | the operators' shared diary on the server: every entry carries its date, time and writer. See `docs/diary.md` |
| `.users` | who is authorized to use the bot, by name, and whether anyone may send dangerous commands |
| `repo` | the link to the project's public repository, https://github.com/everettpenne/WHAM-CONTROLLERS (through a hub: `!hub repo`) |
| `help` | the list of commands. It works at any time, with or without the dot, also while connected (it is never sent to a controller) |

While connected, a line that starts with a **dot** is for the bot and everything else goes to the controller. Not
connected, the dot is optional, and `ech state` or `ech SHOT?` asks one thing without opening a session (read only).

**Which controllers.** Every hub the server runs. The bot reads the hub configuration files next to it
(`host/config/<name>.yaml`, the same files the hub services start from) about twice a minute, so **a controller that gets a
hub appears by itself** in `list`, `connect` and `watch all`, under its hub's name (the beginning of a name is enough:
`connect new`). The three installed ones keep their short names (`xrex`, `limiter`, `ech`), with the hub's name as an alias
(`ech-hvps`). The bot does not scan the network and never talks to a controller's bridge: a new controller needs a hub
first (`docs/bench_devices.md`), and a config file whose hub is not running shows in `list` as "the hub does not answer".
`"discover": false` in the settings turns this off.

**Why not `/list` and `/connect`:** Slack keeps a leading `/` for commands an app has registered, and such names are shared
by the whole workspace. The app registers one, `/wham` (`/wham list`, `/wham connect ech`). If you want the short names,
add `/list` and `/connect` as slash commands of the app too: the bot understands them as they are.

## What it will and will not send

- **Queries and ordinary commands:** sent, from anyone on the allow lists, in a session.
- **Dangerous commands** (`host/dangerous_commands.py`: `ARM`, `FIRE`, `FAULT:CLEar`, `CHARGE:FREQuency <hz>`, switching a
  fault input off, the bypass, dead time, phases, ...): **refused**, unless your Slack user ID is in `danger_users`. Then the
  bot shows the command and its category and sends it only after you answer `.confirm` within 30 s; anything else cancels.
  **`danger_users` is empty by default**, so as installed nobody can arm or fire from Slack. Putting a user ID there is a
  decision about that person: Slack gives no view of the supply, and the bot cannot know who is standing next to it.
- **Firmware and identity commands** (`FWUPdate:*`, `BOOT`, `SYS:SERial`, ...): never sent, whoever asks.
- **Long dumps** (`LOG:DATA?`, `SYS:EVLOG?`, `PFMIN:DATA?`, ...): refused; they hold the controller's one serial link for
  seconds and do not fit a message.
- A flood of traffic in hub-log mode is cut (long lines shortened, at most three messages per batch, the rest counted).

A session is an ordinary hub client named `slack-<your Slack name>`: the hub's log records who sent what, other clients see
your commands as anyone's, and the operator console lists you under "People on this controller". A watch is one such
client on every hub, named `slack-<you>-watch`. A session or a watch that hears nothing from you for 30 minutes is closed
(`session_idle_min`), so a forgotten one does not hold up a hub update (a hub is updated only when nobody is connected).
When the bot itself is stopped or restarted, it first says **goodbye** in every conversation with an open session or
watch, closes them, and says when to connect again. It restarts by itself only when no session is open.

## Setting it up (once)

1. **Create the Slack app** (a workspace admin, at https://api.slack.com/apps, "From a manifest"):

   ```yaml
   display_information:
     name: WHAM-PFM-Controller-Link
   features:
     bot_user:
       display_name: Host-Link_PFM-Controller
       always_online: true
     app_home:
       messages_tab_enabled: true
       messages_tab_read_only_enabled: false
     slash_commands:
       - command: /wham
         description: WHAM-PFM-Controller-Link, the controllers through their hubs
         usage_hint: help | list | connect <xrex|limiter|ech> | users
   oauth_config:
     scopes:
       bot: [app_mentions:read, chat:write, im:history, commands, users:read, files:write]
   settings:
     event_subscriptions:
       bot_events: [app_mention, message.im]
     socket_mode_enabled: true
   ```

   To change the names or the `/wham` hint of an app that already exists: its settings → *App Manifest*, edit, *Save*
   (or *Basic Information → Display Information*, *App Home*, and *Slash Commands → edit*). The bot needs no change.

   Then: *Basic Information → App-Level Tokens* → create one with scope `connections:write` (the `xapp-...` token);
   *Install App* → install to the workspace (the `xoxb-...` bot token).
2. **On the server**, as the hubs' account, copy `host/slack_bot/slack_bot.example.json` to
   `host/slack_bot/slack_bot.local.json` (git ignores it), put in the two tokens, and list who may use it:
   `allow_users` (Slack member IDs, `U...`: profile → ⋮ → *Copy member ID*) and/or `allow_channels` (`C...`). With both
   empty the bot answers nobody. `chmod 600` the file. Later edits to the three lists (`allow_users`, `allow_channels`, `danger_users`) take effect at the next message, without a restart; the tokens are read only when the bot starts. **Never put a token in a tracked file: the repository is public.**
3. `python3 host/slack_bot/wham_slack.py check` (tokens accepted, each hub answers; changes nothing), then
   `python3 host/slack_bot/wham_slack.py install-cron`: cron starts the bot at boot and again within 5 minutes if it stops.
   No sudo is needed. Its log is `/var/lib/wham/hub/slack/bot.log`.

The bot follows the checkout: when `hub_update.py` moves it to a new commit and no session is open, the bot stops and cron
starts it on the new code. `python3 host/slack_bot/wham_slack.py ask "ech state"` shows what it would answer, without Slack.

## Limits

Slack is not a control room: messages can be delayed, and a reply shows only what the controller answered. There is no
passwording beyond Slack's own login and the allow lists, so anyone who can use an allowed Slack account can send what that
account may send. To use a session in a channel instead of a direct message, also subscribe the app to `message.channels`
and add `channels:history`; otherwise each line there needs a mention of the bot. Tests: `host/tests/test_wham_slack.py`.
