# The Slack bot: a text terminal to the controllers, through the hubs

`host/slack_bot/wham_slack.py` runs next to the hubs on the lab server and lets the people you list talk to a controller
from Slack. It connects to Slack by an outgoing connection only (Socket Mode): nothing listens on the server and no port
is opened in the lab network.

## Using it

Open a direct message with the bot (or use `/wham <words>`).

| You type | What happens |
|---|---|
| `list` | every controller: state, who else is connected |
| `connect ech` | opens a session to that controller through its hub (`xrex`, `limiter`, `ech`) |
| `STATE?`, `SHOT?`, `CONFig:MODE HVPS`, ... | while connected, **every line you type is sent to the controller** and its reply is posted. Several lines in one message are sent in order (at most 20) |
| `.hublog on` / `.hublog off` | **hub-log mode**: everything the hub sees is shown, live: other clients' commands as `[name] > command`, their replies as `[name] < reply`, joins and leaves, the controller's events |
| `.log 20 since=1h grep=ARM` | the hub's *stored* log (what happened earlier); also `kind=cmd`, `client=<name>` |
| `.who` | who is connected to that hub |
| `.disconnect` | closes the session (the controller keeps doing what it does) |
| `.help` | the list above |

While connected, a line that starts with a **dot** is for the bot and everything else goes to the controller. Not
connected, the dot is optional, and `ech state` or `ech SHOT?` asks one thing without opening a session (read only).

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
your commands as anyone's, and the operator console lists you under "People on this controller". A session nobody types
in for 30 minutes is closed (`session_idle_min`), so a forgotten one does not hold up a hub update.

## Setting it up (once)

1. **Create the Slack app** (a workspace admin, at https://api.slack.com/apps, "From a manifest"):

   ```yaml
   display_information:
     name: WHAM
   features:
     bot_user:
       display_name: wham
       always_online: true
     app_home:
       messages_tab_enabled: true
       messages_tab_read_only_enabled: false
     slash_commands:
       - command: /wham
         description: WHAM controllers through the hubs
         usage_hint: list | connect ech | .disconnect
   oauth_config:
     scopes:
       bot: [app_mentions:read, chat:write, im:history, commands, users:read]
   settings:
     event_subscriptions:
       bot_events: [app_mention, message.im]
     socket_mode_enabled: true
   ```

   Then: *Basic Information → App-Level Tokens* → create one with scope `connections:write` (the `xapp-...` token);
   *Install App* → install to the workspace (the `xoxb-...` bot token).
2. **On the server**, as the hubs' account, copy `host/slack_bot/slack_bot.example.json` to
   `host/slack_bot/slack_bot.local.json` (git ignores it), put in the two tokens, and list who may use it:
   `allow_users` (Slack member IDs, `U...`: profile → ⋮ → *Copy member ID*) and/or `allow_channels` (`C...`). With both
   empty the bot answers nobody. `chmod 600` the file. **Never put a token in a tracked file: the repository is public.**
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
