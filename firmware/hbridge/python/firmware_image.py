"""firmware_image.py -- which source a firmware image was built from, and the check every flashing script makes before it
writes an image to a board: only an image built from COMMITTED code may be flashed.

SHARED: byte-identical in firmware/xrex/python and firmware/hbridge/python (python/check_shared.py compares them).

The build record
  python/wham_build.py writes build/<image>.build.json beside every image it publishes:
      {"image": "WHAM-HBRIDGE-PFMG474.bin", "sha256": ..., "commit": <full hash>, "dirty": false, "dirty_files": [],
       "git_version_regenerated": true, "built": "2026-11-19T14:02:11"}
  "commit" and "dirty" are what the image itself reports (*IDN? and !BOOT show "<commit>[-dirty]"): they are read from the
  build/generated/git_version.h that was compiled in, so the record cannot claim something the image does not.

What counts as uncommitted
  A change, or an untracked file, in this product's folder that can end up in the image. Changes to build outputs and
  to files that are never compiled in do not count (IMAGE_EXCLUDED: build/, .settings/ (the IDE's own state), docs/,
  python/, tests/, logs/, and Markdown files anywhere); everything else does: src/, Core/, Drivers/, the Debug/ makefiles, the linker script, the
  .ioc. gen_git_version.py uses the same rule for the "-dirty" flag.

The check (check_image), before anything is sent to a board. It refuses:
  * an image with no build record (not built by wham_build.py, or built before records existed): rebuild it;
  * an image whose bytes are not the ones the record describes (changed or replaced after the build);
  * an image built with --no-git-version (it may report the wrong commit);
  * an image built from uncommitted code ("dirty"): commit, rebuild, then flash;
  * an image built from a commit this repository does not have.
  --allow-uncommitted (on each flashing script) flashes it anyway, with a warning: for bench work on a board that is not in
  service. The board then reports "-dirty" in *IDN? and !BOOT.

Standard library only.
"""
import datetime
import hashlib
import json
import os
import re
import subprocess

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECORD_SUFFIX = ".build.json"
IMAGE_EXCLUDED = ("build", ".settings", "docs", "python", "tests", "logs")
GIT_VERSION_H = os.path.join(PROJECT_DIR, "build", "generated", "git_version.h")


class Refused(Exception):
    """The image may not be flashed; the message says why and what to do."""


def git(args, cwd=PROJECT_DIR):
    """stdout of a git command, or None when git or the repository is not there or the command fails."""
    try:
        r = subprocess.run(["git", "-C", cwd] + list(args), capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout.rstrip("\n") if r.returncode == 0 else None


def uncommitted_files(project_dir=PROJECT_DIR):
    """The changed or untracked files of this product that can go into the image ([] when clean), or None when git cannot
    tell (not a repository, no git)."""
    spec = ["."] + [f":(exclude){p}" for p in IMAGE_EXCLUDED] + [":(exclude,glob)**/*.md"]
    out = git(["status", "--porcelain", "--untracked-files=all", "--no-renames", "--"] + spec, cwd=project_dir)
    if out is None:
        return None
    return [line[3:].strip() for line in out.splitlines() if line.strip()]


def record_path(bin_path):
    return bin_path + RECORD_SUFFIX


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def read_git_version_h(path=GIT_VERSION_H):
    """(short commit, dirty) as compiled into the image, or (None, None)."""
    try:
        text = open(path).read()
    except OSError:
        return None, None
    c = re.search(r'#define\s+FW_GIT_COMMIT\s+"([^"]*)"', text)
    d = re.search(r"#define\s+FW_GIT_DIRTY\s+(\d+)", text)
    return (c.group(1) if c else None), (int(d.group(1)) != 0 if d else None)


def write_record(bin_path, regenerated=True, project_dir=PROJECT_DIR, git_version_h=GIT_VERSION_H):
    """Called by wham_build.py right after it publishes an image."""
    short, dirty = read_git_version_h(git_version_h)
    full = git(["rev-parse", "HEAD"], cwd=project_dir)
    if not short or short == "unknown" or not full or not full.startswith(short):
        full = short or "unknown"                  # HEAD moved, or no git: keep only what the image itself says
    rec = {"image": os.path.basename(bin_path), "sha256": sha256(bin_path), "commit": full,
           "dirty": bool(dirty) if dirty is not None else True,
           "dirty_files": (uncommitted_files(project_dir) or []) if dirty else [],
           "git_version_regenerated": bool(regenerated),
           "built": datetime.datetime.now().isoformat(timespec="seconds")}
    with open(record_path(bin_path), "w") as f:
        json.dump(rec, f, indent=1)
        f.write("\n")
    return rec


def describe(rec):
    return f"commit {rec.get('commit', '?')[:8]}{'-dirty' if rec.get('dirty') else ''}, built {rec.get('built', '?')}"


def check_image(bin_path, allow_uncommitted=False, out=print, project_dir=PROJECT_DIR):
    """Raise Refused unless the image was built by wham_build.py from committed code (see the module header). With
    allow_uncommitted, print why it would have been refused and return instead. Returns the build record (or {})."""
    problems, rec = [], {}
    rpath = record_path(bin_path)
    if not os.path.isfile(rpath):
        problems.append(f"{os.path.basename(bin_path)} has no build record ({os.path.basename(rpath)}): it was not built by "
                        "python/wham_build.py, or was built before build records existed. Rebuild it with wham_build.py.")
    else:
        try:
            with open(rpath) as f:
                rec = json.load(f)
        except (OSError, ValueError) as e:
            problems.append(f"its build record cannot be read ({e}). Rebuild it with python/wham_build.py.")
            rec = {}
    if rec:
        if rec.get("sha256") != sha256(bin_path):
            problems.append("the image is not the one its build record describes (it was changed or replaced after the "
                            "build). Rebuild it with python/wham_build.py.")
        if not rec.get("git_version_regenerated", False) or rec.get("commit", "unknown") in ("", "unknown"):
            problems.append("it was built without regenerating git_version.h (--no-git-version, or no git), so it may "
                            "report the wrong commit. Rebuild it with python/wham_build.py.")
        if rec.get("dirty", True):
            files = rec.get("dirty_files") or []
            problems.append("it was built from UNCOMMITTED code" + (": " + ", ".join(files[:8]) + (" ..." if len(files) > 8 else "")
                            if files else "") + ". Commit the changes, rebuild with python/wham_build.py, then flash.")
        commit = rec.get("commit", "")
        if commit and commit != "unknown" and git(["rev-parse", "--git-dir"], cwd=project_dir) is not None:
            if git(["cat-file", "-e", commit + "^{commit}"], cwd=project_dir) is None:
                problems.append(f"it was built from commit {commit[:8]}, which this repository does not have (pull, or "
                                "rebuild from this checkout).")
    if not problems:
        out(f"image:     built from committed code, {describe(rec)}")
        return rec
    text = "\n".join(f"  - {p}" for p in problems)
    if allow_uncommitted:
        out(f"WARNING: flashing anyway (--allow-uncommitted). This image would otherwise be refused:\n{text}\n"
            "         The board will report '-dirty' and no commit holds exactly this code. Bench boards only.")
        return rec
    raise Refused(f"refusing to flash {bin_path}:\n{text}\n"
                  "(--allow-uncommitted flashes it anyway, for bench work on a board that is not in service)")
