"""Reclaim space from development MSIX runs without losing packages or evidence.

Removes only regenerable intermediates (payload/, layout/, pri-input/ and the
upstream.msix that older builds packed twice) from each run under
artifacts/msix-dev. Each run keeps its MSIX, reports, manifests and PRI dump.
Lists what it would remove unless --apply is given.
"""
import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
INTERMEDIATES = ("payload", "layout", "pri-input", "upstream.msix")


def size(path):
    if path.is_file():
        return path.stat().st_size
    return sum(p.stat().st_size for p in path.rglob("*") if p.is_file())


def prune(runs_dir, apply=False):
    removed = []
    for run in sorted(p for p in runs_dir.iterdir() if p.is_dir()):
        if not any(run.glob("*_x64.msix")):
            continue  # Unfinished or failed run: keep everything for inspection.
        for name in INTERMEDIATES:
            target = run / name
            if target.exists():
                removed.append((target, size(target)))
                if apply:
                    shutil.rmtree(target) if target.is_dir() else target.unlink()
    return removed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="delete instead of listing")
    parser.add_argument("--runs-dir", type=Path, default=ROOT / "artifacts/msix-dev")
    args = parser.parse_args()
    removed = prune(args.runs_dir, args.apply)
    for target, bytes_ in removed:
        print(f"{bytes_ / 2**20:9.1f} MiB  {target.relative_to(args.runs_dir)}")
    total = sum(bytes_ for _, bytes_ in removed) / 2**30
    print(f"{'Removed' if args.apply else 'Would remove'} {total:.2f} GiB from {len(removed)} intermediates")


if __name__ == "__main__":
    main()
