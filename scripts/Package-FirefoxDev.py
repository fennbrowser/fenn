"""Build a Windows 11 development MSIX from a clean Firefox package.

The package depends on the shared Windows App SDK framework package and carries
no private copy of its runtime. It is signed with the self-signed certificate
that Initialize-DevSigning.ps1 creates and trusts once; --unsigned builds the
elevation-only unsigned fallback instead. With --loose, the same layout is
registered in place under Developer Mode instead of packed, as a separate
identity. Run via Build-FirefoxDevPackage.ps1, which selects Mach's build
Python environment.
"""

import argparse
import configparser
from dataclasses import dataclass
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
# A signed package's publisher must equal its certificate subject.
SIGNED_PUBLISHER = "CN=Firefox WinUI Development"
# Microsoft's unsigned-developer namespace: installable only with -AllowUnsigned.
UNSIGNED_PUBLISHER = ("CN=Firefox WinUI Development, "
                      "OID.2.25.311729368913984317654407730594956997722=1")
CODE_SIGNING_EKU = "1.3.6.1.5.5.7.3.3"
RUNTIME_FRAMEWORK = "Microsoft.WindowsAppRuntime.2"
RUNTIME_PUBLISHER = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"
# Stock Firefox ships these app-local (widget/windows/moz.build). Packaged, the
# framework supplies them, so one runtime copy can load per process.
FIREFOX_RUNTIME = (
    "CoreMessagingXP.dll", "marshal.dll", "Microsoft.InputStateManager.dll",
    "Microsoft.Internal.FrameworkUdk.dll", "Microsoft.UI.Composition.OSSupport.dll",
    "Microsoft.UI.Input.dll", "Microsoft.UI.Windowing.Core.dll", "Microsoft.UI.Windowing.dll",
    "Microsoft.WindowsAppRuntime.dll", "Microsoft.WindowsAppRuntime.Insights.Resource.dll",
)
NS = {
    "f": "http://schemas.microsoft.com/appx/manifest/foundation/windows10",
    "uap": "http://schemas.microsoft.com/appx/manifest/uap/windows10",
    "desktop": "http://schemas.microsoft.com/appx/manifest/desktop/windows10",
}


@dataclass(frozen=True)
class Lane:
    identity: str
    publisher: str
    display_name: str
    alias: str
    runs_dir: Path
    signed: bool = False


# Packed, signed with the trusted development certificate, and installed or
# updated without elevation by Install-FirefoxDevPackage.ps1.
DEV = Lane(IDENTITY, SIGNED_PUBLISHER, DISPLAY_NAME, "firefox-winui-dev.exe",
           ROOT / "artifacts/msix-dev", signed=True)
# Fallback for machines without the certificate, such as a fresh test guest:
# packed unsigned and installed elevated with -AllowUnsigned. Its publisher makes
# it a different package family from DEV, so only one of the two can be installed.
UNSIGNED = Lane(IDENTITY, UNSIGNED_PUBLISHER, DISPLAY_NAME, "firefox-winui-dev.exe",
                ROOT / "artifacts/msix-dev")
# Registered in place from its layout under Developer Mode: no pack, signature or
# elevation. Windows refuses loose registration for the unsigned publisher OID,
# so this lane has an ordinary publisher, and with it its own identity, alias,
# Start entry and profile.
LOOSE = Lane("FirefoxWinUI.DevLoose", SIGNED_PUBLISHER, "WinUI Browser Dev (Loose)",
             "firefox-winui-loose.exe", ROOT / "artifacts/msix-loose")


def version_string(value):
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\.0", value):
        raise argparse.ArgumentTypeError("Use a four-part version such as 0.1.0.0")
    if any(int(part) > 65535 for part in value.split(".")) or value == "0.0.0.0":
        raise argparse.ArgumentTypeError("Version components must be <= 65535 and version nonzero")
    return value


def firefox_version(app_dir):
    """The packaged Firefox's display version, such as 159.0a1."""
    config = configparser.ConfigParser()
    if not config.read(app_dir / "application.ini", encoding="utf-8"):
        raise FileNotFoundError(app_dir / "application.ini")
    return config["App"]["Version"]


def firefox_series(version):
    """Firefox's major and minor numbers: 159.0a1, 159.0b3 and 159.0.1 are all (159, 0)."""
    match = re.fullmatch(r"(\d+)\.(\d+)(?:\.\d+)?(?:(?:a|b|rc)\d+|esr)?", version)
    if not match or any(int(part) > 65535 for part in match.groups()):
        raise ValueError(f"Unrecognized Firefox version: {version}")
    return tuple(int(part) for part in match.groups())


def next_version(series, *known):
    """The next package of this Firefox series: <major>.<minor>.<build>.0.

    The build number continues past every installed or built package of the same
    series and restarts at 1 for a new one. The fourth part stays 0, as upstream
    requires for MSIX tooling.
    """
    builds = [version_key(value)[2] for value in known if value and version_key(value)[:2] == series]
    build = max(builds, default=0) + 1
    if build > 65535:
        raise ValueError(f"No build number left for Firefox {series[0]}.{series[1]}; pass --version")
    return f"{series[0]}.{series[1]}.{build}.0"


def installed_version(lane=DEV):
    query = (f"Get-AppxPackage -Name {lane.identity} | Where-Object {{ $_.Architecture -eq 'X64' -and "
             f"$_.Publisher -eq '{lane.publisher}' }} | ForEach-Object {{ $_.Version }}")
    output = subprocess.run(["powershell", "-NoProfile", "-Command", query],
                            check=True, capture_output=True, text=True).stdout.split()
    return max(output, key=version_key) if output else None


def built_versions(runs_dir):
    pattern = re.compile(rf"{re.escape(IDENTITY)}_(\d+\.\d+\.\d+\.\d+)_x64\.msix")
    return [match[1] for path in runs_dir.glob(f"*/{IDENTITY}_*_x64.msix")
            if (match := pattern.fullmatch(path.name))]


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def runtime_min_version():
    """The framework floor is the Windows App SDK the pane is built against."""
    config = ET.parse(ROOT / "native/WinUIPane/packages.config")
    version = config.find("package[@id='Microsoft.WindowsAppSDK']").get("version")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError(f"Unexpected Windows App SDK version: {version}")
    return f"{version}.0"


def version_key(value):
    return tuple(int(part) for part in value.split("."))


def installed_framework(min_version):
    """The newest installed x64 framework at or above the floor, for validation."""
    query = (f"Get-AppxPackage -Name {RUNTIME_FRAMEWORK} | Where-Object Architecture -eq X64 | "
             "Select-Object PackageFullName,Version,Publisher,InstallLocation | ConvertTo-Json")
    output = subprocess.run(["powershell", "-NoProfile", "-Command", query],
                            check=True, capture_output=True, text=True).stdout.strip()
    packages = json.loads(output) if output else []
    packages = [packages] if isinstance(packages, dict) else packages
    usable = [p for p in packages if p["Publisher"] == RUNTIME_PUBLISHER
              and version_key(p["Version"]) >= version_key(min_version)]
    if not usable:
        raise FileNotFoundError(f"Install the x64 {RUNTIME_FRAMEWORK} framework >= {min_version} "
                                "(Windows App Runtime) to build and run this package")
    return max(usable, key=lambda p: version_key(p["Version"]))


def framework_record(framework, firefox_dir, pane_files, pane_dir, image_digest):
    """Prove the framework supplies every runtime file the package no longer carries."""
    location = Path(framework["InstallLocation"])
    available = {p.relative_to(location).as_posix().lower(): p
                 for p in location.rglob("*") if p.is_file()}
    sources = [(name, firefox_dir / name) for name in FIREFOX_RUNTIME]
    sources += [(p.relative_to(pane_dir).as_posix(), p) for p in pane_files]
    records = {}
    for relative, local in sources:
        if relative.lower() in records:
            continue
        supplied = available.get(relative.lower())
        if not supplied:
            raise ValueError(f"{framework['PackageFullName']} lacks runtime file: {relative}")
        records[relative.lower()] = {
            "file": relative,
            "imageMatch": local.suffix.lower() != ".dll" or image_digest(local) == image_digest(supplied)}
    return {"framework": framework, "files": sorted(records.values(), key=lambda r: r["file"].lower())}


WINRT_FILE = re.compile(r"<asmv3:file\b[^>]*\bname=\"([^\"]+)\"[^>]*>(?:(?!</asmv3:file>).)*?"
                        r"winrtv1:activatableClass.*?</asmv3:file>\s*", re.S)


def strip_winrt_registrations(text, supplied):
    """Drop executable reg-free WinRT entries for framework-supplied servers.

    The unpackaged firefox.exe registers WinUI classes against app-local DLLs.
    Those entries take precedence over the package graph, so the packaged
    executable must not carry them; the framework registers the same classes
    process-wide, including for incoming COM/UIA calls.
    """
    removed = WINRT_FILE.findall(text)
    if not removed:
        raise ValueError("firefox.exe manifest has no WinUI registrations to replace")
    unsupplied = sorted(name for name in removed if name.lower() not in supplied)
    if unsupplied:
        raise ValueError(f"Framework does not supply registered WinRT servers: {unsupplied}")
    stripped = WINRT_FILE.sub("", text)
    if "activatableClass" in stripped:
        raise ValueError("Unrecognized WinRT registration left in firefox.exe manifest")
    ET.fromstring(stripped.encode("utf-8"))
    return stripped, removed


def replace_exe_manifest(mt, exe, work, supplied):
    extracted, replacement = work / "firefox.exe.manifest", work / "firefox.exe.packaged.manifest"
    subprocess.run([str(mt), "-nologo", f"-inputresource:{exe};#1", f"-out:{extracted}"], check=True)
    stripped, removed = strip_winrt_registrations(extracted.read_text(encoding="utf-8-sig"), supplied)
    replacement.write_text(stripped, encoding="utf-8")
    subprocess.run([str(mt), "-nologo", "-manifest", str(replacement), f"-outputresource:{exe};#1"], check=True)
    subprocess.run([str(mt), "-nologo", f"-inputresource:{exe};#1", f"-out:{extracted}"], check=True)
    if "activatableClass" in extracted.read_text(encoding="utf-8-sig"):
        raise ValueError("Packaged firefox.exe still registers WinRT classes")
    return removed


def customize_manifest(path, min_version, description=DISPLAY_NAME, lane=DEV):
    for _, (prefix, uri) in ET.iterparse(path, events=["start-ns"]):
        ET.register_namespace(prefix, uri)
    tree = ET.parse(path)
    manifest = tree.getroot()
    manifest.find("f:Properties/f:DisplayName", NS).text = lane.display_name
    manifest.find("f:Properties/f:Description", NS).text = description
    dependencies = manifest.find("f:Dependencies", NS)
    dependencies.find("f:TargetDeviceFamily", NS).set("MinVersion", "10.0.22000.0")
    if dependencies.find("f:PackageDependency", NS) is not None:
        raise ValueError("Upstream package dependency contract changed")
    dependency = ET.SubElement(dependencies, f"{{{NS['f']}}}PackageDependency", {
        "Name": RUNTIME_FRAMEWORK, "MinVersion": min_version, "Publisher": RUNTIME_PUBLISHER})
    dependency.tail = dependencies[-2].tail
    dependencies[-2].tail = dependencies.text
    aliases = manifest.findall(".//desktop:ExecutionAlias", NS)
    if len(aliases) != 1:
        raise ValueError("Upstream execution alias contract changed")
    aliases[0].set("Alias", lane.alias)
    tree.write(path, encoding="utf-8", xml_declaration=True)


def validate_package(path, version, expected_hashes, min_version, runtime_files=(), lane=DEV):
    with zipfile.ZipFile(path) as archive:
        names = {unquote(name).replace("\\", "/"): name for name in archive.namelist()}
        return validate_contents(names, archive.read, version, expected_hashes, min_version,
                                 runtime_files, lane)


def validate_layout(layout, version, expected_hashes, min_version, runtime_files=(), lane=DEV):
    names = {p.relative_to(layout).as_posix(): p for p in layout.rglob("*") if p.is_file()}
    return validate_contents(names, Path.read_bytes, version, expected_hashes, min_version,
                             runtime_files, lane)


def validate_contents(names, read, version, expected_hashes, min_version, runtime_files, lane):
    """Check a package archive or registrable layout; names maps member paths to read() keys."""
    manifest = ET.fromstring(read(names["AppxManifest.xml"]))
    identity = manifest.find("f:Identity", NS).attrib
    expected = {"Name": lane.identity, "Publisher": lane.publisher,
                "Version": version, "ProcessorArchitecture": "x64"}
    if any(identity.get(key) != value for key, value in expected.items()):
        raise ValueError(f"Unexpected package identity: {identity}")
    dependencies = [d.attrib for d in manifest.findall("f:Dependencies/f:PackageDependency", NS)]
    if dependencies != [{"Name": RUNTIME_FRAMEWORK, "MinVersion": min_version,
                         "Publisher": RUNTIME_PUBLISHER}]:
        raise ValueError(f"Package must depend only on {RUNTIME_FRAMEWORK} {min_version}: {dependencies}")
    aliases = manifest.findall(".//desktop:ExecutionAlias", NS)
    if [entry.get("Alias") for entry in aliases] != [lane.alias]:
        raise ValueError(f"Development package must not claim the firefox.exe alias; expected {lane.alias}")
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
        if hashlib.sha256(read(names[f"{base}/{relative}"])).hexdigest() != digest:
            raise ValueError(f"Packaged file differs from staged payload: {relative}")
    # The package root keeps Firefox's own Resources.pri; only the former
    # root copy of the WinUI loose assets would duplicate the framework there.
    lowered = {name.lower() for name in names}
    candidates = [f"{base}/{relative}" for relative in runtime_files]
    candidates += [name for name in names if name.lower().startswith("microsoft.ui.xaml/")]
    for candidate in candidates:
        if candidate.lower() in lowered:
            raise ValueError(f"Private Windows App SDK runtime file in package: {candidate}")
    forbidden = {"firefoxwinuipanetest.dll", "panecontracttests.exe", "user.js"}
    if any(PurePosixPath(name).name.lower() in forbidden for name in names):
        raise ValueError("Test binary or developer profile leaked into package")
    if ("AppxSignature.p7x" in names) != lane.signed:
        raise ValueError("This lane's package must be signed" if lane.signed
                         else "This development lane must remain unsigned")
    return {"identity": identity, "applicationId": "App", "payloadFiles": len(expected_hashes),
            "dependencies": dependencies}


DEFERRED_MAKEAPPX = "deferred-makeappx"


def stage_upstream_layout(msix_module, **kwargs):
    """Run upstream's repackager up to its pack step and return the staged layout.

    Upstream packs its staged directory immediately. This package still edits the
    manifest afterwards, so packing there would mean a second unpack and pack of
    the whole payload. Its makeappx calls go through a stand-in that records the
    staged directory instead; every other subprocess call is unchanged.
    """
    real = msix_module.subprocess
    staged = []

    def run(args, *rest, **options):
        if args == [DEFERRED_MAKEAPPX]:  # Upstream's MakeAppx flavor probe.
            return subprocess.CompletedProcess(args, 0, stdout="MakeAppx Tool (deferred)", stderr="")
        return real.run(args, *rest, **options)

    def check_output(args, *rest, **options):
        if args[:2] == [DEFERRED_MAKEAPPX, "pack"]:
            staged.append(Path(args[args.index("/d") + 1]))
            return ""
        return real.check_output(args, *rest, **options)

    shim = type(real)("deferred_subprocess")
    shim.__dict__.update(real.__dict__)
    shim.run, shim.check_output = run, check_output
    msix_module.subprocess = shim
    try:
        msix_module.repackage_msix(makeappx=DEFERRED_MAKEAPPX, **kwargs)
    finally:
        msix_module.subprocess = real
    if len(staged) != 1 or not (staged[0] / "AppxManifest.xml").is_file():
        raise RuntimeError("Upstream repackager no longer packs its staged layout through makeappx; "
                           "update stage_upstream_layout")
    return staged[0]


def take_layout(staged, layout):
    """Move upstream's staging directory into the run; it is rebuilt in full next time."""
    try:
        staged.rename(layout)
    except OSError:  # Different volume.
        shutil.move(staged, layout)


def signing_certificate(publisher):
    """The newest valid code-signing certificate for publisher in CurrentUser\\My,
    and whether its public certificate is trusted in LocalMachine\\TrustedPeople."""
    script = (f"Get-ChildItem Cert:\\CurrentUser\\My | Where-Object {{ $_.Subject -eq '{publisher}' -and "
              f"$_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) -and "
              f"$_.EnhancedKeyUsageList.ObjectId -contains '{CODE_SIGNING_EKU}' }} | "
              "Sort-Object NotAfter -Descending | Select-Object -First 1 | ForEach-Object { "
              "ConvertTo-Json -Compress @{ Thumbprint = $_.Thumbprint; NotAfter = $_.NotAfter.ToString('o'); "
              "Trusted = Test-Path ('Cert:\\LocalMachine\\TrustedPeople\\' + $_.Thumbprint) } }")
    output = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                            check=True, capture_output=True, text=True).stdout.strip()
    if not output:
        raise FileNotFoundError(f"No code-signing certificate for {publisher} in CurrentUser\\My. Run "
                                "scripts\\Initialize-DevSigning.ps1 once from an elevated PowerShell, "
                                "or build with -Unsigned")
    return json.loads(output)


def sign_package(signtool, package, thumbprint):
    subprocess.run([str(signtool), "sign", "/q", "/fd", "SHA256", "/s", "My", "/sha1", thumbprint,
                    str(package)], check=True)


def developer_mode_enabled():
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock") as key:
            return winreg.QueryValueEx(key, "AllowDevelopmentWithoutDevLicense")[0] == 1
    except OSError:
        return False


def registration(lane):
    """The lane's registered package: version, location and processes running from it."""
    script = f"""
$p = Get-AppxPackage -Name {lane.identity} | Where-Object Publisher -eq '{lane.publisher}' | Select-Object -First 1
if ($p) {{
    $root = $p.InstallLocation.TrimEnd('\\') + '\\'
    $running = @(Get-Process | Where-Object {{ $_.Path -and
        $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) }} | ForEach-Object Id)
    ConvertTo-Json -Compress @{{ Version = $p.Version; InstallLocation = $p.InstallLocation; Running = $running }}
}}"""
    output = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                            check=True, capture_output=True, text=True).stdout.strip()
    return json.loads(output) if output else None


def register_layout(layout, lane):
    """Register the layout in place. A newer version from a new directory is an
    update, so package data and the browser profile carry over."""
    manifest = layout / "AppxManifest.xml"
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Add-AppxPackage -Register '{manifest}' -ErrorAction Stop"], check=True)
    state = registration(lane)
    if not state or Path(state["InstallLocation"]).resolve() != layout.resolve():
        raise RuntimeError(f"{lane.identity} is not registered from {layout}: {state}")
    return state


def remove_superseded_runs(lane, current):
    """Earlier loose layouts are no longer referenced once the update registers."""
    for run in lane.runs_dir.iterdir():
        if run.is_dir() and run != current:
            try:
                shutil.rmtree(run)
            except OSError as error:
                print(f"Warning: could not remove superseded layout {run}: {error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", type=version_string,
                        help="default: one build step past the installed and newest built versions")
    parser.add_argument("--sdk-bin", required=True, type=Path)
    lanes = parser.add_mutually_exclusive_group()
    lanes.add_argument("--loose", action="store_true",
                       help=f"register the layout as {LOOSE.identity} under Developer Mode instead of packing")
    lanes.add_argument("--unsigned", action="store_true",
                       help="build the unsigned fallback, installable only elevated with -AllowUnsigned")
    parser.add_argument("--keep-intermediates", action="store_true",
                        help="keep payload/ (and, when packing, layout/) after a successful build")
    args = parser.parse_args()
    lane = LOOSE if args.loose else UNSIGNED if args.unsigned else DEV
    if lane.signed:
        certificate = signing_certificate(lane.publisher)
        if not certificate["Trusted"]:
            print(f"Warning: certificate {certificate['Thumbprint']} is not in LocalMachine\\TrustedPeople; "
                  "installing will fail until Initialize-DevSigning.ps1 trusts it")
    if args.loose:
        if not developer_mode_enabled():
            raise RuntimeError("The loose lane needs Windows Developer Mode (Settings > System > For developers)")
        state = registration(LOOSE)
        if state and state["Running"]:
            raise RuntimeError(f"Close {LOOSE.display_name} first; running process IDs: {state['Running']}")
    makeappx, mt, signtool = (args.sdk_bin / name for name in ("makeappx.exe", "mt.exe", "signtool.exe"))
    clean = ROOT / "build/firefox/dist/firefox"
    msix_server = ROOT / "build/firefox/dist/bin/msixcomserver.dll"
    pane_dir = ROOT / "build/winui-pane/x64/Release"
    for required in (makeappx, mt, signtool, clean / "firefox.exe", clean / "omni.ja", msix_server,
                     pane_dir / "FirefoxWinUIPane.dll",
                     *(clean / name for name in FIREFOX_RUNTIME)):
        if not required.is_file():
            raise FileNotFoundError(f"Missing build prerequisite: {required}")
    staging = load_script("Stage-WinUIPane")
    pane_runtime = staging.runtime_files(pane_dir)
    min_version = runtime_min_version()
    framework = installed_framework(min_version)
    record = framework_record(framework, clean, pane_runtime, pane_dir, staging.image_digest)
    browser_version = firefox_version(clean)
    installed = installed_version(lane)
    if args.version is None:
        # The loose lane's registration is its only record; its runs are superseded.
        built = [] if args.loose else built_versions(DEV.runs_dir)
        args.version = next_version(firefox_series(browser_version), installed, *built)
        print(f"Version {args.version}: Firefox {browser_version} package build "
              f"{version_key(args.version)[2]}; installed {installed or 'none'}")
    if installed and version_key(args.version) <= version_key(installed):
        print(f"Warning: {args.version} is not newer than installed {installed}; "
              "Windows cannot update to it")

    run = lane.runs_dir / f"{args.version}-{uuid.uuid4().hex[:8]}"
    run.mkdir(parents=True)
    (run / "runtime-framework.json").write_text(
        json.dumps({"minVersion": min_version, **record}, indent=2) + "\n", encoding="utf-8")
    payload = run / "payload"
    shutil.copytree(clean, payload, ignore=lambda directory, names: [
        name for name in names if Path(directory) == clean and name in FIREFOX_RUNTIME])
    # This pinned Firefox ZIP manifest omits the MSIX-only background COM server.
    shutil.copyfile(msix_server, payload / msix_server.name)
    shutil.copyfile(pane_dir / "FirefoxWinUIPane.dll", payload / "FirefoxWinUIPane.dll")
    supplied = {p.name.lower() for p in Path(framework["InstallLocation"]).iterdir() if p.is_file()}
    removed_registrations = replace_exe_manifest(mt, payload / "firefox.exe", run, supplied)
    defaults = payload / "browser/defaults/preferences/firefox-winui-dev.js"
    defaults.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "packaging/dev/defaults.js", defaults)
    expected_hashes = {p.relative_to(payload).as_posix(): sha256(p)
                       for p in sorted(payload.rglob("*")) if p.is_file()}

    from mozbuild.repackaging import msix

    def log(level, action, params, message):
        print(message.format(**params), flush=True)

    output = run / f"{lane.identity}_{args.version}_x64.msix"
    # Upstream names the VFS install directory after displayname; keep it the same
    # in both lanes and set each lane's Start display name in the manifest.
    staged = stage_upstream_layout(
        msix, dir_or_package=str(payload), topsrcdir=str(ROOT / "upstream/firefox"),
        channel="unofficial", distribution_dirs=[], version=args.version, vendor="FirefoxWinUI",
        displayname=DISPLAY_NAME, identity=lane.identity, publisher=lane.publisher,
        publisher_display_name="Firefox WinUI Development", arch="x86_64",
        output=str(output), force=True, log=log)
    layout = run / "layout"
    take_layout(staged, layout)
    customize_manifest(layout / "AppxManifest.xml", min_version,
                       f"{lane.display_name} (Firefox {browser_version}, package {args.version})", lane)
    shutil.copyfile(layout / "AppxManifest.xml", run / "AppxManifest.xml")
    runtime_names = [entry["file"] for entry in record["files"]]
    common = {"firefoxVersion": browser_version, "runtimeFramework": framework["PackageFullName"],
              "removedExeWinRTServers": removed_registrations, "payloadSha256": expected_hashes}

    if args.loose:
        result = validate_layout(layout, args.version, expected_hashes, min_version, runtime_names, lane)
        register_layout(layout, lane)
        result.update({"layout": str(layout), "registration": "Developer Mode loose layout", **common})
        (run / "package-report.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        if not args.keep_intermediates:
            shutil.rmtree(payload)
        remove_superseded_runs(lane, run)
        print(f"Registered {lane.display_name} {args.version} from {layout}")
        print(f"Launch it from Start or with {lane.alias}")
        return

    subprocess.run([str(makeappx), "pack", "/d", str(layout), "/p", str(output)], check=True)
    if lane.signed:
        sign_package(signtool, output, certificate["Thumbprint"])
        signing = {"signing": "self-signed development certificate", "certificate": certificate}
    else:
        signing = {"signing": "Windows 11 AllowUnsigned development only"}
    result = validate_package(output, args.version, expected_hashes, min_version, runtime_names, lane)
    result.update({"package": str(output), "sha256": sha256(output),
                   "bytes": output.stat().st_size, **signing, **common})
    (run / "package-report.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    # Validation compared the archive with payload hashes; the intermediates are
    # regenerable and several hundred MB each. A failed build keeps them.
    if not args.keep_intermediates:
        for directory in (payload, layout):
            shutil.rmtree(directory)
    print(f"Validated development MSIX: {output}")
    print(f"SHA-256: {result['sha256']}")


if __name__ == "__main__":
    main()
