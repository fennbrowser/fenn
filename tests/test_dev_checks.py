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

spec = importlib.util.spec_from_file_location("python_fast", Path(__file__).resolve().parents[1] / "scripts/Run-PythonFastTests.py")
python_fast = importlib.util.module_from_spec(spec)
spec.loader.exec_module(python_fast)


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
            with patch.dict(dev.os.environ, {"FENN_SHARED_CACHE": str(shared)}):
                env, resolved = dev.environment(root)
            self.assertEqual(resolved, shared)
            self.assertEqual(env["CARGO_HOME"], str(shared / "cargo"))
            self.assertFalse(shared.exists())


class PythonFastTests(unittest.TestCase):
    def test_coverage_rejects_missing_duplicate_and_failed_reports(self):
        expected = {"module": ["one", "two"]}
        outcomes = [["one", [["passed"]]], ["two", [["skipped", "fixture"]]]]
        self.assertTrue(python_fast.verify(expected, {"module": {"passed": True, "outcomes": outcomes}}))
        for reported, passed in ((outcomes[:1], True), (outcomes + outcomes[:1], True),
                                 (outcomes, False)):
            with self.subTest(reported=reported, passed=passed), contextlib.redirect_stdout(io.StringIO()):
                self.assertFalse(python_fast.verify(expected, {"module": {"passed": passed, "outcomes": reported}}))

    def test_worker_exit_without_results_fails(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            worker = Path(directory) / "exit.py"
            worker.write_text("import os\nos._exit(42)\n", encoding="utf-8", newline="\n")
            reports = {}
            with patch.object(python_fast, "__file__", str(worker)):
                self.assertFalse(python_fast.run_workers(directory, [("module",)], 1, reports))
            self.assertEqual(reports, {})

    def test_worker_deadline_terminates_the_process(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()) as output:
            worker = Path(directory) / "sleep.py"
            worker.write_text("import time\ntime.sleep(30)\n", encoding="utf-8", newline="\n")
            with patch.object(python_fast, "__file__", str(worker)), patch.object(python_fast, "WORKER_TIMEOUT", 0):
                self.assertFalse(python_fast.run_workers(directory, [("module",)], 1, {}))
            self.assertIn("Worker timed out", output.getvalue())

    def test_interrupt_terminates_active_worker(self):
        with tempfile.TemporaryDirectory() as directory:
            worker = Path(directory) / "sleep.py"
            worker.write_text("import time\ntime.sleep(30)\n", encoding="utf-8", newline="\n")
            with patch.object(python_fast, "__file__", str(worker)), \
                    patch.object(python_fast.time, "sleep", side_effect=KeyboardInterrupt), \
                    patch.object(python_fast, "terminate", wraps=python_fast.terminate) as terminate:
                with self.assertRaises(KeyboardInterrupt):
                    python_fast.run_workers(directory, [("module",)], 1, {})
                terminate.assert_called_once()
                self.assertIsNotNone(terminate.call_args.args[0].returncode)

    def test_class_setup_skip_accounts_for_every_case(self):
        class Skipped(unittest.TestCase):
            @classmethod
            def setUpClass(cls):
                raise unittest.SkipTest("fixture")

            def test_first(self):
                self.fail()

            def test_second(self):
                self.fail()

        suite = unittest.defaultTestLoader.loadTestsFromTestCase(Skipped)
        tests = list(suite)
        result = unittest.TextTestRunner(stream=io.StringIO(), resultclass=python_fast.Outcomes).run(suite)
        self.assertTrue(result.wasSuccessful())
        self.assertEqual(result.outcomes(__name__, tests),
                         [[test.id(), [["skipped", "fixture"]]] for test in tests])


if __name__ == "__main__":
    unittest.main()
