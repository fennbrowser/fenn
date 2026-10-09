#!/usr/bin/env python3
"""Read-only prerequisite diagnosis and the non-UI development verification lane."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def environment(root):
    """Match the build wrappers without creating caches or changing user settings."""
    env = os.environ.copy()
    shared = Path(env.get("FENN_SHARED_CACHE") or root / ".cache")
    env.update(CARGO_HOME=str(shared / "cargo"), RUSTUP_HOME=str(shared / "rustup"),
               MOZBUILD_STATE_PATH=str(root / ".cache/mozbuild"),
               RUSTUP_AUTO_INSTALL="0", CARGO_NET_OFFLINE="true")
    env["PATH"] = str(shared / "cargo/bin") + os.pathsep + env.get("PATH", "")
    return env, shared


def tool(name, candidates, env):
    for candidate in candidates:
        try:
            if Path(candidate).is_file():
                return str(candidate)
        except PermissionError:
            # Keep the selected path so execution reports the access problem
            # instead of silently switching to a different toolchain on PATH.
            return str(candidate)
    return shutil.which(name, path=env["PATH"])


class Checks:
    def __init__(self, root, env):
        self.root, self.env = root, env
        self.results = []

    def record(self, name, status, detail=""):
        self.results.append((name, status))
        print(f"{status}: {name}" + (f" - {detail}" if detail else ""), flush=True)

    def run(self, name, command, missing=None, expected=None):
        if missing:
            self.record(name, "BLOCKED", missing)
            return
        print(f"\nRunning {name}", flush=True)
        try:
            result = subprocess.run(command, cwd=self.root, env=self.env,
                                    text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            print(result.stdout, end="" if result.stdout.endswith("\n") else "\n", flush=True)
            passed = result.returncode == 0 and (expected is None or result.stdout.strip() == expected)
            self.record(name, "PASS" if passed else "FAIL",
                        f"expected {expected}" if result.returncode == 0 and not passed
                        else f"exit {result.returncode}")
        except OSError as error:
            self.record(name, "BLOCKED", str(error))

    def finish(self):
        print("\nSummary", flush=True)
        for name, status in self.results:
            print(f"  {status}: {name}")
        return int(any(status in ("FAIL", "BLOCKED") for _, status in self.results))


def resolve(root, env, shared):
    suffix = ".exe" if os.name == "nt" else ""
    cargo = tool("cargo", [shared / f"cargo/bin/cargo{suffix}"], env)
    node = tool("node", [root / f".cache/mozbuild/node/node{suffix}",
                         shared / f"mozbuild/node/node{suffix}"], env)
    # Keep precedence aligned with Export-ShellCoreHeader.py, including CBINDGEN.
    cbindgen = env.get("CBINDGEN") or tool("cbindgen", [
        shared / "mozbuild/cbindgen/cbindgen.exe",
        root / ".cache/mozbuild/cbindgen/cbindgen.exe",
        root.parent / "firefox-winui/.cache/mozbuild/cbindgen/cbindgen.exe"], env)
    return {"git": tool("git", [], env), "cargo": cargo, "node": node,
            "cbindgen": cbindgen}


def source_problem(root):
    firefox = root / "upstream/firefox"
    if not (firefox / ".git").exists():
        return "Prepare the pinned Firefox checkout and apply the project patch; see CONTRIBUTING.md."
    # All patch-added source files must exist, not just one representative adapter.
    for patch in sorted((root / "patches").glob("*.patch")):
        for line in patch.read_text(encoding="utf-8").splitlines():
            if line.startswith("+++ b/") and not (firefox / line[6:]).is_file():
                return f"Missing patched source {line[6:]}; apply the project patch."
    return None


def doctor(checks, tools, baseline):
    checks.record("Python", "PASS", f"{sys.version.split()[0]} ({sys.executable})")
    for name in ("git", "cargo", "node", "cbindgen"):
        checks.run(name, [tools[name], "--version"],
                   missing=None if tools[name] else f"Install/bootstrap {name}; see CONTRIBUTING.md.")
    problem = source_problem(checks.root)
    checks.record("Patched Firefox source files", "BLOCKED" if problem else "PASS", problem or "")
    checks.run("Firefox base", [tools["git"], "-C", str(checks.root / "upstream/firefox"),
                                "rev-parse", "HEAD"],
               missing=problem or (None if tools["git"] else "Git is missing."),
               expected=baseline["firefox"]["commit"])
    if os.name != "nt":
        checks.record("Windows native build", "BLOCKED", "Native build and UIA require Windows x64.")
        return
    paths = {
        "MozillaBuild": (Path("C:/mozilla-build/msys2/usr/bin/bash.exe"), "Install MozillaBuild at C:\\mozilla-build."),
        "NuGet restore CLI": (checks.root / "artifacts/setup/nuget.exe", "Provision the NuGet CLI at artifacts/setup/nuget.exe."),
        "Windows SDK makepri": (Path("C:/Program Files (x86)/Windows Kits/10/bin") /
                                baseline["winuiWindowsSDK"] / "x64/makepri.exe", "Install the Windows SDK listed in configs/toolchain-baseline.json."),
    }
    for name, (path, remedy) in paths.items():
        checks.record(name, "PASS" if path.is_file() else "BLOCKED", str(path) if path.is_file() else remedy)
    vswhere = Path("C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
    if vswhere.is_file():
        result = subprocess.run([str(vswhere), "-latest", "-requires",
                                 "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                 "-property", "installationPath"], capture_output=True, text=True)
        installation = result.stdout.strip()
        ready = result.returncode == 0 and bool(installation) and (Path(installation) / "MSBuild/Current/Bin/MSBuild.exe").is_file()
        checks.record("Visual Studio C++ / MSBuild", "PASS" if ready else "BLOCKED",
                      installation if ready else "Install workloads from configs/visualstudio.vsconfig.")
    else:
        checks.record("Visual Studio C++ / MSBuild", "BLOCKED", "Install Visual Studio using configs/visualstudio.vsconfig.")
    print("\nTool versions above are observations; the baseline is recorded history, not an exact-version gate.")
    print("This diagnosis does not prove SDK restore, compilation, runtime compatibility, or patch content equality.")


def fast(checks, tools):
    python = sys.executable
    problem = source_problem(checks.root)
    checks.run("Line endings", [python, "scripts/Check-LineEndings.py"],
               missing=None if tools["git"] else "Git is required.")
    checks.run("ShellCore", [tools["cargo"], "test", "--offline", "--locked",
                             "--manifest-path", "native/ShellCore/Cargo.toml"],
               missing=None if tools["cargo"] else "Cargo is missing; run the doctor.")
    js_tests = sorted(str(path.relative_to(checks.root)) for path in (checks.root / "tests").glob("*.test.mjs"))
    checks.run("JavaScript contracts", [tools["node"], "--test", *js_tests],
               missing=problem or (None if tools["node"] else "Node is missing; run the doctor.")
               or (None if js_tests else "No JavaScript test files found."))
    checks.run("Python unit tests", [python, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py", "-v"])
    checks.run("ShellCore generated header", [python, "scripts/Export-ShellCoreHeader.py", "--check"],
               missing=None if tools["cbindgen"] and tools["cargo"] else "cbindgen and Cargo are required; run the doctor.")
    checks.record("WinUI manifest, native contracts, Firefox/UIA and performance", "NOT RUN",
                  "Outside the fast lane; see CONTRIBUTING.md for build and integration commands.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("doctor", "fast"))
    args = parser.parse_args()
    env, shared = environment(ROOT)
    tools = resolve(ROOT, env, shared)
    # The same resolved generator is used for both diagnosis and verification.
    if tools["cbindgen"]:
        env["CBINDGEN"] = tools["cbindgen"]
    checks = Checks(ROOT, env)
    if args.mode == "doctor":
        doctor(checks, tools, json.loads((ROOT / "configs/toolchain-baseline.json").read_text()))
    else:
        fast(checks, tools)
    return checks.finish()


if __name__ == "__main__":
    raise SystemExit(main())
