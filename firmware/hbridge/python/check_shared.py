#!/usr/bin/env python3
"""check_shared.py -- are the files WHAM-XREX-PFMG474 and WHAM-HBRIDGE-PFMG474 SHARE still identical?

The two controllers speak the same SCPI-style protocol and keep saved profiles in the same external flash chip
on the same board, so the protocol layer (parser, error codes, argument and reply helpers), the flash driver and
the profile store are one piece of code that lives in both repos.
Nothing links them, so this script is the guard: it compares each shared file (or shared block of a file)
with the sibling repo's copy and reports any difference. The identical script is in both repos.

    python3 python/check_shared.py                 # compares with the sibling next to this repo
    python3 python/check_shared.py --sibling PATH  # or with another checkout

Exit status 0 when everything matches (or the sibling is not there), 1 on a difference. Not part of
wham_build.py on purpose: while one repo is being changed the two legitimately differ until the change is
carried over; run this before finishing such a change. Fixing a difference means copying the NEWER file to
the other repo and running its host tests (`make -C tests`).

What differs per product is configuration and meaning, not these files: ctrlr_config.h (PROFILE_STORE_BASE,
PROFILE_COPY_SECTORS, PROFILE_SLOT_COUNT, PROFILE_MAGIC_BYTES), settings_codec.h (the field table),
settings.c, command_table.c and the cmd_*.c handlers, tests/known_short_collisions.inc. See
docs/saved_profiles_shared.md and docs/scpi_conventions.md.
"""
import argparse
import hashlib
import re
import sys
from pathlib import Path

# whole files that must be byte-identical
WHOLE = [
    "src/middleware/scpi/scpi_parser.c",
    "src/middleware/scpi/scpi_parser.h",
    "src/middleware/scpi/scpi_err.h",
    "src/middleware/scpi/scpi_util.c",
    "src/middleware/scpi/scpi_util.h",
    "tests/test_scpi_parser.c",
    "tests/test_scpi_util.c",
    "tests/test_command_table.c",
    "tests/fake_uart.c",
    "tests/fake_uart.h",
    "docs/scpi_conventions.md",
    "docs/saved_profiles_shared.md",
    "src/drivers/led_indicators.h",
    "src/bsp/stm32g4/led_indicators.c",
    "src/drivers/uart.h",
    "src/bsp/stm32g4/uart.c",
    "src/drivers/device_id.h",
    "src/bsp/stm32g4/device_id.c",
    "src/app/settings/device_identity.h",
    "src/app/settings/device_identity.c",
    "tests/test_device_identity.c",
    "src/drivers/nvstore.h",
    "src/bsp/stm32g4/nvstore.c",
    "src/bsp/stm32g4/qspi_test.c",
    "src/bsp/stm32g4/qspi_test_hw.h",
    "src/drivers/qspi_test.h",
    "src/app/settings/profile_store.h",
    "src/app/settings/profile_store.c",
    "tests/test_profile_store.c",
    "tests/fake_nvstore.c",
    "tests/fake_nvstore.h",
]

# blocks of a file that must be identical, as (file, start marker, end marker); none today (the float formatter that
# was one now lives whole in scpi_util.c)
BLOCKS = []


def digest(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()[:12]


def block(path: Path, start: str, end: str):
    text = path.read_text()
    a = text.find(start)
    b = text.find(end, a + 1) if a >= 0 else -1
    if a < 0 or b < 0:
        return None
    return text[a:b]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sibling", help="the other repo (default: the other product next to this one)")
    args = ap.parse_args()

    here = Path(__file__).resolve().parent.parent
    if args.sibling:
        other = Path(args.sibling).resolve()
    else:
        # the standalone repos sit side by side (WHAM-XREX-PFMG474, WHAM-HBRIDGE-PFMG474); in the WHAM-CONTROLLERS
        # repository they are firmware/xrex and firmware/hbridge
        pair = {"WHAM-XREX-PFMG474": "WHAM-HBRIDGE-PFMG474", "WHAM-HBRIDGE-PFMG474": "WHAM-XREX-PFMG474",
                "xrex": "hbridge", "hbridge": "xrex"}
        other = here.parent / pair.get(here.name, "WHAM-HBRIDGE-PFMG474")
    if not other.is_dir():
        print(f"check_shared: sibling repo not found ({other}); nothing to compare")
        return 0

    print(f"comparing {here.name} with {other.name}")
    bad = 0
    for rel in WHOLE:
        a, b = here / rel, other / rel
        if not a.exists() or not b.exists():
            print(f"  MISSING   {rel}  ({'here' if not a.exists() else other.name})")
            bad += 1
        elif a.read_bytes() != b.read_bytes():
            print(f"  DIFFERENT {rel}   {digest(a.read_text())} vs {digest(b.read_text())}")
            bad += 1
        else:
            print(f"  same      {rel}")
    for rel, start, end in BLOCKS:
        a, b = block(here / rel, start, end), block(other / rel, start, end)
        label = f"{rel} [{start.strip('/* ')[:28]}...]"
        if a is None or b is None:
            print(f"  MISSING   {label}  (marker not found in {'here' if a is None else other.name})")
            bad += 1
        elif a != b:
            print(f"  DIFFERENT {label}   {digest(a)} vs {digest(b)}")
            bad += 1
        else:
            print(f"  same      {label}")
    print("OK: shared code is identical" if bad == 0 else f"{bad} difference(s): carry the newer file over and rerun both repos' tests")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
