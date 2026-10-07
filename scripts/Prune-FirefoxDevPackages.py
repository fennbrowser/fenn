"""Reclaim space from development MSIX artifacts without losing packages or evidence.

By default it removes only regenerable data:
  - intermediates (payload/, layout/, pri-input/ and the upstream.msix that older
    builds packed twice) from each finished run under artifacts/msix-dev, and from
    failed runs that a newer finished run has superseded;
  - the browser profile/ of each smoke run under artifacts/msix-dev-smoke except
    the newest --keep-profiles. Each smoke keeps its result.json and process.log.
--keep-runs N also deletes whole runs older than the newest N, apart from the run
latest.json names and the run of the installed version. The App Installer feed
needs no pruning: it holds only the current package and the one before it.
Lists what it would remove unless --apply is given.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
INTERMEDIATES = ("payload", "layout", "pri-input", "upstream.msix")


def size(path):
    if path.is_file():
        return path.stat().st_size
    return sum(p.stat().st_size for p in path.rglob("*") if p.is_file())


def run_version(run):
    try:
        return tuple(int(part) for part in run.name.split("-", 1)[0].split("."))
    except ValueError:
        return None


def installed_version(name="FirefoxWinUI.Dev"):
    result = subprocess.run(["powershell", "-NoProfile", "-Command",
                             f"(Get-AppxPackage -Name {name}).Version | Select-Object -First 1"],
                            capture_output=True, text=True)
    text = result.stdout.strip()
    return tuple(int(part) for part in text.split(".")) if text else None


def prune(runs_dir, apply=False, keep_runs=None, installed=None):
    removed = []

    def remove(target):
        removed.append((target, size(target)))
        if apply:
            shutil.rmtree(target) if target.is_dir() else target.unlink()

    runs = sorted((p for p in runs_dir.iterdir() if p.is_dir() and run_version(p)), key=run_version)
    finished = [run for run in runs if any(run.glob("*_x64.msix"))]
    newest = run_version(finished[-1]) if finished else None
    protected = set()
    latest = runs_dir / "latest.json"
    if latest.is_file():
        package = Path(json.loads(latest.read_text(encoding="utf-8")).get("package", ""))
        protected.add(package.parent.name)
    if installed:
        protected.update(run.name for run in finished if run_version(run) == installed)
    if keep_runs is not None:
        kept = runs[-keep_runs:] if keep_runs else []
        for run in runs:
            if run not in kept and run.name not in protected:
                remove(run)
        runs = [run for run in runs if run in kept or run.name in protected]
    for run in runs:
        superseded = newest and run_version(run) < newest
        if run not in finished and not superseded:
            continue  # The newest failed run: keep everything for inspection.
        for name in INTERMEDIATES:
            target = run / name
            if target.exists():
                remove(target)
    return removed


def prune_smoke_profiles(smoke_dir, apply=False, keep=3):
    removed = []
    runs = sorted((p for p in smoke_dir.iterdir() if (p / "profile").is_dir()),
                  key=lambda p: p.stat().st_mtime)
    for run in runs[:max(len(runs) - keep, 0)]:
        target = run / "profile"
        removed.append((target, size(target)))
        if apply:
            shutil.rmtree(target)
    return removed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--apply", action="store_true", help="delete instead of listing")
    parser.add_argument("--runs-dir", type=Path, default=ROOT / "artifacts/msix-dev")
    parser.add_argument("--smoke-dir", type=Path, default=ROOT / "artifacts/msix-dev-smoke")
    parser.add_argument("--keep-runs", type=int, help="delete whole runs older than the newest N")
    parser.add_argument("--keep-profiles", type=int, default=3,
                        help="smoke runs whose browser profile is kept (default 3)")
    args = parser.parse_args()
    removed = []
    if args.runs_dir.is_dir():
        installed = installed_version() if args.keep_runs is not None else None
        removed += [(t, b, args.runs_dir) for t, b in prune(args.runs_dir, args.apply, args.keep_runs, installed)]
    if args.smoke_dir.is_dir():
        removed += [(t, b, args.smoke_dir)
                    for t, b in prune_smoke_profiles(args.smoke_dir, args.apply, args.keep_profiles)]
    for target, bytes_, base in removed:
        print(f"{bytes_ / 2**20:9.1f} MiB  {base.name}/{target.relative_to(base).as_posix()}")
    total = sum(bytes_ for _, bytes_, _ in removed) / 2**30
    print(f"{'Removed' if args.apply else 'Would remove'} {total:.2f} GiB in {len(removed)} items")


if __name__ == "__main__":
    main()
