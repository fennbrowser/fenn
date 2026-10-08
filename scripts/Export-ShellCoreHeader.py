#!/usr/bin/env python3
"""Generate (or verify) bridge/v2/shell_core.h from native/ShellCore.

The checked-in header is what the native pane compiles against, so the build
does not need cbindgen. This script regenerates it and, with --check, fails if
the checked-in copy has drifted from the Rust source.

cbindgen comes from the pinned mozbuild toolchain (the same binary Firefox's own
build uses), not from a separate install, so the generator version moves only
when the Firefox toolchain does.
"""

import argparse
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CRATE = ROOT / "native" / "ShellCore"
HEADER = ROOT / "bridge" / "v2" / "shell_core.h"
# Use the same generator pin for local verification and CI provisioning.
EXPECTED_CBINDGEN = json.loads((ROOT / "configs/toolchain-baseline.json").read_text(encoding="utf-8"))["cbindgen"]


def find_cbindgen():
    override = os.environ.get("CBINDGEN")
    if override:
        return pathlib.Path(override)
    candidates = []
    shared = os.environ.get("FENN_SHARED_CACHE")
    if shared:
        candidates.append(pathlib.Path(shared) / "mozbuild" / "cbindgen" / "cbindgen.exe")
    candidates.append(ROOT / ".cache" / "mozbuild" / "cbindgen" / "cbindgen.exe")
    candidates.append(ROOT.parent / "firefox-winui" / ".cache" / "mozbuild" / "cbindgen" / "cbindgen.exe")
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return pathlib.Path("cbindgen")


def generate(cbindgen):
    version = subprocess.run(
        [str(cbindgen), "--version"], capture_output=True, text=True, check=True
    ).stdout.strip()
    if not version.endswith(EXPECTED_CBINDGEN):
        print(
            f"warning: {version} is not the pinned cbindgen {EXPECTED_CBINDGEN}; "
            "the generated header may differ from the checked-in one",
            file=sys.stderr,
        )
    result = subprocess.run(
        [str(cbindgen), "--config", "cbindgen.toml", "--lang", "c", "--quiet", "."],
        cwd=CRATE,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        sys.stderr.write(result.stderr)
        raise SystemExit(result.returncode)
    # Normalise to LF so the checked-in header does not churn on Windows.
    return result.stdout.replace("\r\n", "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if the checked-in header differs from freshly generated output",
    )
    arguments = parser.parse_args()

    cbindgen = find_cbindgen()
    generated = generate(cbindgen)
    existing = HEADER.read_text(encoding="utf-8").replace("\r\n", "\n") if HEADER.exists() else None

    if arguments.check:
        if existing is None:
            print(f"FAIL {HEADER} does not exist", file=sys.stderr)
            return 1
        if existing != generated:
            print(
                f"FAIL {HEADER} is out of date with native/ShellCore/src/ffi.rs.\n"
                "Run scripts/Export-ShellCoreHeader.py to regenerate it.",
                file=sys.stderr,
            )
            return 1
        print(f"OK {HEADER.relative_to(ROOT)} matches native/ShellCore ({cbindgen.name})")
        return 0

    HEADER.parent.mkdir(parents=True, exist_ok=True)
    HEADER.write_text(generated, encoding="utf-8", newline="\n")
    print(f"Wrote {HEADER.relative_to(ROOT)} ({len(generated)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
