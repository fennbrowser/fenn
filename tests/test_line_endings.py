"""Committed convention changes must fail CI even when the worktree is clean."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("line_endings", Path(__file__).resolve().parents[1] / "scripts/Check-LineEndings.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class LineEndingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="line endings ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git("init")
        self.git("config", "core.autocrlf", "false")
        self.file = self.root / "sample file.txt"
        self.file.write_bytes(b"original\n")
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), *args], check=True,
                              capture_output=True, text=True).stdout

    def commit(self):
        self.git("add", ".")
        self.git("-c", "user.name=Line Ending Test", "-c", "user.email=test@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-m", "fixture")

    def check(self, *args):
        with patch.object(checker, "ROOT", self.root), patch("sys.argv", ["checker", *args]), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return checker.main()

    def test_clean_worktree_with_committed_drift_fails_against_base(self):
        self.file.write_bytes(b"changed\r\n")
        self.commit()
        self.assertEqual(self.check(), 0)
        self.assertEqual(self.check("--base", self.base), 1)

    def test_content_change_preserving_convention_passes(self):
        self.file.write_bytes(b"changed\n")
        self.commit()
        self.assertEqual(self.check("--base", self.base), 0)

    def test_new_file_can_establish_its_convention(self):
        (self.root / "new.txt").write_bytes(b"new\r\n")
        self.commit()
        self.assertEqual(self.check("--base", self.base), 0)

    def test_unknown_base_fails_without_modifying_files(self):
        before = self.file.read_bytes()
        self.assertEqual(self.check("--base", "missing-revision"), 1)
        self.assertEqual(self.file.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
