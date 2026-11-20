# The operators' diary

One plain text file on the lab server that everyone working with the controllers can read and add to: a shared logbook
for what was done, changed or noticed. It lives with the hubs (`/var/lib/wham/hub/diary.txt`), one file for all controllers.

Every entry is one line, and every line a reader sees carries **the date, the time, who wrote it and where**:

```
2026-11-20 11:02:13 | Everett Penne (Slack) | ech | Raised the bank charge to 540 V for the afternoon shots.
2026-11-20 11:20:40 | epenne | lmtr-ctrl | Limiter shot 14: droop compensation iteration 3 loaded.
```

- **time**: the server's clock, to the second.
- **who**: from Slack, your Slack name followed by "(Slack)"; through a hub, the name your program gave the hub
  (your user name in the operator console and `net_client.py`, the program's name for LabVIEW).
- **where**: the controller you were connected to when you wrote it (the hub's name, or the session's controller in
  Slack), or `general`.

## Commands

| From Slack (direct message with the bot) | From a hub (console's Console tab, `net_client.py`, any hub client) | What it does |
|---|---|---|
| `diary write <text>` | `!hub diary write <text>` | adds an entry (up to 1000 characters; several lines become one, joined with " / ") |
| `diary read` | `!hub diary read` | today's entries |
| `diary read 2026-11-20` (or `today`, `yesterday`) | `!hub diary read 2026-11-20` | one day |
| `diary read all` | `!hub diary read all` | everything; when that is too much the newest are shown and the reply says how many there are |
| `diary last 5` | `!hub diary last 5` | the newest entries (10 without a number) |
| `diary search valve` | `!hub diary search valve` | entries containing the text (in the text, the writer or the place) |
| `diary dates` | `!hub diary dates` | the days that have entries, with how many |
| `diary index` | `!hub diary index` | **who wrote when and where, without the texts**: one line per entry, all days (`diary index today`, or a date, for one day) |
| `diary index file` | (Slack only) | sends that index of the whole diary as a text file to download |
| `diary file` | (Slack only) | sends **the whole diary as a text file** into the conversation, to download: the file exactly as stored, however long it is |
| `diary correct 11:02 <text>` | `!hub diary correct 11:02 <text>` | adds an entry marked as a correction of the one written at that time (`2026-11-19 14:30` for another day) |

`read`, `last`, `search` and `index` also take `by=<who>` and `at=<where>`: `diary read all by=everett`, `diary last 20 at=ech`.
In Slack the words work with or without the leading dot, also while connected to a controller: a line starting with
`diary` is never sent to a controller. `/diary ...` works too if the Slack app registers `/diary` as a slash command.

## Good to know

- **Entries are never edited or removed by a command.** The diary is a record: a mistake is put right with `diary correct`,
  which adds a new entry naming the old one. (The file is ordinary text, so the server's owner can still edit it by hand.)
- **Who can write:** anyone who can reach a hub, and the people on the Slack bot's allow list. The name is what the
  program reports, as everywhere in the hubs: there are no passwords.
- When an entry is written through a hub, the other people connected to *that* hub see a line `--- diary: ... ---`
  (programs connected in quiet mode, such as LabVIEW, do not). A Slack `watch all` shows those lines too.
- Slack shows at most 40 entries per answer and a hub at most 200; ask for a date or search to see older ones, or use
  `diary file` in Slack to get everything as one file. (`diary file` needs the Slack app's `files:write` permission; the bot
  says so if it is missing.)
- Writing through a hub is also recorded in that hub's own log (`!hub log`), as every hub command is.
- The file's place can be changed with `diary_file` in the hub configs (and in the bot's settings); all hubs must name
  the same file. Code: `host/diary.py`; tests: `host/tests/test_diary.py`.
