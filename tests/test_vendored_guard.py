#!/usr/bin/env python3
"""Exercise vendored guard against isolated Git fixtures, without network access."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GUARD = Path(__file__).resolve().parents[1] / "tools" / "vendored_guard.py"
SPEC = importlib.util.spec_from_file_location("vendored_guard", GUARD)
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)


class VendoredGuardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="agent-c-vendored-guard-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = os.environ.copy()
        for key in (
            "ALLOW_VENDORED_CHANGES", "VENDORED_GUARD_BASE",
            "VENDORED_GUARD_REQUIRE_BASE", "VENDORED_GUARD_QUIET",
            "GITHUB_BASE_REF", "GIT_BASE_REF", "GIT_DIR", "GIT_WORK_TREE",
        ):
            self.env.pop(key, None)
        self.env["GIT_CONFIG_GLOBAL"] = os.devnull
        self.env["GIT_CONFIG_NOSYSTEM"] = "1"
        self.git("init", "--quiet")
        (self.root / "ref").mkdir()
        (self.root / "ref" / "vendor.txt").write_text("baseline\n")
        self.git("add", "ref/vendor.txt")
        self.git("commit", "--quiet", "-m", "fixture baseline")
        self.base = self.git("rev-parse", "HEAD").strip()
        self.git("update-ref", "refs/remotes/origin/master", self.base)

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-C", str(self.root),
             "-c", "user.name=Guard test",
             "-c", "user.email=guard-test@example.invalid",
             "-c", f"core.hooksPath={os.devnull}", *args],
            env=self.env, stderr=subprocess.PIPE, text=True,
        )

    def run_guard(self, *args, env=None):
        return subprocess.run(
            [sys.executable, str(GUARD), "--root", str(self.root),
             "--require-base", *args],
            env=env or self.env, capture_output=True, text=True, check=False,
        )

    def change_vendor(self, commit=False):
        (self.root / "ref" / "vendor.txt").write_text("changed\n")
        if commit:
            self.git("add", "ref/vendor.txt")
            self.git("commit", "--quiet", "-m", "fixture vendor change")

    def test_remote_ref_and_commit_ids_resolve(self):
        for ref in ("origin/master", "refs/remotes/origin/master", self.base):
            with self.subTest(ref=ref):
                self.assertEqual(guard.normalize_base_ref(self.root, ref), self.base)

    def test_default_remote_base_is_found(self):
        result = self.run_guard()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_committed_vendored_change_fails(self):
        self.change_vendor(commit=True)
        result = self.run_guard()
        self.assertEqual(result.returncode, 2)
        self.assertIn("ref/vendor.txt", result.stdout)

    def test_explicit_sha_compares_pre_push_commit(self):
        self.change_vendor(commit=True)
        # A checkout of the just-pushed branch has origin/master at HEAD.
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")
        result = self.run_guard("--base", self.base)
        self.assertEqual(result.returncode, 2)
        self.assertIn("ref/vendor.txt", result.stdout)

    def test_environment_sha_compares_pre_push_commit(self):
        self.change_vendor(commit=True)
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")
        env = dict(self.env, VENDORED_GUARD_BASE=self.base)
        self.assertEqual(self.run_guard(env=env).returncode, 2)

    def test_invalid_explicit_base_fails_closed(self):
        result = self.run_guard("--base", "missing-before-commit")
        self.assertEqual(result.returncode, 2)
        self.assertIn("Invalid explicit base", result.stderr)

    def test_missing_required_base_fails(self):
        self.git("update-ref", "-d", "refs/remotes/origin/master")
        result = self.run_guard()
        self.assertEqual(result.returncode, 2)
        self.assertIn("No base ref found", result.stderr)

    def test_staged_vendored_change_fails(self):
        self.change_vendor()
        self.git("add", "ref/vendor.txt")
        self.assertEqual(self.run_guard().returncode, 2)

    def test_unstaged_vendored_change_fails(self):
        self.change_vendor()
        self.assertEqual(self.run_guard().returncode, 2)

    def test_option_like_base_is_not_executed(self):
        self.assertIsNone(guard.normalize_base_ref(self.root, "--help"))


if __name__ == "__main__":
    unittest.main()
