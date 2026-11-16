# 4. Workflow and conventions

Snapshot: 2026-11-16, `main` after `666fd35`.

## 4.1 Tools you need

* **STM32CubeIDE** (includes the `arm-none-eabi-gcc` 13.3 toolchain; put its `bin/` on your `PATH` so `make` and `arm-none-eabi-objcopy` work from a terminal).
* **Python 3** (the host tools are standard library only; `pyserial` for serial links; `numpy` and `matplotlib` for plots and tests that plot).
* A host C compiler (`cc`/`gcc`/`clang`) for the unit tests.
* **git**, and the GitHub CLI `gh` if you work with pull requests.
* For hardware: an **ST-Link V2** (SWD), `openocd` (or `st-flash`), a USB-serial adapter (CH340), a logic analyzer (the project used a DSLogic U3Pro32 with DSView), a meter and a scope.

## 4.2 Building

From inside the product folder:

```bash
cd firmware/hbridge          # or firmware/xrex
python3 python/wham_build.py # XREX simulator image: add --target simulator
```

`wham_build.py` does, in order: generate `build/generated/git_version.h` (the commit `*IDN?` reports) and `build_target.h`; check that
`Debug/`'s makefiles match the source tree; check the layout rules; run the host unit tests; `make`; `objcopy` to `.bin`; copy
`.elf/.bin/.map` to `build/`. The flashing and update tools read `build/`.

**Do not use a bare `make` for anything you will flash.** It never produces the `.bin` and never updates `build/`, so you would flash
an old image that "verifies" perfectly (this happened on 2026-09-10). A bare `make` in `Debug/` has also been seen to run the clean goal.

**`-dirty` in the version.** The commit string ends in `-dirty` when *anything* in the repository is uncommitted at build time (the whole
monorepo, not just the product). The images flashed on 2026-11-16 are `-dirty` because unrelated host files were uncommitted. To know
exactly what a board runs, build from a clean tree.

## 4.3 Testing

```bash
make -C tests                         # inside firmware/<product>: every host unit test (also run by wham_build.py)
python3 -m unittest discover -s host/tests   # at the repository root: hub, dangerous-command list, operator UI
python3 python/check_shared.py        # inside either product: byte-identical shared files still match
```

* Hardware-free logic (parsers, shot generator, profile codec, state machine, PID core) must have a host test in `tests/`.
* `test_command_table` checks every command is reachable by its short and long form, so a new command that collides with an
  existing one fails the build.
* Known pre-existing failures: `check_shared.py` reports `uart.c`/`uart.h` differing between the products (a deliberate XREX-only
  change from 2026-11-09, not yet reconciled); `test_wham_hub.SlowClientTests` fail on macOS.
* Hardware checks: `python/pfm_shot_test.py` (fire a shot, capture the outputs on the PFM inputs, compare with the table) and
  `python/pfm_gap_test.py` (gaps). They need the outputs wired to the PFM inputs and they FIRE: ask first.

## 4.4 Changing code: the checklist

1. Read the product's `AGENTS.md` sections for the area, and the changelog entries that touched it.
2. Put the code in the right layer (`src/app` for product logic, `src/drivers` + `src/bsp/stm32g4` for anything touching hardware).
3. **Added, moved or renamed a source file?** Run `python3 python/sync_build_sources.py` (regenerates CubeIDE's `Debug/` makefiles and
   `.cproject`; the build refuses to run while they are stale).
4. **New command?** Handler in `src/app/commands/cmd_<subsystem>.c`, declaration in `commands.h`, a row in `command_table.c`, errors with
   `Scpi_SendErr` and an existing error code, documentation in `docs/command_reference.md`. If it can start output or weaken a protection,
   add it to `host/dangerous_commands.py` and its test.
5. **New operator setting?** Decide whether it belongs in a saved profile. If yes: a new field id (never reuse one) in
   `settings_codec.h`, the table row in `settings_codec.c`, capture and apply in `settings.c`, and tests. Watch the presence-bitmap size
   (`SET_PRESENT_BITS`) and the payload budget (`test_settings` prints the worst case; 15,704 of 16,336 bytes on HBRIDGE).
6. Build with `wham_build.py`; all tests must pass with zero warnings.
7. Add a changelog entry (4.6), update the docs that describe the behavior, and record the footprint (4.7).
8. Commit (when asked), then commit the rebuilt image separately (4.8).

## 4.5 Code style

Module-prefixed public functions (`PFM_ForceStop`, `GdsInputs_SetEnabled`), `stdint.h` types, `volatile` on anything shared with an
interrupt, short non-blocking interrupt handlers, no heap. A bugfix comment states the symptom, the root cause and why the fix works.
`boot_jump.c` and `scpi_parser.c` show the expected standard.

## 4.6 Changelog entries

`firmware/<product>/docs/changelog.txt`, newest first. Header line: date, a short title, and a **status tag in brackets** that says
exactly what was verified, for example:

```
2026-11-16  -  GDS:POLarity: flip the fault polarity of all twelve gate-driver status inputs at runtime  [host-tested, builds clean; NOT flashed]
```

Body: the request, what changed and where, how it was tested, what was not tested. Update the tag when the item is later flashed or
verified on hardware. Never rewrite history entries, except to correct a factual error (say so).

## 4.7 Memory footprint

After any change that can move flash or RAM use, from the product folder:

```bash
python3 python/memory_report.py history   # appends a row for HEAD to docs/reports/memory_history.csv
python3 python/memory_report.py report    # regenerates docs/reports/memory_report.html
```

HBRIDGE image on 2026-11-16: 113,472 bytes flash, 121,756 bytes RAM (of 128 KB; free RAM is small, so large buffers are streamed).

## 4.8 Commits

Pattern used throughout the history:

1. `HBRIDGE: <what changed>` (or `XREX:`, or `docs:`): source, tests, docs, changelog.
2. `Rebuild the HBRIDGE image from the committed tree; record flash/RAM footprint ...`: the `build/` images and the memory report.

The images in `build/` are tracked, so the image matching every commit is in the history. Commit only your own files: other work may be
uncommitted in the tree at the same time (as on 2026-11-16: the operator UI and some host tools).

## 4.9 Documents to keep in step with the code

| When you change... | Update |
|---|---|
| a command | `docs/command_reference.md` (product), `docs/labview_commands.md` (top level) if a front end would use it |
| a saved setting | `docs/saved_profiles.md` (HBRIDGE) and the profile section of the command reference |
| an operator procedure | `docs/operators_guide.md`, the SOP (`docs/sop/*.tex`, rebuild the PDF with `pdflatex` twice and look at it) |
| a board, address or hub | `docs/bench_devices.md`, `host/devices.csv`, `host/config/*.yaml` |
| anything in this handoff | the file's Snapshot line |

Generated LaTeX reports go in their own folder under `docs/reports/<report-name>/`.
