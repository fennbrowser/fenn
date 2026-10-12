#!/usr/bin/env python3
"""Prepare pinned Firefox source, or verify committed patches in an isolated clone."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ("0001-fenn-winui-pane.patch",)
# Firefox directories the fast checks read beyond the patch targets. A sparse
# verification checks out only these and each patched file's directory.
SPARSE_DIRECTORIES = ("browser", "toolkit", "devtools/startup", "devtools/client/performance-new/popup",
                      "third_party/node/node_modules/acorn")


def git_env():
    env = os.environ.copy()
    # An inherited alternate index/worktree must never redirect our mutations.
    for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR",
                "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES"):
        env.pop(key, None)
    env["GIT_TERMINAL_PROMPT"] = "0"
    return env


def git(cwd, *args):
    result = subprocess.run(["git", "-C", str(cwd), *map(str, args)],
                            env=git_env(), stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or f"Git exited {result.returncode}")
    return result.stdout.strip()


def reserve(path):
    # mkdir without exist_ok also refuses empty dirs, files and dangling links.
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        path.mkdir()
    except FileExistsError:
        raise RuntimeError(f"Refusing existing destination: {path}. Nothing was reset or overwritten.") from None


def inputs(root):
    config = json.loads((root / "configs/toolchain-baseline.json").read_text(encoding="utf-8"))["firefox"]
    if not re.fullmatch(r"[0-9a-fA-F]{40}", config["commit"]):
        raise ValueError("Firefox baseline must contain a full 40-character commit SHA.")
    patches = [root / "patches" / name for name in PATCHES]
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in patches}
    return config, patches, hashes


def sparse_directories(patches):
    """Cone-mode directories: the checked Firefox areas plus every patch target's directory."""
    directories = set(SPARSE_DIRECTORIES)
    for patch in patches:
        for line in patch.read_text(encoding="utf-8").splitlines():
            if line.startswith(("--- a/", "+++ b/")):
                parent = Path(line[6:]).parent.as_posix()
                if parent != ".":
                    directories.add(parent)
    return sorted(directories)


def prepare(root, reference=None, sparse=False):
    config, patches, hashes = inputs(root)
    target = root / "upstream/firefox"
    if os.path.lexists(target):
        raise RuntimeError(f"Refusing existing destination: {target}. Nothing was reset or overwritten.")
    if reference is not None:
        reference = reference.resolve(strict=True)
        actual = git(reference, "rev-parse", f"{config['commit']}^{{commit}}")
        if actual.lower() != config["commit"].lower():
            raise RuntimeError("Reference does not contain the pinned Firefox commit.")
    reserve(target)
    phase = "initializing repository"
    try:
        print(f"Preparing {target}", flush=True)
        if reference is not None:
            phase = "copying reference Git objects"
            print("Copying local Git objects (no hardlinks or shared database)", flush=True)
            git(root, "clone", "--local", "--no-hardlinks", "--no-checkout", reference, target)
        else:
            git(target, "init")
        git(target, "config", "core.longpaths", "true")
        git(target, "config", "core.autocrlf", "false")
        if reference is not None:
            git(target, "remote", "set-url", "origin", config["repository"])
        else:
            git(target, "remote", "add", "origin", config["repository"])
            phase = "fetching pinned commit"
            print(f"Fetching {config['commit']} (independent object database)", flush=True)
            if sparse:
                # Blobless: checkout fetches only the sparse files' contents.
                git(target, "config", "remote.origin.promisor", "true")
                git(target, "config", "remote.origin.partialclonefilter", "blob:none")
                git(target, "fetch", "--no-tags", "--depth=1", "--filter=blob:none", "origin", config["commit"])
            else:
                git(target, "fetch", "--no-tags", "--depth=1", "origin", config["commit"])
        if sparse:
            phase = "restricting checkout"
            print("Checking out only the directories the fast checks read", flush=True)
            git(target, "sparse-checkout", "set", "--cone", *sparse_directories(patches))
        phase = "checking out pinned commit"
        print("Checking out Firefox source", flush=True)
        git(target, "checkout", "--detach", config["commit"])
        if git(target, "rev-parse", "HEAD").lower() != config["commit"].lower():
            raise RuntimeError("Checked-out HEAD differs from the pinned commit.")
        # A reference may itself borrow objects; sever that dependency only
        # after the pinned revision is reachable from this checkout's HEAD.
        if (target / ".git/objects/info/alternates").exists():
            phase = "making borrowed Git objects independent"
            git(target, "repack", "-a", "-d")
            (target / ".git/objects/info/alternates").unlink()
        for patch in patches:
            phase = f"applying {patch.name}"
            print(phase, flush=True)
            git(target, "apply", "--index", "--check", patch)
            git(target, "apply", "--index", patch)
        phase = "verifying patched index"
        git(target, "diff", "--exit-code")
        # Reversing the complete series must restore the exact pinned tree.
        # Use an isolated index, never the developer's or the patched index.
        index = target / ".git/reconstruction-check-index"
        env = git_env()
        env["GIT_INDEX_FILE"] = str(index)
        index.write_bytes((target / ".git/index").read_bytes())
        try:
            for patch in reversed(patches):
                subprocess.run(["git", "-C", str(target), "apply", "--cached", "--reverse", str(patch)],
                               env=env, check=True)
            subprocess.run(["git", "-C", str(target), "diff", "--cached", "--exit-code", config["commit"]],
                           env=env, check=True)
        finally:
            index.unlink(missing_ok=True)
        report = {"firefox_commit": config["commit"], "patch_sha256": hashes,
                  "patched_tree": git(target, "write-tree"), "status": "passed"}
        if sparse:
            report["sparse_directories"] = sparse_directories(patches)
        (target / ".git/firefox-winui-bootstrap.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("Pinned source and ordered patches verified. Changes remain staged in Firefox.", flush=True)
        return report
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise RuntimeError(f"Failed while {phase}: {error}\nPartial checkout retained at {target}. "
                           "Inspect it, then choose a fresh project checkout or manually move the partial directory "
                           "aside before retrying. No automatic reset, deletion or resume was attempted.") from error


def verify(root, destination, reference=None, sparse=False):
    revision = git(root, "rev-parse", "HEAD")
    reserve(destination)
    report = {"project_commit": revision, "destination": str(destination), "status": "failed"}
    report_path = destination / "artifacts/reconstruction.json"
    try:
        print(f"Cloning committed project {revision} into {destination}", flush=True)
        git(root, "clone", "--no-hardlinks", "--no-checkout", str(root), str(destination))
        git(destination, "config", "core.longpaths", "true")
        git(destination, "config", "core.autocrlf", "false")
        git(destination, "checkout", "--detach", revision)
        report["source"] = prepare(destination, reference, sparse)
        report_path.parent.mkdir(parents=True, exist_ok=True)
        env = git_env()
        env["FENN_SHARED_CACHE"] = os.environ.get("FENN_SHARED_CACHE") or str(root / ".cache")
        env["CARGO_TARGET_DIR"] = str(destination / "build/shell-core-tests")
        # All transient test files also stay in the disposable checkout.
        temporary = destination / "artifacts/tmp"
        temporary.mkdir(parents=True)
        env.update(TMP=str(temporary), TEMP=str(temporary), TMPDIR=str(temporary))
        log = destination / "artifacts/fast-checks.log"
        print(f"Running committed fast checks; output: {log}", flush=True)
        with log.open("w", encoding="utf-8") as output:
            result = subprocess.run([sys.executable, str(destination / "scripts/DevChecks.py"), "fast"],
                                    cwd=destination, env=env, stdout=output, stderr=subprocess.STDOUT)
        report["fast_exit_code"] = result.returncode
        if result.returncode:
            raise RuntimeError(f"Reconstructed-source fast checks failed (exit {result.returncode}); inspect {log}")
        report["status"] = "passed"
        print(f"PASS: committed source reconstruction and fast checks. Evidence: {report_path}", flush=True)
        return report
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        report["error"] = str(error)
        raise
    finally:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Verification checkout retained at {destination}; no profiles were launched.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "verify"))
    parser.add_argument("--reference", type=Path, help="Read pinned Git objects from an existing Firefox repository instead of the network")
    parser.add_argument("--destination", type=Path, help="New verification project directory (verify only)")
    parser.add_argument("--sparse", action="store_true",
                        help="Check out only the Firefox directories the fast checks read (verify only; not buildable)")
    args = parser.parse_args()
    if args.mode == "prepare" and (args.destination or args.sparse):
        parser.error("--destination and --sparse are only supported for verify; prepare uses this project's upstream/firefox")
    try:
        if args.mode == "prepare":
            prepare(ROOT, args.reference)
        else:
            destination = (args.destination or ROOT / "artifacts" / f"reconstruct-{uuid.uuid4().hex[:8]}").absolute()
            verify(ROOT, destination, args.reference, args.sparse)
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
