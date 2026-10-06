"""Development packaging failure paths; no browser build or Windows install needed."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


package = load("Package-FirefoxDev")
staging = load("Stage-WinUIPane")
prune = load("Prune-FirefoxDevPackages")


def fake_repackager(staging_dir, pack=True):
    """Mimics upstream's makeappx probe and pack calls, plus an unrelated subprocess."""
    module = types.ModuleType("fake_msix")
    module.subprocess = subprocess

    def repackage_msix(makeappx, output, **_):
        staging_dir.mkdir(parents=True, exist_ok=True)
        (staging_dir / "AppxManifest.xml").write_text("<Package/>")
        module.calls.append(module.subprocess.run([sys.executable, "-c", "print(1)"],
                                                  capture_output=True, text=True).stdout.strip())
        stdout = module.subprocess.run([makeappx], check=False, capture_output=True, text=True).stdout
        if pack and "MakeAppx Tool" in stdout:
            module.subprocess.check_output([makeappx, "pack", "/d", str(staging_dir), "/p", output,
                                            "/overwrite"], universal_newlines=True)
        return output

    module.calls = []
    module.repackage_msix = repackage_msix
    return module
RUNTIME = ["Microsoft.UI.Xaml.dll", "resources.pri"]


class DevPackageTests(unittest.TestCase):
    def test_version_bounds_and_upgrade_format(self):
        self.assertEqual(package.version_string("0.1.0.0"), "0.1.0.0")
        for value in ("0.0.0.0", "1.2.3", "1.2.3.4", "01.2.3.0", "1.65536.0.0", "../1.0.0.0"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                package.version_string(value)

    def test_next_version_steps_past_installed_and_built(self):
        self.assertEqual(package.next_version(None), "0.1.0.0")
        self.assertEqual(package.next_version("0.2.0.0", "0.2.1.0", "0.1.1.0"), "0.2.2.0")
        self.assertEqual(package.next_version("0.10.0.0", "0.9.5.0"), "0.10.1.0")  # Numeric, not lexical.
        with self.assertRaisesRegex(ValueError, "pass --version"):
            package.next_version("0.1.65535.0")
        self.assertEqual(package.version_string(package.next_version("0.1.65534.0")), "0.1.65535.0")

    def test_built_versions_come_from_finished_packages(self):
        with tempfile.TemporaryDirectory() as temp:
            runs = Path(temp)
            for run, name in (("0.2.0.0-a", f"{package.IDENTITY}_0.2.0.0_x64.msix"),
                              ("0.3.0.0-b", "upstream.msix"),
                              ("0.4.0.0-c", f"{package.IDENTITY}Probe_0.4.0.0_x64.msix")):
                (runs / run).mkdir()
                (runs / run / name).write_bytes(b"")
            (runs / "0.5.0.0-d").mkdir()  # Failed run: no package.
            self.assertEqual(package.built_versions(runs), ["0.2.0.0"])

    def test_runtime_mismatch_does_not_overwrite_existing_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, dest = root / "source", root / "dest"
            source.mkdir()
            dest.mkdir()
            (source / "Microsoft.ui.xaml.dll").write_bytes(b"new runtime")
            (source / "FirefoxWinUIPane.dll").write_bytes(b"new pane")
            (dest / "Microsoft.ui.xaml.dll").write_bytes(b"existing runtime")
            with self.assertRaisesRegex(RuntimeError, "Runtime image mismatch"):
                staging.stage(source, dest, root / "report.json")
            self.assertEqual((dest / "Microsoft.ui.xaml.dll").read_bytes(), b"existing runtime")
            self.assertFalse((dest / "FirefoxWinUIPane.dll").exists())
            self.assertFalse((root / "report.json").exists())

    def test_framework_must_supply_every_omitted_runtime_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            framework, firefox, pane = root / "framework", root / "firefox", root / "pane"
            for directory in (framework, firefox, pane / "Microsoft.UI.Xaml"):
                directory.mkdir(parents=True)
            for name in package.FIREFOX_RUNTIME:
                (firefox / name).write_bytes(b"runtime")
                (framework / name).write_bytes(b"runtime")
            asset = pane / "Microsoft.UI.Xaml/asset.xbf"
            asset.write_bytes(b"asset")
            info = {"PackageFullName": "Framework", "InstallLocation": str(framework)}
            with self.assertRaisesRegex(ValueError, "lacks runtime file: Microsoft.UI.Xaml/asset.xbf"):
                package.framework_record(info, firefox, [asset], pane, staging.image_digest)
            (framework / "Microsoft.UI.Xaml").mkdir()
            (framework / "Microsoft.UI.Xaml/asset.xbf").write_bytes(b"asset")
            record = package.framework_record(info, firefox, [asset], pane, staging.image_digest)
            self.assertEqual(len(record["files"]), len(package.FIREFOX_RUNTIME) + 1)

    def test_manifest_gains_framework_dependency(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "AppxManifest.xml"
            path.write_text(f"""<Package xmlns="{package.NS['f']}" xmlns:desktop="{package.NS['desktop']}">
  <Properties><DisplayName>x</DisplayName><Description>x</Description></Properties>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0"/>
  </Dependencies>
  <Applications><Application><desktop:ExecutionAlias Alias="firefox.exe"/></Application></Applications>
</Package>""", encoding="utf-8")
            package.customize_manifest(path, "2.5.1.0")
            dependencies = ET.parse(path).getroot().findall("f:Dependencies/f:PackageDependency", package.NS)
            self.assertEqual([entry.attrib for entry in dependencies],
                             [{"Name": package.RUNTIME_FRAMEWORK, "MinVersion": "2.5.1.0",
                               "Publisher": package.RUNTIME_PUBLISHER}])
            with self.assertRaisesRegex(ValueError, "dependency contract"):
                package.customize_manifest(path, "2.5.1.0")

    def test_exe_winrt_registrations_are_replaced_only_when_supplied(self):
        text = """<assembly xmlns="urn:schemas-microsoft-com:asm.v1">
<dependency><dependentAssembly><assemblyIdentity name="mozglue"/></dependentAssembly></dependency>
<asmv3:file xmlns:asmv3="urn:schemas-microsoft-com:asm.v3" xmlns:winrtv1="urn:schemas-microsoft-com:winrt.v1" name="CoreMessagingXP.dll">
    <winrtv1:activatableClass name="Microsoft.UI.Dispatching.DispatcherQueue" threadingModel="both" />
  </asmv3:file>
</assembly>"""
        stripped, removed = package.strip_winrt_registrations(text, {"coremessagingxp.dll"})
        self.assertEqual(removed, ["CoreMessagingXP.dll"])
        self.assertNotIn("activatableClass", stripped)
        self.assertIn("mozglue", stripped)
        with self.assertRaisesRegex(ValueError, "does not supply"):
            package.strip_winrt_registrations(text, set())
        with self.assertRaisesRegex(ValueError, "no WinUI registrations"):
            package.strip_winrt_registrations(stripped, {"coremessagingxp.dll"})

    def test_upstream_layout_is_staged_without_packing(self):
        with tempfile.TemporaryDirectory() as temp:
            staging_dir = Path(temp) / "msix-temp"
            module = fake_repackager(staging_dir)
            staged = package.stage_upstream_layout(module, output=str(Path(temp) / "out.msix"))
            self.assertEqual(staged, staging_dir)
            self.assertFalse((Path(temp) / "out.msix").exists())
            self.assertEqual(module.calls, ["1"])  # Other subprocess calls still run.
            self.assertIs(module.subprocess, subprocess)

    def test_upstream_pack_contract_change_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            module = fake_repackager(Path(temp) / "msix-temp", pack=False)
            with self.assertRaisesRegex(RuntimeError, "no longer packs"):
                package.stage_upstream_layout(module, output=str(Path(temp) / "out.msix"))
            self.assertIs(module.subprocess, subprocess)

    def test_prune_keeps_packages_evidence_and_unfinished_runs(self):
        with tempfile.TemporaryDirectory() as temp:
            runs = Path(temp)
            done, failed = runs / "0.1.0.0-done", runs / "0.2.0.0-failed"
            for run in (done, failed):
                for directory in ("payload", "layout"):
                    (run / directory).mkdir(parents=True)
                    (run / directory / "xul.dll").write_bytes(b"x" * 10)
            for name in ("FirefoxWinUI.Dev_0.1.0.0_x64.msix", "upstream.msix", "package-report.json",
                         "root-pri.xml", "AppxManifest.xml"):
                (done / name).write_bytes(b"keep")
            listed = prune.prune(runs)
            self.assertEqual(sorted(p.name for p, _ in listed), ["layout", "payload", "upstream.msix"])
            self.assertTrue((done / "payload").exists())  # Listing alone deletes nothing.
            prune.prune(runs, apply=True)
            self.assertEqual(sorted(p.name for p in done.iterdir()),
                             ["AppxManifest.xml", "FirefoxWinUI.Dev_0.1.0.0_x64.msix",
                              "package-report.json", "root-pri.xml"])
            self.assertTrue((failed / "payload").exists())

    def fixture(self, root, *, missing=False, tampered=False, alias="firefox-winui-dev.exe",
                dependency=True, private_runtime=False):
        manifest = ET.Element(f"{{{package.NS['f']}}}Package")
        ET.SubElement(manifest, f"{{{package.NS['f']}}}Identity", {
            "Name": package.IDENTITY, "Publisher": package.PUBLISHER,
            "Version": "0.1.0.0", "ProcessorArchitecture": "x64"})
        dependencies = ET.SubElement(manifest, f"{{{package.NS['f']}}}Dependencies")
        if dependency:
            ET.SubElement(dependencies, f"{{{package.NS['f']}}}PackageDependency", {
                "Name": package.RUNTIME_FRAMEWORK, "MinVersion": "2.5.1.0",
                "Publisher": package.RUNTIME_PUBLISHER})
        apps = ET.SubElement(manifest, f"{{{package.NS['f']}}}Applications")
        app = ET.SubElement(apps, f"{{{package.NS['f']}}}Application", {
            "Id": "App", "Executable": "VFS/ProgramFiles/Dev Root/firefox.exe"})
        ET.SubElement(app, f"{{{package.NS['desktop']}}}ExecutionAlias", {"Alias": alias})
        path = root / "test.msix"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("AppxManifest.xml", ET.tostring(manifest))
            # Firefox's own package-root PRI must not be mistaken for the runtime's.
            archive.writestr("Resources.pri", b"firefox resources")
            if not missing:
                archive.writestr("VFS/ProgramFiles/Dev%20Root/firefox.exe", b"bad" if tampered else b"exe")
            if private_runtime:
                archive.writestr("VFS/ProgramFiles/Dev%20Root/Microsoft.UI.Xaml.dll", b"runtime")
        return path, {"firefox.exe": hashlib.sha256(b"exe").hexdigest()}

    def validate(self, path, hashes):
        return package.validate_package(path, "0.1.0.0", hashes, "2.5.1.0", RUNTIME)

    def test_url_encoded_msix_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp))
            result = self.validate(path, hashes)
            self.assertEqual(result["identity"]["Name"], package.IDENTITY)

    def test_missing_manifest_executable_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp), missing=True)
            with self.assertRaisesRegex(ValueError, "missing payload"):
                self.validate(path, hashes)

    def test_payload_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp), tampered=True)
            with self.assertRaisesRegex(ValueError, "differs"):
                self.validate(path, hashes)

    def test_firefox_alias_collision_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp), alias="firefox.exe")
            with self.assertRaisesRegex(ValueError, "must not claim"):
                self.validate(path, hashes)

    def test_missing_framework_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp), dependency=False)
            with self.assertRaisesRegex(ValueError, "must depend only on"):
                self.validate(path, hashes)

    def test_private_runtime_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp), private_runtime=True)
            with self.assertRaisesRegex(ValueError, "Private Windows App SDK runtime"):
                self.validate(path, hashes)


if __name__ == "__main__":
    unittest.main()
