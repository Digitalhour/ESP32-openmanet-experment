#!/usr/bin/env python3
"""Regression test for patch_vendored_halow.py - review finding F16
(design/PROJECT_REVIEW_2026-09-10.md), "no maintained automated firmware
regression suite was found." Formalizes a real bug found and manually
verified during that session's own work (see design/ROADMAP.md's F16 entry)
so it stays caught rather than being a one-off check that was never saved.

Uses temp files, not the real managed_components/ tree: that directory is
gitignored and only exists after ESP-IDF's component manager has actually
fetched dependencies, which a lightweight host-tests job has no reason to
do. patch_file() takes its target path as a plain argument, so it can be
pointed at a fixture instead without any change to the script under test.

stdlib unittest only - no pytest, no extra dependency to install just to
run this.
"""
import importlib.util
import os
import sys
import tempfile
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

spec = importlib.util.spec_from_file_location(
    "patch_vendored_halow", os.path.join(REPO_ROOT, "patch_vendored_halow.py")
)
patch_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch_mod)


class TestPatchFile(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp()
        os.close(fd)

    def tearDown(self):
        os.unlink(self.path)

    def write(self, content):
        with open(self.path, "w") as f:
            f.write(content)

    def read(self):
        with open(self.path) as f:
            return f.read()

    def test_fully_unpatched_applies_cleanly(self):
        self.write(patch_mod.SOURCE_OLD_TRANSMIT + "\n" + patch_mod.SOURCE_OLD_START)
        changed = patch_mod.patch_file(
            self.path,
            [
                (patch_mod.SOURCE_OLD_TRANSMIT, patch_mod.SOURCE_NEW_TRANSMIT),
                (patch_mod.SOURCE_OLD_START, patch_mod.SOURCE_NEW_START),
            ],
        )
        self.assertTrue(changed)
        content = self.read()
        self.assertIn(patch_mod.SOURCE_NEW_TRANSMIT, content)
        self.assertIn(patch_mod.SOURCE_NEW_START, content)

    def test_fully_patched_is_a_noop(self):
        self.write(patch_mod.SOURCE_NEW_TRANSMIT + "\n" + patch_mod.SOURCE_NEW_START)
        before = self.read()
        changed = patch_mod.patch_file(
            self.path,
            [
                (patch_mod.SOURCE_OLD_TRANSMIT, patch_mod.SOURCE_NEW_TRANSMIT),
                (patch_mod.SOURCE_OLD_START, patch_mod.SOURCE_NEW_START),
            ],
        )
        self.assertFalse(changed)
        self.assertEqual(before, self.read())

    def test_partial_patch_is_detected_and_completed(self):
        """The actual bug this test exists to catch (design/ROADMAP.md's F16
        entry): one replacement already applied, the other still in its
        pre-patch form - the exact state an interrupted prior run or a
        hand-edit could leave a file in. The old, single-whole-file-marker
        implementation would have seen SOURCE_NEW_TRANSMIT's marker text,
        concluded the entire file was already patched, and returned False
        without ever looking at (or fixing) mmhalow_wifi_start()."""
        self.write(patch_mod.SOURCE_NEW_TRANSMIT + "\n" + patch_mod.SOURCE_OLD_START)
        changed = patch_mod.patch_file(
            self.path,
            [
                (patch_mod.SOURCE_OLD_TRANSMIT, patch_mod.SOURCE_NEW_TRANSMIT),
                (patch_mod.SOURCE_OLD_START, patch_mod.SOURCE_NEW_START),
            ],
        )
        self.assertTrue(changed, "a partially-patched file must be reported as changed")
        content = self.read()
        self.assertIn(patch_mod.SOURCE_NEW_TRANSMIT, content)
        self.assertIn(
            patch_mod.SOURCE_NEW_START,
            content,
            "mmhalow_wifi_start() must be completed, not silently left in its "
            "pre-patch form because the OTHER replacement's marker was already present",
        )

    def test_unrecognized_state_fails_loudly(self):
        """Neither the pre- nor post-patch form - a changed upstream hunk or
        a hand-edit gone wrong. Must not silently accept or guess."""
        self.write(patch_mod.SOURCE_NEW_TRANSMIT + "\nsomething unrecognizable")
        with self.assertRaises(SystemExit) as ctx:
            patch_mod.patch_file(
                self.path,
                [
                    (patch_mod.SOURCE_OLD_TRANSMIT, patch_mod.SOURCE_NEW_TRANSMIT),
                    (patch_mod.SOURCE_OLD_START, patch_mod.SOURCE_NEW_START),
                ],
            )
        self.assertEqual(ctx.exception.code, 1)

    def test_header_replacement_applies_and_is_idempotent(self):
        """The single-replacement (header) path, separately from mmhalow.c's
        two-replacement path above - both call sites in main() go through
        the same function, worth covering both shapes."""
        self.write(patch_mod.HEADER_OLD)
        changed = patch_mod.patch_file(self.path, [(patch_mod.HEADER_OLD, patch_mod.HEADER_NEW)])
        self.assertTrue(changed)
        self.assertIn(patch_mod.HEADER_NEW, self.read())

        changed_again = patch_mod.patch_file(self.path, [(patch_mod.HEADER_OLD, patch_mod.HEADER_NEW)])
        self.assertFalse(changed_again, "reapplying to an already-patched file must be a no-op")


if __name__ == "__main__":
    unittest.main()
