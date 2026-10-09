"""Exercise source reconstruction using tiny real Git repositories, without network."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("bootstrap", Path(__file__).resolve().parents[1] / "scripts/Bootstrap-Firefox.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bootstrap test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        bootstrap.git(self.source, "init")
        bootstrap.git(self.source, "config", "core.autocrlf", "false")
        (self.source / "sample.txt").write_bytes(b"base\n")
        bootstrap.git(self.source, "add", ".")
        self.commit(self.source)
        self.base = bootstrap.git(self.source, "rev-parse", "HEAD")
        self.project = self.root / "project"
        (self.project / "configs").mkdir(parents=True)
        (self.project / "patches").mkdir()
        (self.project / "configs/toolchain-baseline.json").write_text(json.dumps({"firefox": {
            "commit": self.base, "repository": self.source.as_uri()}}))
        (self.project / "patches" / bootstrap.PATCHES[0]).write_bytes(
            b"diff --git a/sample.txt b/sample.txt\n--- a/sample.txt\n+++ b/sample.txt\n@@ -1 +1 @@\n-base\n+second\n")
        self.output = contextlib.redirect_stdout(io.StringIO())
        self.output.__enter__()
        self.addCleanup(self.output.__exit__, None, None, None)

    def commit(self, repo):
        bootstrap.git(repo, "-c", "user.name=Bootstrap Test", "-c", "user.email=test@example.invalid",
                      "-c", "commit.gpgsign=false", "commit", "-m", "fixture")

    def test_reference_clone_uses_objects_not_modified_worktree(self):
        (self.source / "sample.txt").write_bytes(b"uncommitted developer edit\n")
        status = bootstrap.git(self.source, "status", "--porcelain")
        report = bootstrap.prepare(self.project, self.source)
        target = self.project / "upstream/firefox"
        self.assertEqual((target / "sample.txt").read_bytes(), b"second\n")
        self.assertEqual(bootstrap.git(target, "rev-parse", "HEAD"), self.base)
        self.assertEqual(bootstrap.git(self.source, "status", "--porcelain"), status)
        self.assertFalse((target / ".git/objects/info/alternates").exists())
        self.assertEqual(report["status"], "passed")

    def test_borrowed_reference_objects_become_independent(self):
        borrowed = self.root / "borrowed"
        bootstrap.git(self.root, "clone", "--shared", self.source, borrowed)
        self.assertTrue((borrowed / ".git/objects/info/alternates").exists())
        bootstrap.prepare(self.project, borrowed)
        target = self.project / "upstream/firefox"
        self.assertFalse((target / ".git/objects/info/alternates").exists())
        self.assertEqual(bootstrap.git(target, "show", f"{self.base}:sample.txt"), "base")

    def test_configured_remote_transport(self):
        bootstrap.prepare(self.project)
        self.assertEqual((self.project / "upstream/firefox/sample.txt").read_bytes(), b"second\n")

    def test_existing_checkout_is_never_overwritten(self):
        target = self.project / "upstream/firefox"
        target.mkdir(parents=True)
        marker = target / "keep.txt"
        marker.write_bytes(b"precious")
        with self.assertRaisesRegex(RuntimeError, "Refusing existing"):
            bootstrap.prepare(self.project, self.source)
        self.assertEqual(marker.read_bytes(), b"precious")
        self.assertEqual(list(target.iterdir()), [marker])

    def test_patch_failure_retains_checkout_and_refuses_retry(self):
        (self.project / "patches" / bootstrap.PATCHES[0]).write_text("broken patch")
        with self.assertRaisesRegex(RuntimeError, "Partial checkout retained"):
            bootstrap.prepare(self.project, self.source)
        self.assertEqual((self.project / "upstream/firefox/sample.txt").read_bytes(), b"base\n")
        with self.assertRaisesRegex(RuntimeError, "Refusing existing"):
            bootstrap.prepare(self.project, self.source)

    def test_missing_reference_pin_does_not_create_target(self):
        config = self.project / "configs/toolchain-baseline.json"
        data = json.loads(config.read_text())
        data["firefox"]["commit"] = "0" * 40
        config.write_text(json.dumps(data))
        with self.assertRaises(RuntimeError):
            bootstrap.prepare(self.project, self.source)
        self.assertFalse((self.project / "upstream/firefox").exists())

    def test_verification_uses_committed_patches_and_isolated_outputs(self):
        (self.project / "scripts").mkdir()
        (self.project / "scripts/DevChecks.py").write_text(
            "import json, os\nfrom pathlib import Path\n"
            "Path('artifacts/test-env.json').write_text(json.dumps({k: os.environ[k] "
            "for k in ('CARGO_TARGET_DIR', 'TEMP')}))\n")
        bootstrap.git(self.project, "init")
        bootstrap.git(self.project, "add", ".")
        self.commit(self.project)
        # Verification must ignore a dirty patch in the invoking checkout.
        (self.project / "patches" / bootstrap.PATCHES[0]).write_text("uncommitted invalid patch")
        destination = self.root / "verification"
        report = bootstrap.verify(self.project, destination, self.source)
        self.assertEqual(report["status"], "passed")
        env = json.loads((destination / "artifacts/test-env.json").read_text())
        for path in env.values():
            self.assertTrue(Path(path).is_relative_to(destination))
        self.assertEqual((destination / "upstream/firefox/sample.txt").read_bytes(), b"second\n")
        with self.assertRaisesRegex(RuntimeError, "Refusing existing"):
            bootstrap.verify(self.project, destination, self.source)


    def test_failed_fast_checks_leave_failure_report_and_source(self):
        (self.project / "scripts").mkdir()
        (self.project / "scripts/DevChecks.py").write_text("raise SystemExit(7)\n")
        bootstrap.git(self.project, "init")
        bootstrap.git(self.project, "add", ".")
        self.commit(self.project)
        destination = self.root / "failed verification"
        with self.assertRaisesRegex(RuntimeError, "fast checks failed"):
            bootstrap.verify(self.project, destination, self.source)
        report = json.loads((destination / "artifacts/reconstruction.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["fast_exit_code"], 7)
        self.assertEqual((destination / "upstream/firefox/sample.txt").read_bytes(), b"second\n")


if __name__ == "__main__":
    unittest.main()
