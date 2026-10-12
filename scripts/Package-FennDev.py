"""Build a Windows 11 development MSIX from a clean Firefox package.

The package depends on the shared Windows App SDK framework package and carries
no private copy of its runtime. It is signed with the self-signed certificate
that Initialize-DevSigning.ps1 creates and trusts once; --unsigned builds the
elevation-only unsigned fallback instead. With --loose, the same layout is
registered in place under Developer Mode instead of packed, as a separate
identity. Run via Build-FennDevPackage.ps1, which selects Mach's build
Python environment.
"""

import argparse
import configparser
from dataclasses import dataclass
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import uuid
from urllib.parse import unquote
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
# The browser executable is <APP_NAME>.exe, and `mach package` writes its clean
# payload to dist/<APP_NAME>; both mozconfigs set it (--with-app-name).
APP_NAME = "fenn"
IDENTITY = "Fenn.Dev"
DISPLAY_NAME = "Fenn Dev"
# A signed package's publisher must equal its certificate subject.
SIGNED_PUBLISHER = "CN=Fenn Development"
DEV_PUBLISHER_DISPLAY_NAME = "Fenn Development"
# Microsoft's unsigned-developer namespace: installable only with -AllowUnsigned.
UNSIGNED_PUBLISHER = ("CN=Fenn Development, "
                      "OID.2.25.311729368913984317654407730594956997722=1")
# The Store product's identity from Partner Center (Product identity page). The
# Store signs the package after certification, so this lane packs it unsigned.
STORE_IDENTITY = "FennBrowser.FennBrowser"
STORE_PUBLISHER = "CN=4EE000D9-3774-42FF-AC7F-E77E790B3C25"
STORE_PUBLISHER_DISPLAY_NAME = "Fenn Browser"
CODE_SIGNING_EKU = "1.3.6.1.5.5.7.3.3"
# Upstream's template still names Windows 11 22H2. Claim the SDK this package is
# built and tested with, so Windows applies current packaged-app behaviour.
MAX_VERSION_TESTED = "10.0.26100.0"
FEED_DIR = ROOT / "artifacts/msix-feed"
# Firefox object directories of configs/mozconfig and configs/mozconfig.release.
# Development builds iterate; the release build adds PGO and LTO. The Store
# package always comes from the release build, and a development lane takes it
# with --release-build, so the build that ships can be installed and tested.
DEV_OBJDIR = ROOT / "build/firefox"
RELEASE_OBJDIR = ROOT / "build/firefox-release"
APPINSTALLER_NS = "http://schemas.microsoft.com/appx/appinstaller/2021"
RUNTIME_FRAMEWORK = "Microsoft.WindowsAppRuntime.2"
RUNTIME_PUBLISHER = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"
# Firefox DLLs that share a file name with a framework DLL but are not the
# Windows App SDK runtime. Firefox loads them by full path, so the framework's
# copy cannot stand in: onnxruntime.dll (dom/onnx/InferenceSession.cpp, beside xul).
FIREFOX_SAME_NAME_DLLS = {"onnxruntime.dll"}
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
    publisher_display_name: str = DEV_PUBLISHER_DISPLAY_NAME
    # Upstream names the VFS install directory "<install_name> Package Root".
    install_name: str = DISPLAY_NAME
    # packaging/dev/defaults.js: development conveniences, never in a release.
    dev_defaults: bool = True
    store: bool = False


# Packed, signed with the trusted development certificate, and installed or
# updated without elevation by Install-FennDevPackage.ps1.
DEV = Lane(IDENTITY, SIGNED_PUBLISHER, DISPLAY_NAME, "fenn-dev.exe",
           ROOT / "artifacts/msix-dev", signed=True)
# Fallback for machines without the certificate, such as a fresh test guest:
# packed unsigned and installed elevated with -AllowUnsigned. Its publisher makes
# it a different package family from DEV, so only one of the two can be installed.
UNSIGNED = Lane(IDENTITY, UNSIGNED_PUBLISHER, DISPLAY_NAME, "fenn-dev.exe",
                ROOT / "artifacts/msix-dev")
# Registered in place from its layout under Developer Mode: no pack, signature or
# elevation. Windows refuses loose registration for the unsigned publisher OID,
# so this lane has an ordinary publisher, and with it its own identity, alias,
# Start entry and profile.
LOOSE = Lane("Fenn.DevLoose", SIGNED_PUBLISHER, "Fenn Dev (Loose)",
             "fenn-loose.exe", ROOT / "artifacts/msix-loose")
# The release package for upload to Partner Center. Its display name is "Fenn",
# one of the product's reserved names, as the Store requires. It claims the
# fenn.exe alias, and carries no development defaults.
STORE = Lane(STORE_IDENTITY, STORE_PUBLISHER, "Fenn", "fenn.exe", ROOT / "artifacts/msix-store",
             publisher_display_name=STORE_PUBLISHER_DISPLAY_NAME, install_name="Fenn",
             dev_defaults=False, store=True)


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


def built_versions(runs_dir, identity=IDENTITY):
    pattern = re.compile(rf"{re.escape(identity)}_(\d+\.\d+\.\d+\.\d+)_x64\.msix")
    return [match[1] for path in runs_dir.glob(f"*/{identity}_*_x64.msix")
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


def framework_record(framework):
    """The framework the package depends on, and the runtime DLLs it supplies. None
    of them may appear in the package: patch 2 stops Firefox shipping its own
    app-local copies, and the pane is framework-dependent, so only the
    framework's copy can load."""
    location = Path(framework["InstallLocation"])
    return {"framework": framework,
            "files": sorted(p.name for p in location.iterdir() if p.is_file() and p.suffix.lower() == ".dll"
                            and p.name.lower() not in FIREFOX_SAME_NAME_DLLS)}


def check_exe_manifest(mt, exe, work):
    """The browser executable must register no WinRT classes: in a package they would take
    precedence over the framework in the package graph (WinUI activation then
    fails with 0x8007007E). Firefox's build no longer adds any."""
    extracted = work / f"{APP_NAME}.exe.manifest"
    subprocess.run([str(mt), "-nologo", f"-inputresource:{exe};#1", f"-out:{extracted}"],
                   check=True, stdout=subprocess.DEVNULL)
    if "activatableClass" in extracted.read_text(encoding="utf-8-sig"):
        raise ValueError(f"{exe} registers WinRT classes; rebuild Firefox from the current patch")


def pri_candidates(dump):
    """(resource, qualifiers, value) for every file candidate in a MakePri dump."""
    return {(resource.get("name"), candidate.get("qualifiers", ""), candidate.findtext("Value"))
            for resource in ET.parse(dump).getroot().iter("NamedResource")
            for candidate in resource.findall("Candidate[@type='Path']")}


def check_package_pri(dump, layout, identity, expected):
    """The package PRI must be this package's resource map, carry exactly the
    expected candidates, and every file it names must exist in the layout.
    MakePri reports success even when it merged nothing."""
    maps = [m.get("name") for m in ET.parse(dump).getroot().iter("ResourceMap")]
    if maps[:1] != [identity]:
        raise ValueError(f"Package PRI resource map is {maps}, expected {identity}")
    candidates = pri_candidates(dump)
    if not expected or candidates != expected:
        raise ValueError(f"Package PRI candidates differ from upstream: missing {sorted(expected - candidates)}, "
                         f"extra {sorted(candidates - expected)}")
    for _, _, value in candidates:
        relative = PurePosixPath(value.replace("\\", "/"))
        if relative.is_absolute() or ".." in relative.parts or not (layout / relative).is_file():
            raise ValueError(f"Package PRI references a missing file: {value}")


def reindex_package_pri(makepri, layout, work, identity):
    """Upstream ships a prebuilt Resources.pri whose map is named "firefox". Windows
    resolves the manifest's assets through the map named after the package, so
    re-index it under this package's identity, keeping every candidate."""
    def dump(pri, name):
        target = work / name
        subprocess.run([str(makepri), "dump", "/if", str(pri), "/of", str(target), "/o"],
                       check=True, stdout=subprocess.DEVNULL)
        return target

    expected = pri_candidates(dump(layout / "Resources.pri", "upstream-pri.xml"))
    source = work / "pri-input"
    source.mkdir()
    shutil.copyfile(layout / "Resources.pri", source / "upstream.pri")
    subprocess.run([str(makepri), "new", "/pr", str(source), "/cf", str(ROOT / "configs/msix-package-pri.xml"),
                    "/mn", str(layout / "AppxManifest.xml"), "/of", str(layout / "Resources.pri"), "/o"],
                   check=True, stdout=subprocess.DEVNULL)
    check_package_pri(dump(layout / "Resources.pri", "package-pri.xml"), layout, identity, expected)
    shutil.rmtree(source)


def customize_manifest(path, min_version, description=DISPLAY_NAME, lane=DEV):
    for _, (prefix, uri) in ET.iterparse(path, events=["start-ns"]):
        ET.register_namespace(prefix, uri)
    tree = ET.parse(path)
    manifest = tree.getroot()
    manifest.find("f:Properties/f:DisplayName", NS).text = lane.display_name
    manifest.find("f:Properties/f:Description", NS).text = description
    # Upstream's template paints tiles in Firefox's purple. Until Fenn has brand
    # assets, let Windows choose the tile colour.
    manifest.find(".//uap:VisualElements", NS).set("BackgroundColor", "transparent")
    dependencies = manifest.find("f:Dependencies", NS)
    family = dependencies.find("f:TargetDeviceFamily", NS)
    family.set("MinVersion", "10.0.22000.0")
    family.set("MaxVersionTested", MAX_VERSION_TESTED)
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
        raise ValueError(f"Package must claim only its own alias {lane.alias}, never firefox.exe or {APP_NAME}.exe")
    publisher_name = manifest.find("f:Properties/f:PublisherDisplayName", NS).text
    if publisher_name != lane.publisher_display_name:
        raise ValueError(f"PublisherDisplayName is {publisher_name!r}, expected {lane.publisher_display_name!r}")
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
    forbidden = {"fennpanetest.dll", "panecontracttests.exe", "user.js"}
    if not lane.dev_defaults:
        forbidden.add("fenn-dev.js")
    if any(PurePosixPath(name).name.lower() in forbidden for name in names):
        raise ValueError("Test binary, developer profile or development defaults leaked into package")
    if ("AppxSignature.p7x" in names) != lane.signed:
        raise ValueError("This lane's package must be signed" if lane.signed
                         else "This lane's package must remain unsigned")
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


FIREFOX_ZIP = re.compile(rf"{APP_NAME}-[^-]+\.[A-Za-z-]+\.win(32|64|64-aarch64)\.zip")


def firefox_objdir(lane, release_build=False):
    """The Firefox build a lane packages: always the release build for the Store."""
    return RELEASE_OBJDIR if lane.store or release_build else DEV_OBJDIR


def payload_status(dist, staged_names):
    """Whether `mach package` must refresh dist/<APP_NAME> before packaging.

    `mach package` writes the Firefox ZIP as its last step, so the clean payload
    is current when no dist/bin file (following its symlinks into the source
    tree) is newer than that ZIP. Staged pane files are ignored: the package
    takes the pane from its own build output, so pane-only changes never need
    a repackage. Returns (current, reason).
    """
    zips = [p for p in dist.glob(f"{APP_NAME}-*.zip") if FIREFOX_ZIP.fullmatch(p.name)]
    if not zips or not (dist / APP_NAME / f"{APP_NAME}.exe").is_file():
        return False, "no clean Firefox package yet"
    packaged = max(p.stat().st_mtime for p in zips)
    newest, newest_name = packaged, None
    root = dist / "bin"
    for directory, _, files in os.walk(root, followlinks=True):
        for name in files:
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root).replace("\\", "/").lower()
            if relative in staged_names:
                continue
            try:
                modified = os.stat(path).st_mtime
            except OSError:  # Dangling link: not packaged either.
                continue
            if modified > newest:
                newest, newest_name = modified, relative
    if newest_name:
        return False, f"dist/bin/{newest_name} changed after the last mach package"
    return True, f"dist/{APP_NAME} is current"


def staged_pane_names(staging):
    """Files Stage-WinUIPane.py puts in dist/bin; the package takes the pane from its build."""
    return {name.lower() for name in staging.STAGED}


def appinstaller_xml(feed_uri, package_uri, lane, version):
    """An App Installer feed for one package. Windows checks it on every launch and
    in the background, and installs a newer package without a prompt once the
    browser is closed; the dependency is not listed, so the Windows App Runtime
    must already be installed."""
    ET.register_namespace("", APPINSTALLER_NS)
    root = ET.Element(f"{{{APPINSTALLER_NS}}}AppInstaller", {"Version": version, "Uri": feed_uri})
    ET.SubElement(root, f"{{{APPINSTALLER_NS}}}MainPackage", {
        "Name": lane.identity, "Publisher": lane.publisher, "Version": version,
        "ProcessorArchitecture": "x64", "Uri": package_uri})
    settings = ET.SubElement(root, f"{{{APPINSTALLER_NS}}}UpdateSettings")
    ET.SubElement(settings, f"{{{APPINSTALLER_NS}}}OnLaunch", {
        "HoursBetweenUpdateChecks": "0", "ShowPrompt": "false", "UpdateBlocksActivation": "false"})
    ET.SubElement(settings, f"{{{APPINSTALLER_NS}}}AutomaticBackgroundTask")
    ET.indent(root)
    return ET.tostring(root, encoding="utf-8", xml_declaration=True) + b"\n"


def publish_feed(package, lane, version, feed_dir=FEED_DIR):
    """Copy the package into the feed and point the feed at it. The previous
    package stays for an install that may still be reading it."""
    if not lane.signed:
        raise ValueError("App Installer feeds need the signed lane")
    feed_dir.mkdir(parents=True, exist_ok=True)
    published = feed_dir / package.name
    shutil.copyfile(package, published)
    feed = feed_dir / f"{lane.identity}.appinstaller"
    staging = feed.with_suffix(".appinstaller.tmp")
    staging.write_bytes(appinstaller_xml(feed.resolve().as_uri(), published.resolve().as_uri(), lane, version))
    staging.replace(feed)
    keep = {published.name}
    older = sorted((p for p in feed_dir.glob(f"{lane.identity}_*_x64.msix") if p.name not in keep),
                   key=lambda p: version_key(p.name.split("_")[1]), reverse=True)
    for stale in older[1:]:
        stale.unlink()
    return feed


def write_latest(lane, entry):
    """What the last successful build produced, for Build-FennDevPackage.ps1."""
    (lane.runs_dir / "latest.json").write_text(json.dumps(
        {"identity": lane.identity, "alias": lane.alias, "loose": lane is LOOSE, "store": lane.store, **entry},
        indent=2) + "\n",
        encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload-status", action="store_true",
                        help="only report whether the clean payload needs `mach package` (exit 0 current, 10 stale)")
    parser.add_argument("--version", type=version_string,
                        help="default: one build step past the installed and newest built versions")
    parser.add_argument("--sdk-bin", type=Path)
    lanes = parser.add_mutually_exclusive_group()
    lanes.add_argument("--loose", action="store_true",
                       help=f"register the layout as {LOOSE.identity} under Developer Mode instead of packing")
    lanes.add_argument("--unsigned", action="store_true",
                       help="build the unsigned fallback, installable only elevated with -AllowUnsigned")
    lanes.add_argument("--store", action="store_true",
                       help=f"build the unsigned {STORE.identity} release package for Partner Center upload")
    parser.add_argument("--release-build", action="store_true",
                        help=f"package the PGO/LTO release build ({RELEASE_OBJDIR.name}); --store always does")
    parser.add_argument("--publish", action="store_true",
                        help=f"also publish the signed package to the App Installer feed in {FEED_DIR.name}")
    parser.add_argument("--keep-intermediates", action="store_true",
                        help="keep payload/ (and, when packing, layout/) after a successful build")
    args = parser.parse_args()
    lane = LOOSE if args.loose else UNSIGNED if args.unsigned else STORE if args.store else DEV
    objdir = firefox_objdir(lane, args.release_build)
    build_kind = "release" if objdir == RELEASE_OBJDIR else "development"
    if args.payload_status:
        current, reason = payload_status(objdir / "dist", staged_pane_names(load_script("Stage-WinUIPane")))
        print(f"Firefox {build_kind} payload {'current' if current else 'stale'}: {reason}")
        raise SystemExit(0 if current else 10)
    if args.sdk_bin is None:
        parser.error("--sdk-bin is required")
    if args.publish and lane is not DEV:
        parser.error("--publish needs the signed development lane (no --loose, --unsigned or --store)")
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
    makeappx, mt, signtool, makepri = (args.sdk_bin / name for name in
                                        ("makeappx.exe", "mt.exe", "signtool.exe", "makepri.exe"))
    clean = objdir / "dist" / APP_NAME
    msix_server = objdir / "dist/bin/msixcomserver.dll"
    pane_dir = ROOT / "build/winui-pane/x64/Release"
    if objdir == RELEASE_OBJDIR and not (objdir / "dist/bin" / f"{APP_NAME}.exe").is_file():
        raise FileNotFoundError(f"No release build in {objdir}; build it with FENN_MOZCONFIG=mozconfig.release "
                                "(see configs/mozconfig.release)")
    for required in (makeappx, mt, signtool, makepri, clean / f"{APP_NAME}.exe", clean / "omni.ja", msix_server,
                     pane_dir / "FennPane.dll"):
        if not required.is_file():
            raise FileNotFoundError(f"Missing build prerequisite: {required}")
    print(f"Packaging the Firefox {build_kind} build from {objdir}")
    min_version = runtime_min_version()
    framework = installed_framework(min_version)
    record = framework_record(framework)
    browser_version = firefox_version(clean)
    installed = installed_version(lane)
    if args.version is None:
        # The loose lane's registration is its only record; its runs are superseded.
        built = [] if args.loose else built_versions(lane.runs_dir, lane.identity)
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
    shutil.copytree(clean, payload)
    # This pinned Firefox ZIP manifest omits the MSIX-only background COM server.
    shutil.copyfile(msix_server, payload / msix_server.name)
    shutil.copyfile(pane_dir / "FennPane.dll", payload / "FennPane.dll")
    check_exe_manifest(mt, payload / f"{APP_NAME}.exe", run)
    if lane.dev_defaults:
        defaults = payload / "browser/defaults/preferences/fenn-dev.js"
        defaults.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "packaging/dev/defaults.js", defaults)
    expected_hashes = {p.relative_to(payload).as_posix(): sha256(p)
                       for p in sorted(payload.rglob("*")) if p.is_file()}

    from mozbuild.repackaging import msix

    def log(level, action, params, message):
        print(message.format(**params), flush=True)

    output = run / f"{lane.identity}_{args.version}_x64.msix"
    # Upstream names the VFS install directory after displayname; the development
    # lanes share one, and each lane's Start display name is set in the manifest.
    staged = stage_upstream_layout(
        msix, dir_or_package=str(payload), topsrcdir=str(ROOT / "upstream/firefox"),
        channel="unofficial", distribution_dirs=[], version=args.version, vendor="Fenn",
        displayname=lane.install_name, identity=lane.identity, publisher=lane.publisher,
        publisher_display_name=lane.publisher_display_name, arch="x86_64",
        output=str(output), force=True, log=log)
    layout = run / "layout"
    take_layout(staged, layout)
    description = ("Fenn Browser" if lane.store
                   else f"{lane.display_name} (Firefox {browser_version}, package {args.version})")
    customize_manifest(layout / "AppxManifest.xml", min_version, description, lane)
    shutil.copyfile(layout / "AppxManifest.xml", run / "AppxManifest.xml")
    reindex_package_pri(makepri, layout, run, lane.identity)
    runtime_names = record["files"]
    common = {"firefoxVersion": browser_version, "firefoxBuild": build_kind, "runtimeFramework": framework["PackageFullName"],
              "payloadSha256": expected_hashes}

    if args.loose:
        result = validate_layout(layout, args.version, expected_hashes, min_version, runtime_names, lane)
        register_layout(layout, lane)
        result.update({"layout": str(layout), "registration": "Developer Mode loose layout", **common})
        (run / "package-report.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        if not args.keep_intermediates:
            shutil.rmtree(payload)
        remove_superseded_runs(lane, run)
        write_latest(lane, {"version": args.version, "firefoxBuild": build_kind, "layout": str(layout)})
        print(f"Registered {lane.display_name} {args.version} from {layout}")
        print(f"Launch it from Start or with {lane.alias}")
        return

    subprocess.run([str(makeappx), "pack", "/d", str(layout), "/p", str(output)], check=True)
    if lane.signed:
        sign_package(signtool, output, certificate["Thumbprint"])
        signing = {"signing": "self-signed development certificate", "certificate": certificate}
    elif lane.store:
        signing = {"signing": "unsigned upload; the Microsoft Store signs it after certification"}
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
    latest = {"version": args.version, "firefoxBuild": build_kind, "package": str(output)}
    if args.publish:
        latest["feed"] = str(publish_feed(output, lane, args.version))
        print(f"Published to App Installer feed: {latest['feed']}")
    write_latest(lane, latest)
    print(f"Validated {'Store upload' if lane.store else 'development'} MSIX: {output}")
    print(f"SHA-256: {result['sha256']}")


if __name__ == "__main__":
    main()
