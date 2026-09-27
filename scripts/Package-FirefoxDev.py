"""Build a Windows 11 unsigned development MSIX from a clean Firefox package.

Run via Build-FirefoxDevPackage.ps1, which selects Mach's build Python environment.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import uuid
from urllib.parse import unquote
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
IDENTITY = "FirefoxWinUI.Dev"
DISPLAY_NAME = "WinUI Browser Dev"
PUBLISHER = ("CN=Firefox WinUI Development, "
             "OID.2.25.311729368913984317654407730594956997722=1")
NS = {
    "f": "http://schemas.microsoft.com/appx/manifest/foundation/windows10",
    "uap": "http://schemas.microsoft.com/appx/manifest/uap/windows10",
    "desktop": "http://schemas.microsoft.com/appx/manifest/desktop/windows10",
}


def version_string(value):
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\.0", value):
        raise argparse.ArgumentTypeError("Use a four-part version such as 0.1.0.0")
    if any(int(part) > 65535 for part in value.split(".")) or value == "0.0.0.0":
        raise argparse.ArgumentTypeError("Version components must be <= 65535 and version nonzero")
    return value


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def customize_manifest(path):
    for _, (prefix, uri) in ET.iterparse(path, events=["start-ns"]):
        ET.register_namespace(prefix, uri)
    tree = ET.parse(path)
    manifest = tree.getroot()
    for name in ("DisplayName", "Description"):
        manifest.find(f"f:Properties/f:{name}", NS).text = DISPLAY_NAME
    manifest.find("f:Dependencies/f:TargetDeviceFamily", NS).set("MinVersion", "10.0.22000.0")
    aliases = manifest.findall(".//desktop:ExecutionAlias", NS)
    if len(aliases) != 1:
        raise ValueError("Upstream execution alias contract changed")
    aliases[0].set("Alias", "firefox-winui-dev.exe")
    tree.write(path, encoding="utf-8", xml_declaration=True)


def validate_pri_layout(layout, dump):
    for candidate in ET.parse(dump).findall(".//Candidate[@type='Path']/Value"):
        relative = PurePosixPath(candidate.text.replace("\\", "/"))
        if relative.is_absolute() or ".." in relative.parts or not (layout / relative).is_file():
            raise ValueError(f"PRI references missing package-root resource: {candidate.text}")


def validate_package(path, version, expected_hashes, root_hashes=None):
    with zipfile.ZipFile(path) as archive:
        names = {unquote(name).replace("\\", "/"): name for name in archive.namelist()}
        manifest = ET.fromstring(archive.read(names["AppxManifest.xml"]))
        identity = manifest.find("f:Identity", NS).attrib
        expected = {"Name": IDENTITY, "Publisher": PUBLISHER,
                    "Version": version, "ProcessorArchitecture": "x64"}
        if any(identity.get(key) != value for key, value in expected.items()):
            raise ValueError(f"Unexpected package identity: {identity}")
        aliases = manifest.findall(".//desktop:ExecutionAlias", NS)
        if [entry.get("Alias") for entry in aliases] != ["firefox-winui-dev.exe"]:
            raise ValueError("Development package must not claim the firefox.exe alias")
        for element in manifest.iter():
            for key in ("Executable", "Path"):
                value = element.get(key)
                if value and value.replace("\\", "/") not in names:
                    raise ValueError(f"Manifest references missing payload: {value}")
            if element.tag.endswith("}Path") and element.text:
                if element.text.replace("\\", "/") not in names:
                    raise ValueError(f"Manifest references missing payload: {element.text}")
        app = manifest.find("f:Applications/f:Application", NS)
        base = str(PurePosixPath(app.get("Executable").replace("\\", "/")).parent)
        for relative, digest in expected_hashes.items():
            if hashlib.sha256(archive.read(names[f"{base}/{relative}"])).hexdigest() != digest:
                raise ValueError(f"Packaged file differs from staged payload: {relative}")
        for relative, digest in (root_hashes or {}).items():
            if hashlib.sha256(archive.read(names[relative])).hexdigest() != digest:
                raise ValueError(f"Packaged resource differs from layout: {relative}")
        forbidden = {"firefoxwinuipanetest.dll", "panecontracttests.exe", "user.js"}
        if any(PurePosixPath(name).name.lower() in forbidden for name in names):
            raise ValueError("Test binary or developer profile leaked into package")
        if "AppxSignature.p7x" in names:
            raise ValueError("This development lane must remain unsigned")
        return {"identity": identity, "applicationId": "App", "payloadFiles": len(expected_hashes)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, type=version_string)
    parser.add_argument("--sdk-bin", required=True, type=Path)
    args = parser.parse_args()
    makeappx, makepri = (args.sdk_bin / name for name in ("makeappx.exe", "makepri.exe"))
    clean = ROOT / "build/firefox/dist/firefox"
    msix_server = ROOT / "build/firefox/dist/bin/msixcomserver.dll"
    for required in (makeappx, makepri, clean / "firefox.exe", clean / "omni.ja",
                     msix_server,
                     ROOT / "build/winui-pane/x64/Release/FirefoxWinUIPane.dll"):
        if not required.is_file():
            raise FileNotFoundError(f"Missing build prerequisite: {required}")
    run = ROOT / "artifacts/msix-dev" / f"{args.version}-{uuid.uuid4().hex[:8]}"
    run.mkdir(parents=True)
    payload = run / "payload"
    shutil.copytree(clean, payload)
    # This pinned Firefox ZIP manifest omits the MSIX-only background COM server.
    shutil.copyfile(msix_server, payload / msix_server.name)
    spec = importlib.util.spec_from_file_location("pane_staging", ROOT / "scripts/Stage-WinUIPane.py")
    staging = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(staging)
    staging.stage(dest_dir=payload, manifest=run / "runtime-staging.json")
    defaults = payload / "browser/defaults/preferences/firefox-winui-dev.js"
    defaults.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "packaging/dev/defaults.js", defaults)
    expected_hashes = {p.relative_to(payload).as_posix(): sha256(p)
                       for p in sorted(payload.rglob("*")) if p.is_file()}

    from mozbuild.repackaging.msix import repackage_msix

    def log(level, action, params, message):
        print(message.format(**params), flush=True)

    base = run / "upstream.msix"
    repackage_msix(str(payload), str(ROOT / "upstream/firefox"), channel="unofficial",
                   distribution_dirs=[], version=args.version, vendor="FirefoxWinUI",
                   displayname=DISPLAY_NAME, identity=IDENTITY, publisher=PUBLISHER,
                   publisher_display_name="Firefox WinUI Development", arch="x86_64",
                   output=str(base), force=True, log=log, makeappx=str(makeappx))
    layout = run / "layout"
    subprocess.run([str(makeappx), "unpack", "/p", str(base), "/d", str(layout)], check=True)
    customize_manifest(layout / "AppxManifest.xml")

    # Packaged XAML resolves resources from the package root, not just the EXE directory.
    pri_input = run / "pri-input"
    pri_input.mkdir()
    shutil.copyfile(layout / "AppxManifest.xml", pri_input / "AppxManifest.xml")
    shutil.copyfile(layout / "Resources.pri", pri_input / "firefox.pri")
    shutil.copyfile(payload / "resources.pri", pri_input / "winui.pri")
    subprocess.run([str(makepri), "new", "/pr", str(pri_input), "/cf",
                    str(ROOT / "configs/winui-pane-pri.xml"), "/of",
                    str(layout / "Resources.pri"), "/o"], check=True)
    shutil.copytree(payload / "Microsoft.UI.Xaml", layout / "Microsoft.UI.Xaml")
    pri_dump = run / "root-pri.xml"
    subprocess.run([str(makepri), "dump", "/if", str(layout / "Resources.pri"),
                    "/of", str(pri_dump)], check=True)
    validate_pri_layout(layout, pri_dump)
    root_hashes = {p.relative_to(layout).as_posix(): sha256(p)
                   for p in sorted((layout / "Microsoft.UI.Xaml").rglob("*")) if p.is_file()}
    root_hashes["Resources.pri"] = sha256(layout / "Resources.pri")
    output = run / f"{IDENTITY}_{args.version}_x64.msix"
    subprocess.run([str(makeappx), "pack", "/d", str(layout), "/p", str(output)], check=True)
    result = validate_package(output, args.version, expected_hashes, root_hashes)
    result.update({"package": str(output), "sha256": sha256(output),
                   "bytes": output.stat().st_size, "signing": "Windows 11 AllowUnsigned development only",
                   "payloadSha256": expected_hashes, "rootResourceSha256": root_hashes})
    (run / "package-report.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Validated development MSIX: {output}")
    print(f"SHA-256: {result['sha256']}")


if __name__ == "__main__":
    main()
