# Instructions for Claude in this repository

Before doing anything else, read `handoff/00_CLAUDE_START_HERE.md`, then `handoff/README.md` for the rest of the handoff set
(project overview, architecture, settled decisions, workflow, operations, known hardware issues, fragile software, open items,
glossary). Before changing a product's code, read `firmware/<product>/AGENTS.md`.

The short version of the safety rules (the full text is in `handoff/00_CLAUDE_START_HERE.md`):

* These controllers drive real high-voltage supplies. Never `ARM`, `FIRE`, `SHOT:STARt`, `SOURce:RUN` or otherwise start output without the
  user's explicit OK for that board in the current conversation. The ECH-HVPS controller (WB-0001-8) is installed in its supply.
* Always ask before flashing firmware to any controller, and confirm each time that the board is safe.
* Identify every board by `*IDN?` (product and `SN=`) before sending anything else.
* Never pipe pre-written answers into a confirmation prompt.
* Build only with `python3 python/wham_build.py` inside `firmware/<product>/`; keep the changelog, command reference, tests and memory report
  up to date with every change; commit only when asked, with no AI attribution lines.
* This repository is public: no secrets or personal details in commits.
