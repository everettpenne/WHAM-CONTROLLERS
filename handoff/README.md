# Project handoff: WHAM controllers

| | |
|---|---|
| **Snapshot date** | 2026-11-16 (US Central) |
| **Repository state when written** | `main` after commit `666fd35` (the handoff commit itself follows it) |
| **Firmware on the bench boards at that time** | HBRIDGE `v0.11 c36f382e` on the ECH-HVPS (WB-0001-8) and the Limiter (WB-0002-6); XREX `v0.8i db83dcd9` on XREX-CTRL (WB-0003-4) |
| **Written for** | the engineer taking over the controller work, and the Claude instance assisting them |

This folder consolidates what has been learned while building this project: what the system is, the design decisions that have
been settled, how to work on it safely, what is fragile, what is broken in hardware, and what is unfinished. It is a
**summary and a map**. The detailed, authoritative records stay where they are: each firmware's `AGENTS.md`,
`docs/changelog.txt`, `docs/command_reference.md` and SOP. When this folder and those disagree, trust the newer dated
entry in the changelog, and then update this folder.

## Reading order

| # | File | Read it when |
|---|---|---|
| 0 | [`00_CLAUDE_START_HERE.md`](00_CLAUDE_START_HERE.md) | **First, if you are an AI assistant.** The rules for working on this project and with this hardware |
| 1 | [`01_project_overview.md`](01_project_overview.md) | First, if you are a person. What the system is, the two products, the boards on the bench |
| 2 | [`02_architecture.md`](02_architecture.md) | Before changing code. How the hardware, firmware and host software fit together |
| 3 | [`03_design_decisions.md`](03_design_decisions.md) | Before "fixing" something that looks odd. What was decided on purpose, and why |
| 4 | [`04_workflow_and_conventions.md`](04_workflow_and_conventions.md) | Before your first build or commit |
| 5 | [`05_operations_and_best_practices.md`](05_operations_and_best_practices.md) | Before touching a board: connecting, identifying, updating firmware, running shots safely |
| 6 | [`06_known_hardware_issues.md`](06_known_hardware_issues.md) | When a board behaves strangely, and before the next board revision |
| 7 | [`07_fragile_software.md`](07_fragile_software.md) | Before editing anything near interrupts, the HRTIM, CubeMX files, saved profiles or shared code |
| 8 | [`08_open_items.md`](08_open_items.md) | When planning work. Unfinished and unverified items |
| 9 | [`09_glossary_and_quick_reference.md`](09_glossary_and_quick_reference.md) | Any time. Terms explained for an electrical engineer new to embedded work, plus a command cheat sheet |

## How to use this with Claude

The repository root has a `CLAUDE.md` that points Claude Code at this folder, so a fresh session in the repository finds it by
itself. With another Claude client, give it this folder (or at least `00_CLAUDE_START_HERE.md`) at the start of the conversation,
and tell it which firmware (`xrex` or `hbridge`) and which board you are working on.

## Keeping this folder current

* Every file starts with a "Snapshot" line. When you change a file, update its line with the date and the commit it describes.
* When an open item is finished, move it from `08_open_items.md` to the relevant changelog entry (do not just delete it).
* When a new hardware problem or fragile area is found, add it to `06` or `07` with the date, the symptom, and the evidence.
* This repository is **public**. Do not put passwords, personal contact details, or anything sensitive in these files. The bench
  network addresses are already published in `docs/bench_devices.md`; refer to that file rather than copying them.
