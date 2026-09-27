#!/usr/bin/env python3
"""Fail when a file's line-ending convention changed, and optionally repair it.

Editing a file through a tool that reads and rewrites whole text (Python's
`Path.read_text`/`write_text` among them) silently rewrites every line ending to
the platform's, on Windows turning an LF file into a CRLF one. Nothing breaks,
so it is easy to miss -- but every line of the file then shows as changed, which
makes a real diff unreviewable and fills the exported Firefox patch with
thousands of whitespace errors.

Two sets of files are checked:

* Tracked files, against the convention the committed version uses. A file is
  allowed to keep a mixed convention it already had; what is refused is
  flipping, because that is never deliberate here.
* The Firefox sources the patches touch, against LF. `upstream/` is ignored by
  git and has no committed version to compare with, and mozilla-central is LF
  throughout, so LF is the authority there.

  Check-LineEndings.py            # report, exit 1 if anything drifted
  Check-LineEndings.py --fix      # repair in place, preserving each edit
  Check-LineEndings.py --base REF # compare committed changes against REF (CI)
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Anything whose bytes are not line-oriented text, where "fixing" endings would
# corrupt the file.
BINARY = {".png", ".ico", ".dll", ".exe", ".lib", ".pdb", ".pfx", ".p12", ".zip"}


def style(data):
    crlf = data.count(b"\r\n")
    lf = data.count(b"\n") - crlf
    if crlf and lf:
        return "mixed"
    if crlf:
        return "crlf"
    if lf:
        return "lf"
    return "none"


def convert(data, target):
    # Normalise to LF first so a mixed file converges rather than doubling up.
    lf = data.replace(b"\r\n", b"\n")
    return lf.replace(b"\n", b"\r\n") if target == "crlf" else lf


def tracked_files(base="HEAD"):
    out = subprocess.run(["git", "-C", str(ROOT), "diff", "--name-only", "-z", base, "--"],
                         capture_output=True, text=True, check=True).stdout
    return [name for name in out.split("\0") if name]


def committed(name, base="HEAD"):
    result = subprocess.run(["git", "-C", str(ROOT), "show", f"{base}:{name}"],
                            capture_output=True)
    return result.stdout if result.returncode == 0 else None


def patched_files():
    """Firefox sources the patches apply to, read from the patch headers."""
    names = set()
    for patch in sorted((ROOT / "patches").glob("*.patch")):
        for line in patch.read_text(encoding="utf-8", errors="replace").splitlines():
            match = re.match(r"^\+\+\+ b/(.+)$", line)
            if match and match.group(1) != "/dev/null":
                names.add(match.group(1))
    return sorted(names)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fix", action="store_true",
                        help="rewrite drifted files instead of only reporting them")
    parser.add_argument("--base", default="HEAD",
                        help="compare against this commit instead of HEAD (including already committed changes)")
    arguments = parser.parse_args()
    result = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--verify",
                             "--end-of-options", f"{arguments.base}^{{commit}}"],
                            capture_output=True, text=True)
    if result.returncode:
        print(f"Cannot resolve comparison commit: {arguments.base}", file=sys.stderr)
        return 1
    base = result.stdout.strip()

    drifted = []

    for name in tracked_files(base):
        path = ROOT / name
        if not path.is_file() or path.suffix.lower() in BINARY:
            continue
        before = committed(name, base)
        if before is None:
            continue  # newly added: it sets its own convention
        want, have = style(before), style(path.read_bytes())
        if want in ("none", "mixed") or have == want:
            continue
        drifted.append((name, want, have))

    for name in patched_files():
        path = ROOT / "upstream" / "firefox" / name
        if not path.is_file() or path.suffix.lower() in BINARY:
            continue
        have = style(path.read_bytes())
        if have in ("lf", "none"):
            continue
        drifted.append((f"upstream/firefox/{name}", "lf", have))

    if not drifted:
        print("OK line endings unchanged")
        return 0

    for name, want, have in drifted:
        print(f"{'FIXED' if arguments.fix else 'DRIFT'} {name}: {have}, expected {want}")
        if arguments.fix:
            path = ROOT / name
            path.write_bytes(convert(path.read_bytes(), want))

    if arguments.fix:
        print(f"Repaired {len(drifted)} file(s); re-export any patch that covers them.")
        return 0
    print(f"{len(drifted)} file(s) drifted. Re-run with --fix.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
