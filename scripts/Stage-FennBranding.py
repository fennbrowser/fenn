"""Mirror the project's Fenn branding into the Firefox checkout.

branding/fenn is the source of truth. configs/mozconfig builds with
--with-branding=browser/branding/fenn, so mach.sh runs this before every mach
command: it copies new or changed files, removes files the source no longer has,
and leaves unchanged files untouched so their timestamps do not trigger rebuilds.
The directory is project-owned, not part of the Firefox patches, so it is also
listed in the checkout's .git/info/exclude.
"""
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "branding/fenn"
CHECKOUT = ROOT / "upstream/firefox"
RELATIVE = "browser/branding/fenn"


def stage(source=SOURCE, checkout=CHECKOUT):
    target = checkout / RELATIVE
    wanted = {p.relative_to(source) for p in source.rglob("*") if p.is_file()}
    if not wanted:
        raise SystemExit(f"{source} holds no branding files")
    changed = []
    for relative in sorted(wanted):
        src, dst = source / relative, target / relative
        if not dst.is_file() or dst.read_bytes() != src.read_bytes():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
            changed.append(f"updated {relative.as_posix()}")
    if target.is_dir():
        for path in sorted(target.rglob("*"), reverse=True):
            relative = path.relative_to(target)
            if path.is_file() and relative not in wanted:
                path.unlink()
                changed.append(f"removed {relative.as_posix()}")
            elif path.is_dir() and not any(path.iterdir()):
                path.rmdir()
    exclude = checkout / ".git/info/exclude"
    if exclude.parent.is_dir():
        lines = exclude.read_text(encoding="utf-8").splitlines() if exclude.is_file() else []
        if f"/{RELATIVE}/" not in lines:
            exclude.write_text("\n".join(lines + [f"/{RELATIVE}/"]) + "\n", encoding="utf-8")
    return changed


if __name__ == "__main__":
    if not (CHECKOUT / "mach").is_file():
        sys.exit(f"No Firefox checkout at {CHECKOUT}")
    changes = stage()
    print(f"Fenn branding: {len(changes)} change(s)" + "".join(f"\n  {c}" for c in changes))
