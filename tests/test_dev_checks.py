"""The verification runner must not turn missing or failed checks into success."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("dev_checks", Path(__file__).resolve().parents[1] / "scripts/DevChecks.py")
dev = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dev)


class DevChecksTests(unittest.TestCase):
    def test_missing_and_failing_checks_do_not_stop_independent_checks(self):
        checks = dev.Checks(Path.cwd(), {})
        responses = [subprocess.CompletedProcess([], 2, "failure\n"),
                     subprocess.CompletedProcess([], 0, "success\n")]
        with contextlib.redirect_stdout(io.StringIO()), patch.object(dev.subprocess, "run", side_effect=responses) as run:
            checks.run("missing", [None], missing="install tool")
            checks.run("failed", ["tool"])
            checks.run("independent", ["tool"])
            self.assertEqual(checks.finish(), 1)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(checks.results, [("missing", "BLOCKED"), ("failed", "FAIL"), ("independent", "PASS")])

    def test_successful_command_with_wrong_revision_fails(self):
        checks = dev.Checks(Path.cwd(), {})
        with contextlib.redirect_stdout(io.StringIO()), patch.object(dev.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "wrong\n")):
            checks.run("base", ["git"], expected="pinned")
            self.assertEqual(checks.finish(), 1)

    def test_source_requires_all_patch_targets_and_accepts_git_worktree_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            firefox = root / "upstream/firefox"
            firefox.mkdir(parents=True)
            (firefox / ".git").write_text("gitdir: elsewhere")
            (root / "patches").mkdir()
            (root / "patches/test.patch").write_text("+++ b/adapter.js\n")
            self.assertIn("adapter.js", dev.source_problem(root))
            (firefox / "adapter.js").write_text("// adapter")
            self.assertIsNone(dev.source_problem(root))

    def test_environment_does_not_create_missing_shared_cache(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            shared = root / "missing shared cache"
            with patch.dict(dev.os.environ, {"FIREFOX_WINUI_SHARED_CACHE": str(shared)}):
                env, resolved = dev.environment(root)
            self.assertEqual(resolved, shared)
            self.assertEqual(env["CARGO_HOME"], str(shared / "cargo"))
            self.assertFalse(shared.exists())


if __name__ == "__main__":
    unittest.main()
