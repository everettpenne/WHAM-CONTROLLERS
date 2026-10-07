#!/usr/bin/env python3
"""wham_build.py -- one-command build for WHAM-HBRIDGE-PFMG474: regenerates
build/generated/git_version.h, checks that Debug/'s makefiles and .cproject
match the source tree, checks the source layout rules (AGENTS.md, "Source
layout"), runs the off-target unit tests (tests/), runs `make` in Debug/,
regenerates Debug/WHAM-HBRIDGE-PFMG474.bin, and publishes the images to
build/:

    build/WHAM-HBRIDGE-PFMG474.{elf,bin,map}

build/ (not Debug/) is the tracked record of what was built and what the
flashing tools read; Debug/ is CubeIDE's working folder, whose copies are
ignored by git.

This project's .cproject does NOT enable CubeIDE's "Convert to binary file"
post-build step, so a bare `make` never produces the .bin -- flashing a stale
.bin "succeeds" while silently not containing your latest changes. This
script always regenerates it.

Usage:
  python3 python/wham_build.py                # build, test, publish
  python3 python/wham_build.py --clean        # make clean first
  python3 python/wham_build.py --skip-tests   # skip the host unit tests

Then flash with:
  python3 python/wham_serial_flash.py
"""
import argparse
import os
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)  # this script lives in python/
DEBUG_DIR = os.path.join(PROJECT_DIR, "Debug")
BUILD_DIR = os.path.join(PROJECT_DIR, "build")   # published, tracked images (see header)
STEM = "WHAM-HBRIDGE-PFMG474"
ELF_NAME = STEM + ".elf"
BIN_NAME = STEM + ".bin"


def run(cmd, cwd=None, description=""):
    print(f"\n--- {description or ' '.join(cmd)} ---")
    result = subprocess.run(cmd, cwd=cwd)
    if result.returncode != 0:
        sys.exit(f"error: {description or cmd[0]} failed (exit {result.returncode})")


def main():
    ap = argparse.ArgumentParser(description="One-command build for WHAM-HBRIDGE-PFMG474")
    ap.add_argument("--no-git-version", action="store_true",
                    help="skip regenerating build/generated/git_version.h")
    ap.add_argument("--clean", action="store_true", help="`make clean` before building")
    ap.add_argument("--skip-tests", action="store_true",
                    help="don't run the off-target unit tests (tests/, needs a host C compiler)")
    ap.add_argument("--jobs", "-j", type=int, default=4, help="make -j parallelism (default 4)")
    ap.add_argument("--objcopy", default="arm-none-eabi-objcopy",
                    help="objcopy binary (default: arm-none-eabi-objcopy, must be on PATH)")
    args = ap.parse_args()

    if not os.path.isdir(DEBUG_DIR):
        sys.exit(f"error: {DEBUG_DIR} not found -- run this from a normal checkout "
                 "(this script lives in python/, one level under the project root)")

    if not args.no_git_version:
        run([sys.executable, os.path.join(SCRIPT_DIR, "gen_git_version.py")],
            description="regenerating build/generated/git_version.h")
    else:
        print("\n--- skipping git_version.h regeneration (--no-git-version) ---")

    # The source tree and the build must agree before make runs: a file
    # added, moved or renamed without re-syncing would otherwise be silently
    # left out of the link (or fail with an undefined reference).
    run([sys.executable, os.path.join(SCRIPT_DIR, "sync_build_sources.py"), "--check"],
        description="checking Debug/ makefiles and .cproject match the source tree "
                    "(if this fails: python3 python/sync_build_sources.py)")

    # The source layout rules (AGENTS.md, "Source layout") -- a violation
    # fails the build, the same as a compile error would.
    run([sys.executable, os.path.join(SCRIPT_DIR, "check_layout.py")],
        description="checking the source layout (python/check_layout.py)")

    if not args.skip_tests:
        run(["make", "-C", os.path.join(PROJECT_DIR, "tests")],
            description="off-target unit tests (tests/)")

    if args.clean:
        run(["make", "clean"], cwd=DEBUG_DIR, description="make clean")

    run(["make", f"-j{args.jobs}", "all"], cwd=DEBUG_DIR, description="make all")

    elf_path = os.path.join(DEBUG_DIR, ELF_NAME)
    bin_path = os.path.join(DEBUG_DIR, BIN_NAME)
    if not os.path.isfile(elf_path):
        sys.exit(f"error: {elf_path} wasn't produced -- build must have failed silently")
    run([args.objcopy, "-O", "binary", elf_path, bin_path],
        description=f"objcopy -> {BIN_NAME}")

    size = os.path.getsize(bin_path)

    # Publish to build/. build/ is the tracked record of what was built;
    # Debug/ is CubeIDE's working folder.
    os.makedirs(BUILD_DIR, exist_ok=True)
    for ext in ("elf", "bin", "map"):
        shutil.copyfile(os.path.join(DEBUG_DIR, f"{STEM}.{ext}"),
                        os.path.join(BUILD_DIR, f"{STEM}.{ext}"))
    print(f"\n--- published build/{STEM}.elf/.bin/.map ---")
    print(f"\n[done] {bin_path} ({size} bytes). Flash with:\n"
          f"       python3 python/wham_serial_flash.py")


if __name__ == "__main__":
    main()
