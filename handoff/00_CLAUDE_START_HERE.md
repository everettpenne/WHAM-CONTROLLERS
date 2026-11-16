# Start here (for an AI assistant working on this project)

Snapshot: 2026-11-16, `main` after `666fd35`.

You are helping an electrical engineer who is new to embedded firmware continue this project. They know circuits, power
electronics and test equipment. They may not know C toolchains, interrupts, git, or STM32 specifics. Explain the embedded
and software reasons behind what you do, in plain terms, and show the command you ran and what it proved.

## 1. The rules that protect people and hardware

These come from the project owner and from real incidents. They are not suggestions.

1. **These boards drive real high-voltage and high-current supplies.** A "shot" (`FIRE`, `SHOT:STARt`, `SOURce:RUN`, an
   external trigger edge) makes real power electronics switch. `ARM` makes a board ready to do that on the next edge.
2. **Never start output, `ARM`, or fire a shot without the user's explicit OK in the current conversation**, for that board
   and that action. The ECH-HVPS controller (serial **WB-0001-8**) is **installed in its supply**: no shots on it without
   the owner's OK. Queries and settings commands are acceptable on it only while it reads `STATE? -> OK IDLE`.
3. **Always ask before flashing firmware to any controller.** State what image (commit), which board (serial), and why. Each
   flash needs its own confirmation that the board is on a bench or otherwise safe. One approval does not carry over.
4. **Identify every board by its reply to `*IDN?` (product name and `SN=`), never by IP address, port, USB device name or
   hub port number alone.** The bench network has at least one *unrelated* high-voltage controller on the same subnet and port
   (`docs/bench_devices.md` and section 5 of `05_operations_and_best_practices.md`).
5. **Never pipe pre-written answers (`y`, `yes`) into a tool that asks for confirmation before a dangerous action.** This
   once fired a real, unconfirmed shot (2026-09-14). Drive such tools one step at a time and check the board state between
   steps.
6. **Switching off a protection is a deliberate act.** `GDS:ENAble 0 0`, `GDS:POLarity`, `DEBUG:FAULT:BYPASS 1`,
   `EXTernal:ENAble 0` and `FAULT:CLEar` all weaken or remove an interlock. On a bench board with nothing wired they are the
   normal way to reach `IDLE` (needed for a firmware update). On a board connected to a supply, do them only when the user
   has said why it is safe.
7. **Do not test timing-dependent refusals over the network bridge.** A round trip is about 65 ms; a short shot can finish
   before your "should be refused" command arrives, and the command then really runs. (This once sent a board into its ROM
   bootloader.) Use shots of several seconds and confirm `STATE? -> OK FIRING` first.
8. **Report results honestly.** Separate "builds", "host tests pass", "command works over serial", and "seen on the pins with a
   logic analyzer or scope". The changelog tags every entry with which of these was actually done. Never round "host-tested"
   up to "verified".

## 2. How work is done here

* Work in the monorepo `WHAM-CONTROLLERS` (this repository). The older standalone repositories `WHAM-XREX-PFMG474` and
  `WHAM-HBRIDGE-PFMG474` are frozen at 2026-10-07 and are behind. Do not work there.
* Each product is self-contained under `firmware/<product>/`. **Read that product's `AGENTS.md` before changing its code.**
  Then read the matching section of `docs/changelog.txt` for anything you are about to touch.
* Build only with `python3 python/wham_build.py` from inside `firmware/<product>/` (section 4 of `04_workflow_and_conventions.md`
  explains why a bare `make` is a trap).
* Keep the layered source layout (`04_workflow_and_conventions.md`, section 3). `check_layout.py` fails the build if you break it.
* After any behavior change: add a dated `docs/changelog.txt` entry with a status tag, update `docs/command_reference.md` for any
  command change, add or update a host test in `tests/`, and record the flash/RAM footprint (`python/memory_report.py`).
  Do these without being asked; they are standing instructions.
* Commit only when the user asks. Do not add AI attribution lines to commit messages (owner's instruction). Push only when asked.
* Code shared byte-for-byte between the two products must be changed in both, then checked with `python/check_shared.py`.
* If a rule or invariant seems wrong, say so and propose changing it deliberately (with a changelog entry). Do not work around it.

## 3. Where the truth lives

| Question | Authoritative source |
|---|---|
| What does command X do, what are its arguments and errors? | `firmware/<product>/docs/command_reference.md` |
| Why is the code like this? What was tried before? | `firmware/<product>/docs/changelog.txt` (newest first, dated, with root causes) |
| What must never be changed casually? | `firmware/<product>/AGENTS.md` ("Hard-won invariants", "Source layout") and `07_fragile_software.md` |
| Which pin is which? | `firmware/<product>/docs/pin_mapping_v4.csv` (the "XREX Pin Name" column is the XREX meaning) |
| How do I operate the board safely, step by step? | `firmware/<product>/docs/operators_guide.md` and the SOP in `docs/sop/` |
| How does a front end (LabVIEW, scripts) talk to it? | `docs/labview_*.md` at the repository top, and `firmware/<product>/docs/labview_interface.md` |
| What is saved in a profile? | `firmware/hbridge/docs/saved_profiles.md`, `firmware/*/docs/saved_profiles_shared.md` |
| Which board is where on the network? | `docs/bench_devices.md`, `host/devices.csv`, `host/config/*.yaml` |

## 4. When you are unsure

Ask. The owner prefers one clear question over a guess, especially for anything that changes output, protections, flash
contents, saved profiles, or a frozen identifier (error codes, saved-profile field ids). Explain the choice and its
consequences in a sentence or two so that a non-specialist can decide.
