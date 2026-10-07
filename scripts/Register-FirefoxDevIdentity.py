"""Give the unpackaged development build package identity in place.

Registers a signed identity package (packaging with external location, a
"sparse" package) whose content is build/firefox/dist/bin itself, so nothing is
copied or packed. Windows grants identity only to executables whose embedded
manifest names the package, so the lane runs dist/bin/firefox-identity.exe: a
copy of firefox.exe carrying that msix element. Plain firefox.exe, and every
suite that launches it, stays unpackaged.

It reuses Firefox's MSIX declarations that its identity-dependent code needs
(notifications, background tasks, launch on login, accessibility proxy stubs)
from the newest MSIX build, rewritten for dist/bin. Protocols, file
associations and the execution alias stay with the installed MSIX.

The package declares no Windows App SDK framework dependency: dist/bin carries
the app-local runtime that firefox.exe's manifest registers, and a framework in
the package graph would let a second runtime copy load. Use the MSIX lanes to
test the shared runtime. Needs the trusted signing certificate from
Initialize-DevSigning.ps1; no elevation or Developer Mode.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build/firefox/dist/bin"
WORK = ROOT / "artifacts/msix-identity"
NAME = "FirefoxWinUI.DevIdentity"
APP_ID = "App"
EXE = "firefox-identity.exe"
DISPLAY_NAME = "WinUI Browser Dev (Identity)"
# The same version cannot be registered twice, so a changed manifest is
# re-registered by removing the old registration first; the version stays fixed.
VERSION = "1.0.0.0"
ASM = "urn:schemas-microsoft-com:asm.v1"
MSIX = "urn:schemas-microsoft-com:msix.v1"
LOGO = r"browser\chrome\browser\content\branding"


def load_packager():
    spec = importlib.util.spec_from_file_location("packager", ROOT / "scripts/Package-FirefoxDev.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FOUNDATION = "http://schemas.microsoft.com/appx/manifest/foundation/windows10"
# Firefox's MSIX declarations that its identity-dependent code uses: notification
# activation, the background-task timer and its host, launch on login, and the
# accessibility proxy stubs. Protocols, file associations and the execution alias
# are left out: they would list a second browser in Default Apps and collide with
# the installed MSIX's alias.
APPLICATION_EXTENSIONS = ("windows.comServer", "windows.toastNotificationActivation",
                          "windows.backgroundTasks", "windows.startupTask")
PACKAGE_EXTENSIONS = ("windows.activatableClass.inProcessServer", "windows.comInterface")


def newest_msix_manifest():
    """The substituted manifest of the newest FirefoxWinUI.Dev build; upstream's
    template needs build-time values (install directory, background-task class)."""
    for path in sorted((ROOT / "artifacts/msix-dev").glob("*/AppxManifest.xml"),
                       key=lambda p: p.stat().st_mtime, reverse=True):
        identity = ET.parse(path).getroot().find(f"{{{FOUNDATION}}}Identity")
        if identity is not None and identity.get("Name") == "FirefoxWinUI.Dev":
            return path
    raise FileNotFoundError("Build the MSIX once (Build-FirefoxDevPackage.ps1) so its Firefox "
                            "declarations can be reused")


def firefox_declarations(source, bin_dir=BIN):
    """Firefox's MSIX extensions, rewritten for an external location of dist/bin.
    Returns (application extensions XML, package extensions XML)."""
    for _, (prefix, uri) in ET.iterparse(source, events=["start-ns"]):
        ET.register_namespace(prefix, uri)
    root = ET.parse(source).getroot()
    application = root.find(f"{{{FOUNDATION}}}Applications/{{{FOUNDATION}}}Application")
    executable = application.get("Executable")
    install_dir = executable.rsplit("\\", 1)[0] + "\\"
    kept = {"application": [], "package": []}
    for scope, parent, categories in (
            ("application", application.find(f"{{{FOUNDATION}}}Extensions"), APPLICATION_EXTENSIONS),
            ("package", root.find(f"{{{FOUNDATION}}}Extensions"), PACKAGE_EXTENSIONS)):
        for extension in list(parent):
            if extension.get("Category") in categories:
                kept[scope].append(extension)
    found = {e.get("Category") for scope in kept.values() for e in scope}
    missing = set(APPLICATION_EXTENSIONS + PACKAGE_EXTENSIONS) - found
    if missing:
        raise ValueError(f"Upstream MSIX manifest no longer declares {sorted(missing)}")
    referenced = set()
    for element in (node for scope in kept.values() for extension in scope for node in extension.iter()):
        for key, value in element.attrib.items():
            if key == "Executable" and value == executable:
                element.set(key, EXE)
                referenced.add(EXE)
            elif key in ("Executable", "Path") and value.startswith(install_dir):
                element.set(key, value[len(install_dir):])
                referenced.add(element.get(key))
        if element.tag == f"{{{FOUNDATION}}}Path" and element.text and element.text.startswith(install_dir):
            element.text = element.text[len(install_dir):]
            referenced.add(element.text)
        if element.tag.endswith("}StartupTask"):
            element.set("DisplayName", DISPLAY_NAME)
    absent = sorted(name for name in referenced if name != EXE and not (bin_dir / name).is_file())
    if absent:
        raise FileNotFoundError(f"dist/bin lacks files the declarations reference: {absent}")

    def serialize(elements):
        return "".join(ET.tostring(element, encoding="unicode") for element in elements)
    return serialize(kept["application"]), serialize(kept["package"])


def package_manifest(publisher, max_version_tested, declarations=("", "")):
    application_extensions, package_extensions = declarations
    return f"""<?xml version="1.0" encoding="utf-8"?>
<Package IgnorableNamespaces="uap uap10"
  xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
  xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
  xmlns:uap10="http://schemas.microsoft.com/appx/manifest/uap/windows10/10"
  xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities">
  <Identity Name="{NAME}" Publisher="{publisher}" Version="{VERSION}" ProcessorArchitecture="neutral" />
  <Properties>
    <DisplayName>{DISPLAY_NAME}</DisplayName>
    <PublisherDisplayName>Firefox WinUI Development</PublisherDisplayName>
    <Logo>{LOGO}\\icon48.png</Logo>
    <uap10:AllowExternalContent>true</uap10:AllowExternalContent>
  </Properties>
  <Resources>
    <Resource Language="en-us" />
  </Resources>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.22000.0" MaxVersionTested="{max_version_tested}" />
  </Dependencies>
  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
    <rescap:Capability Name="unvirtualizedResources" />
  </Capabilities>
  <Applications>
    <Application Id="{APP_ID}" Executable="{EXE}" uap10:TrustLevel="mediumIL" uap10:RuntimeBehavior="win32App">
      <uap:VisualElements AppListEntry="none" DisplayName="{DISPLAY_NAME}" Description="{DISPLAY_NAME}"
        BackgroundColor="transparent" Square150x150Logo="{LOGO}\\icon128.png" Square44x44Logo="{LOGO}\\icon48.png" />
      {f"<Extensions>{application_extensions}</Extensions>" if application_extensions else ""}
    </Application>
  </Applications>
  {f"<Extensions>{package_extensions}</Extensions>" if package_extensions else ""}
</Package>
"""


def with_msix_element(text, publisher):
    """Add the element that ties an executable to the identity package."""
    if f'xmlns="{MSIX}"' in text or f"{{{MSIX}}}msix" in text:
        raise ValueError("firefox.exe already names a package")
    element = (f'<msix xmlns="{MSIX}" publisher="{publisher}" packageName="{NAME}" '
               f'applicationId="{APP_ID}" />\n')
    if text.count("</assembly>") != 1:
        raise ValueError("Unexpected firefox.exe manifest layout")
    marked = text.replace("</assembly>", element + "</assembly>")
    ET.fromstring(marked.encode("utf-8"))
    return marked


def msix_element(manifest_text):
    found = ET.fromstring(manifest_text.lstrip("\ufeff").encode("utf-8")).find(f"{{{MSIX}}}msix")
    return None if found is None else dict(found.attrib)


def refresh_identity_exe(mt, publisher):
    """Copy firefox.exe to firefox-identity.exe and mark it, when the build changed."""
    source, target = BIN / "firefox.exe", BIN / EXE
    WORK.mkdir(parents=True, exist_ok=True)
    extracted = WORK / "firefox-identity.manifest"

    def read_manifest(exe):
        subprocess.run([str(mt), "-nologo", f"-inputresource:{exe};#1", f"-out:{extracted}"],
                       check=True, stdout=subprocess.DEVNULL)
        return extracted.read_text(encoding="utf-8-sig")

    expected = {"publisher": publisher, "packageName": NAME, "applicationId": APP_ID}
    if (target.is_file() and target.stat().st_mtime >= source.stat().st_mtime
            and msix_element(read_manifest(target)) == expected):
        return target, False
    staging = WORK / EXE
    shutil.copyfile(source, staging)
    marked = WORK / "firefox-identity.marked.manifest"
    marked.write_text(with_msix_element(read_manifest(staging), publisher), encoding="utf-8")
    subprocess.run([str(mt), "-nologo", "-manifest", str(marked), f"-outputresource:{staging};#1"],
                   check=True, stdout=subprocess.DEVNULL)
    if msix_element(read_manifest(staging)) != expected:
        raise ValueError("firefox-identity.exe does not carry the expected msix element")
    shutil.copyfile(staging, target)
    return target, True


def powershell(script):
    return subprocess.run(["powershell", "-NoProfile", "-Command", script],
                          check=True, capture_output=True, text=True).stdout.strip()


def registration():
    # InstallLocation is the identity package's own folder under WindowsApps; the
    # directory it lends identity to is its external location (WinRT only).
    output = powershell(f"""
$p = Get-AppxPackage -Name {NAME} | Select-Object -First 1
if ($p) {{
    $null = [Windows.Management.Deployment.PackageManager, Windows.Management.Deployment, ContentType = WindowsRuntime]
    $winrt = [Windows.Management.Deployment.PackageManager]::new().FindPackageForUser('', $p.PackageFullName)
    $external = $winrt.EffectiveExternalLocation.Path
    $root = $external.TrimEnd('\\') + '\\'
    $running = @(Get-Process | Where-Object {{ $_.Path -and
        $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($_.Path) -eq '{EXE}' }} | ForEach-Object Id)
    ConvertTo-Json -Compress @{{ FullName = $p.PackageFullName; ExternalLocation = $external;
        Manifest = (Get-AppxPackageManifest -Package $p.PackageFullName).OuterXml; Running = $running }}
}}""")
    return json.loads(output) if output else None


def same_manifest(registered_xml, wanted_xml):
    def canonical(text):
        return ET.canonicalize(text.lstrip("\ufeff"), strip_text=True)
    return canonical(registered_xml) == canonical(wanted_xml.split("?>", 1)[1])


def register(packager, sdk_bin):
    makeappx, signtool, mt = (sdk_bin / name for name in ("makeappx.exe", "signtool.exe", "mt.exe"))
    certificate = packager.signing_certificate(packager.SIGNED_PUBLISHER)
    if not certificate["Trusted"]:
        raise RuntimeError("The signing certificate is not trusted; run Initialize-DevSigning.ps1 (elevated) once")
    exe, refreshed = refresh_identity_exe(mt, packager.SIGNED_PUBLISHER)
    manifest = package_manifest(packager.SIGNED_PUBLISHER, packager.MAX_VERSION_TESTED,
                                firefox_declarations(newest_msix_manifest()))
    state = registration()
    if (state and Path(state["ExternalLocation"]).resolve() == BIN.resolve()
            and same_manifest(state["Manifest"], manifest)):
        print(f"{NAME} already registered for {BIN}; identity exe {'refreshed' if refreshed else 'current'}")
        return exe
    if state and state["Running"]:
        raise RuntimeError(f"Close {EXE} first; running process IDs: {state['Running']}")
    layout = WORK / "layout"
    if layout.exists():
        shutil.rmtree(layout)
    layout.mkdir(parents=True)
    (layout / "AppxManifest.xml").write_text(manifest, encoding="utf-8")
    package = WORK / f"{NAME}.msix"
    # /nv: the manifest's files live in the external location, not the package.
    subprocess.run([str(makeappx), "pack", "/o", "/nv", "/d", str(layout), "/p", str(package)],
                   check=True, stdout=subprocess.DEVNULL)
    packager.sign_package(signtool, package, certificate["Thumbprint"])
    if state:
        powershell(f"Remove-AppxPackage -Package '{state['FullName']}'")
    powershell(f"Add-AppxPackage -Path '{package}' -ExternalLocation '{BIN.resolve()}'")
    state = registration()
    if not state or Path(state["ExternalLocation"]).resolve() != BIN.resolve():
        raise RuntimeError(f"{NAME} did not register for {BIN}: {state}")
    print(f"Registered {state['FullName']} for {BIN}")
    return exe


def unregister():
    state = registration()
    if state:
        if state["Running"]:
            raise RuntimeError(f"Close {EXE} first; running process IDs: {state['Running']}")
        powershell(f"Remove-AppxPackage -Package '{state['FullName']}'")
        print(f"Removed {state['FullName']}")
    (BIN / EXE).unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-bin", type=Path,
                        default=Path(r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"))
    parser.add_argument("--remove", action="store_true", help="unregister and delete firefox-identity.exe")
    args = parser.parse_args()
    if args.remove:
        unregister()
    else:
        print(f"Identity executable: {register(load_packager(), args.sdk_bin)}")


if __name__ == "__main__":
    main()
