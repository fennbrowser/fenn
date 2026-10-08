"""Stage the framework-dependent pane into Firefox's dist/bin.

The pane runs on the shared Microsoft.WindowsAppRuntime.2 framework, which an
unpackaged Firefox joins at startup through the bootstrapper. Staging therefore
copies only the pane and the bootstrapper DLL. It also removes the runtime files
an earlier, self-contained staging recorded, so no second runtime copy is left
beside fenn.exe. Patch 2 also
stops Firefox's build from shipping its own app-local Windows App SDK DLLs.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "build/winui-pane/x64/Release"
DEST = ROOT / "build/firefox/dist/bin"
RECORD = ROOT / "artifacts/setup/winui-pane-staging.json"
STAGED = ("FennPane.dll", "Microsoft.WindowsAppRuntime.Bootstrap.dll")


def image_digest(path):
    """SHA-256 of a PE image without its certificate, its directory entry and checksum."""
    data = bytearray(path.read_bytes())
    if path.suffix.lower() == ".dll" and data[:2] == b"MZ":
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"Invalid PE: {path}")
        optional = pe + 24
        if struct.unpack_from("<H", data, optional)[0] != 0x20B:
            raise ValueError(f"Expected x64 PE: {path}")
        security = optional + 112 + 4 * 8
        offset, size = struct.unpack_from("<II", data, security)
        if offset:
            if offset + size != len(data):
                raise ValueError(f"Unexpected certificate layout: {path}")
            del data[offset:]
        data[optional + 64:optional + 68] = bytes(4)
        data[security:security + 8] = bytes(8)
    return hashlib.sha256(data).hexdigest()


def remove_self_contained_staging(dest_dir, record):
    """Delete the runtime files a self-contained staging copied into dest_dir.

    Its record lists the pane's whole runtime. ("preserved" only meant that a
    file already existed, which re-staging made true for all of them.)"""
    if not record.is_file():
        return 0
    previous = json.loads(record.read_text(encoding="utf-8"))
    removed = 0
    for entry in previous.get("files", []):
        if "imageSha256" not in entry:
            continue  # A framework-dependent record.
        target = dest_dir / entry["file"]
        if target.is_file():
            target.unlink()
            removed += 1
            parent = target.parent
            while parent != dest_dir and parent.is_dir() and not any(parent.iterdir()):
                parent.rmdir()
                parent = parent.parent
    return removed


def stage(source_dir=SOURCE, dest_dir=DEST, record=RECORD):
    sources = [source_dir / name for name in STAGED]
    missing = [str(p) for p in sources if not p.is_file()]
    if missing:
        raise RuntimeError(f"Build the pane first (Build-WinUIPane.ps1); missing {missing}")
    removed = remove_self_contained_staging(dest_dir, record)
    for source in sources:
        shutil.copyfile(source, dest_dir / source.name)
    record.parent.mkdir(parents=True, exist_ok=True)
    record.write_text(json.dumps({
        "runtime": "Microsoft.WindowsAppRuntime.2 framework (bootstrapped when unpackaged)",
        "files": [{"file": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in sources],
    }, indent=2), encoding="utf-8")
    print(f"Staged {', '.join(STAGED)}; removed {removed} self-contained runtime files; record: {record}")


if __name__ == "__main__":
    stage()
