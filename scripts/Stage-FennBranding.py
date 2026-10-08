"""Mirror the project's Fenn branding into the Firefox checkout.

branding/fenn is the source of truth. configs/mozconfig builds with
--with-branding=browser/branding/fenn, so mach.sh runs this before every mach
command: it copies new or changed files, removes files the source no longer has,
and leaves unchanged files untouched so their timestamps do not trigger rebuilds.
The directory is project-owned, not part of the Firefox patches, so it is also
listed in the checkout's .git/info/exclude.

It also provides browser/app/fenn.exe.manifest. The build embeds a program's
manifest only from a file named <program>.exe.manifest beside its sources, and the
executable is fenn.exe (--with-app-name=fenn), so without this copy of Firefox's
firefox.exe.manifest it would silently carry none. The copy is refreshed from the
checkout's firefox.exe.manifest on every run, so it never drifts from upstream.
"""
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "branding/fenn"
CHECKOUT = ROOT / "upstream/firefox"
RELATIVE = "browser/branding/fenn"
APP_MANIFEST = ("browser/app/firefox.exe.manifest", "browser/app/fenn.exe.manifest")


def exclude(checkout, entry):
    path = checkout / ".git/info/exclude"
    if path.parent.is_dir():
        lines = path.read_text(encoding="utf-8").splitlines() if path.is_file() else []
        if entry not in lines:
            path.write_text("\n".join(lines + [entry]) + "\n", encoding="utf-8")


def stage_app_manifest(checkout=CHECKOUT):
    source, target = (checkout / name for name in APP_MANIFEST)
    if not source.is_file():
        raise SystemExit(f"{source} is missing; Firefox's executable manifest is the template")
    exclude(checkout, f"/{APP_MANIFEST[1]}")
    if target.is_file() and target.read_bytes() == source.read_bytes():
        return []
    shutil.copyfile(source, target)
    return [f"updated {APP_MANIFEST[1]}"]


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
    exclude(checkout, f"/{RELATIVE}/")
    return changed


if __name__ == "__main__":
    if not (CHECKOUT / "mach").is_file():
        sys.exit(f"No Firefox checkout at {CHECKOUT}")
    changes = stage() + stage_app_manifest()
    print(f"Fenn branding: {len(changes)} change(s)" + "".join(f"\n  {c}" for c in changes))
