"""Tests of firmware/*/python/firmware_image.py: the build record and the committed-code check every flashing script makes.
A temporary git repository stands in for a product folder; nothing is flashed. Run: python3 -m unittest tests.test_firmware_image"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "firmware", "xrex", "python"))
import firmware_image as fi


def git(cwd, *args):
    r = subprocess.run(["git", "-C", cwd] + list(args), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout.strip()


class FirmwareImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.proj = os.path.join(self.tmp.name, "firmware", "hbridge")
        for path, text in {"src/app/app.c": "int x;\n", "Debug/makefile": "all:\n", "docs/notes.md": "x\n",
                           "python/tool.py": "x = 1\n", "README.md": "x\n", ".settings/ide.xml": "<a/>\n",
                           "tests/t.c": "int t;\n", "build/WHAM.bin": "old\n"}.items():
            self.write(path, text)
        git(self.tmp.name, "init", "--quiet")
        git(self.tmp.name, "config", "user.email", "t@example.invalid")
        git(self.tmp.name, "config", "user.name", "t")
        git(self.tmp.name, "add", "-A")
        git(self.tmp.name, "commit", "--quiet", "-m", "v1")
        self.head = git(self.tmp.name, "rev-parse", "HEAD")
        self.bin = os.path.join(self.proj, "build", "WHAM.bin")
        self.hdr = os.path.join(self.proj, "build", "generated", "git_version.h")
        self.out = []

    def write(self, path, text):
        full = os.path.join(self.proj, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)

    def build(self, dirty=None, regenerated=True, commit=None):
        """What wham_build.py does: gen_git_version (dirty from uncommitted_files), the image, the record."""
        if dirty is None:
            dirty = bool(fi.uncommitted_files(self.proj))
        self.write("build/generated/git_version.h",
                   f'#define FW_GIT_COMMIT "{(commit or self.head)[:8]}"\n#define FW_GIT_DIRTY   {int(dirty)}U\n')
        self.write("build/WHAM.bin", "image bytes " + self.head + str(dirty))
        return fi.write_record(self.bin, regenerated=regenerated, project_dir=self.proj, git_version_h=self.hdr)

    def check(self, **kw):
        return fi.check_image(self.bin, out=self.out.append, project_dir=self.proj, **kw)

    def test_only_what_goes_into_the_image_counts_as_uncommitted(self):
        self.assertEqual(fi.uncommitted_files(self.proj), [])
        for path in ("docs/notes.md", "python/tool.py", "README.md", ".settings/ide.xml", "tests/t.c", "build/WHAM.bin",
                     "build/generated/git_version.h", "docs/new.md", "logs/run.log"):
            self.write(path, "changed\n")
        self.assertEqual(fi.uncommitted_files(self.proj), [], "outputs, docs, tools, tests and IDE state are not in the image")
        self.write("src/app/app.c", "int x = 2;\n")
        self.write("src/app/new.c", "int y;\n")
        self.write("Debug/makefile", "all: x\n")
        self.assertEqual(sorted(fi.uncommitted_files(self.proj)), ["firmware/hbridge/Debug/makefile", "firmware/hbridge/src/app/app.c", "firmware/hbridge/src/app/new.c"])

    def test_a_clean_build_passes(self):
        rec = self.build()
        self.assertEqual((rec["commit"], rec["dirty"], rec["dirty_files"]), (self.head, False, []))
        self.assertEqual(rec["sha256"], fi.sha256(self.bin))
        self.check()
        self.assertIn("built from committed code", self.out[0])
        with open(fi.record_path(self.bin)) as f:
            self.assertEqual(json.load(f)["image"], "WHAM.bin")

    def test_an_image_built_from_uncommitted_code_is_refused_and_names_the_files(self):
        self.write("src/app/app.c", "int x = 3;\n")
        rec = self.build()
        self.assertTrue(rec["dirty"])
        self.assertEqual(rec["dirty_files"], ["firmware/hbridge/src/app/app.c"])
        with self.assertRaises(fi.Refused) as cm:
            self.check()
        self.assertIn("UNCOMMITTED", str(cm.exception))
        self.assertIn("src/app/app.c", str(cm.exception))
        self.assertIn("--allow-uncommitted", str(cm.exception))

    def test_committing_afterwards_is_not_enough_the_image_must_be_rebuilt(self):
        self.write("src/app/app.c", "int x = 3;\n")
        self.build()
        git(self.tmp.name, "commit", "--quiet", "-am", "v2")
        with self.assertRaises(fi.Refused):
            self.check()                                  # the image still holds code no commit had when it was built

    def test_allow_uncommitted_flashes_with_a_warning(self):
        self.write("src/app/app.c", "int x = 3;\n")
        self.build()
        rec = self.check(allow_uncommitted=True)
        self.assertTrue(rec["dirty"])
        self.assertIn("WARNING: flashing anyway", self.out[0])

    def test_no_record_is_refused(self):
        with self.assertRaises(fi.Refused) as cm:
            self.check()
        self.assertIn("no build record", str(cm.exception))

    def test_an_image_changed_after_the_build_is_refused(self):
        self.build()
        with open(self.bin, "a") as f:
            f.write("patched")
        with self.assertRaises(fi.Refused) as cm:
            self.check()
        self.assertIn("not the one its build record describes", str(cm.exception))

    def test_a_build_without_regenerating_the_version_is_refused(self):
        self.build(regenerated=False)
        with self.assertRaises(fi.Refused) as cm:
            self.check()
        self.assertIn("--no-git-version", str(cm.exception))

    def test_a_commit_this_repository_does_not_have_is_refused(self):
        self.build()
        with open(fi.record_path(self.bin)) as f:
            rec = json.load(f)
        rec["commit"] = "0123456789abcdef0123456789abcdef01234567"
        with open(fi.record_path(self.bin), "w") as f:
            json.dump(rec, f)
        with self.assertRaises(fi.Refused) as cm:
            self.check()
        self.assertIn("does not have", str(cm.exception))

    def test_the_header_is_what_the_record_reports(self):
        self.write("src/app/app.c", "int x = 4;\n")
        rec = self.build(dirty=False)                     # e.g. a header written before the edit: the image says clean
        self.assertFalse(rec["dirty"], "the record states what the image reports, from git_version.h")
        self.assertEqual(fi.read_git_version_h(self.hdr), (self.head[:8], False))

    def test_both_products_carry_the_same_module(self):
        a = open(os.path.join(ROOT, "firmware", "xrex", "python", "firmware_image.py")).read()
        b = open(os.path.join(ROOT, "firmware", "hbridge", "python", "firmware_image.py")).read()
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
