"""Stage the 2.5.1 pane without replacing Firefox's existing runtime files."""
import hashlib
import json
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "build/winui-pane/x64/Release"
DEST = ROOT / "build/firefox/dist/bin"


def image_digest(path):
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


def stage(source_dir=SOURCE, dest_dir=DEST, manifest=None):
    names = {
        "coremessagingxp.dll", "marshal.dll", "dcompi.dll", "dwmcorei.dll",
        "dwmscenei.dll", "dwritecore.dll", "mrm.dll", "wuceffectsi.dll", "winuiedit.dll",
        "microsoft.directmanipulation.dll", "microsoft.graphics.display.dll",
        "microsoft.inputstatemanager.dll", "microsoft.internal.frameworkudk.dll",
        "microsoft.windowsappruntime.dll", "microsoft.windowsappruntime.pri",
        "microsoft.windows.applicationmodel.resources.dll",
        "resources.pri",
    }
    files = [p for p in source_dir.rglob("*") if p.is_file() and
             ((p.name.lower() in names or p.name.lower().startswith("microsoft.ui")) and
              p.suffix.lower() in {".dll", ".pri", ".winmd", ".mui"} or
              p.relative_to(source_dir).parts[0] == "Microsoft.UI.Xaml")]
    if not (source_dir / "Microsoft.ui.xaml.dll").is_file():
        raise RuntimeError("Build the pane and its self-contained runtime first")
    records = []
    for source in files:
        target = dest_dir / source.relative_to(source_dir)
        if target.exists() and image_digest(source) != image_digest(target):
            raise RuntimeError(f"Runtime image mismatch: {target}. Nothing staged.")
        records.append({"file": source.relative_to(source_dir).as_posix(),
                        "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "imageSha256": image_digest(source), "preserved": target.exists()})
    for source in files:
        target = dest_dir / source.relative_to(source_dir)
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    shutil.copyfile(source_dir / "FirefoxWinUIPane.dll", dest_dir / "FirefoxWinUIPane.dll")
    manifest = manifest or ROOT / "artifacts/setup/winui-pane-staging.json"
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps({"appSdk": "2.5.1", "files": records}, indent=2))
    print(f"Staged pane and {len(files)} runtime/resource files; manifest: {manifest}")


if __name__ == "__main__":
    stage()
