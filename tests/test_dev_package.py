"""Development packaging failure paths; no browser build or Windows install needed."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from urllib.parse import unquote
from uuid import uuid4
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


package = load("Package-FennDev")
staging = load("Stage-WinUIPane")
prune = load("Prune-FennDevPackages")


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

    def test_firefox_series_ignores_channel_and_patch(self):
        for version in ("159.0a1", "159.0b3", "159.0rc2", "159.0", "159.0.1", "159.0esr", "159.0.1esr"):
            with self.subTest(version=version):
                self.assertEqual(package.firefox_series(version), (159, 0))
        for version in ("159", "159.0a", "159.0-dev", "65536.0"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                package.firefox_series(version)

    def test_firefox_version_reads_packaged_application_ini(self):
        with tempfile.TemporaryDirectory() as temp:
            (Path(temp) / "application.ini").write_text("[App]\nName=Firefox\nVersion=159.0a1\n")
            self.assertEqual(package.firefox_version(Path(temp)), "159.0a1")

    def test_next_version_counts_within_the_firefox_series(self):
        self.assertEqual(package.next_version((159, 0), "0.2.0.0", "0.2.2.0"), "159.0.1.0")
        self.assertEqual(package.next_version((159, 0), "159.0.2.0", "159.0.10.0", "158.0.40.0"),
                         "159.0.11.0")  # Numeric, not lexical; other series ignored.
        self.assertEqual(package.next_version((160, 0), "159.0.11.0"), "160.0.1.0")
        with self.assertRaisesRegex(ValueError, "pass --version"):
            package.next_version((159, 0), "159.0.65535.0")
        self.assertEqual(package.version_string(package.next_version((159, 0), "159.0.65534.0")),
                         "159.0.65535.0")

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

    def test_staging_copies_the_pane_and_retires_a_self_contained_runtime(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, dest, record = root / "source", root / "dest", root / "record.json"
            (dest / "en-us").mkdir(parents=True)
            source.mkdir()
            with self.assertRaisesRegex(RuntimeError, "Build the pane first"):
                staging.stage(source, dest, record)
            for name in staging.STAGED:
                (source / name).write_bytes(name.encode())
            # A self-contained staging left the pane runtime beside firefox.exe.
            for name in ("Microsoft.UI.Xaml.dll", "en-us/Microsoft.ui.xaml.dll.mui", "unrelated.dll"):
                (dest / name).write_bytes(b"runtime")
            record.write_text(json.dumps({"files": [
                {"file": name, "sha256": "x", "imageSha256": "x", "preserved": True}
                for name in ("Microsoft.UI.Xaml.dll", "en-us/Microsoft.ui.xaml.dll.mui")]}))
            staging.stage(source, dest, record)
            self.assertEqual(sorted(p.relative_to(dest).as_posix() for p in dest.rglob("*")),
                             sorted(["unrelated.dll", *staging.STAGED]))  # Unrecorded files stay.
            self.assertNotIn("imageSha256", record.read_text())
            staging.stage(source, dest, record)  # A framework-dependent record removes nothing.
            self.assertTrue((dest / "unrelated.dll").exists())

    def test_header_runtime_minimum_matches_the_pane_sdk(self):
        header = (ROOT / "bridge/WinUIPaneApi.h").read_text(encoding="utf-8")
        value = int(header.split("#define WINUI_PANE_RUNTIME_MIN_VERSION ")[1].split("ull")[0], 16)
        parts = [(value >> shift) & 0xFFFF for shift in (48, 32, 16, 0)]
        self.assertEqual(".".join(map(str, parts)), package.runtime_min_version())
        firefox = ROOT / "upstream/firefox/widget/windows/WinUIPaneApi.h"
        if firefox.exists():
            self.assertEqual(firefox.read_bytes(), (ROOT / "bridge/WinUIPaneApi.h").read_bytes())

    def test_framework_record_names_the_runtime_dlls_the_package_must_not_carry(self):
        with tempfile.TemporaryDirectory() as temp:
            framework = Path(temp)
            for name in ("Microsoft.UI.Xaml.dll", "CoreMessagingXP.dll", "resources.pri", "AppxManifest.xml"):
                (framework / name).write_bytes(b"x")
            info = {"PackageFullName": "Framework", "InstallLocation": str(framework)}
            self.assertEqual(package.framework_record(info)["files"], ["CoreMessagingXP.dll", "Microsoft.UI.Xaml.dll"])

    def test_manifest_gains_framework_dependency(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "AppxManifest.xml"
            path.write_text(f"""<Package xmlns="{package.NS['f']}" xmlns:desktop="{package.NS['desktop']}">
  <Properties><DisplayName>x</DisplayName><Description>x</Description></Properties>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0"/>
  </Dependencies>
  <Applications><Application><uap:VisualElements xmlns:uap="{package.NS['uap']}" BackgroundColor="#20123A"/><desktop:ExecutionAlias Alias="firefox.exe"/></Application></Applications>
</Package>""", encoding="utf-8")
            package.customize_manifest(path, "2.5.1.0")
            visuals = ET.parse(path).getroot().find(".//uap:VisualElements", package.NS)
            self.assertEqual(visuals.get("BackgroundColor"), "transparent")
            dependencies = ET.parse(path).getroot().findall("f:Dependencies/f:PackageDependency", package.NS)
            self.assertEqual([entry.attrib for entry in dependencies],
                             [{"Name": package.RUNTIME_FRAMEWORK, "MinVersion": "2.5.1.0",
                               "Publisher": package.RUNTIME_PUBLISHER}])
            with self.assertRaisesRegex(ValueError, "dependency contract"):
                package.customize_manifest(path, "2.5.1.0")

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
            for name in ("Fenn.Dev_0.1.0.0_x64.msix", "upstream.msix", "package-report.json",
                         "root-pri.xml", "AppxManifest.xml"):
                (done / name).write_bytes(b"keep")
            listed = prune.prune(runs)
            self.assertEqual(sorted(p.name for p, _ in listed), ["layout", "payload", "upstream.msix"])
            self.assertTrue((done / "payload").exists())  # Listing alone deletes nothing.
            prune.prune(runs, apply=True)
            self.assertEqual(sorted(p.name for p in done.iterdir()),
                             ["AppxManifest.xml", "Fenn.Dev_0.1.0.0_x64.msix",
                              "package-report.json", "root-pri.xml"])
            self.assertTrue((failed / "payload").exists())

    def test_prune_superseded_failures_and_old_runs(self):
        with tempfile.TemporaryDirectory() as temp:
            runs = Path(temp)
            names = ["159.0.9.0-a", "159.0.10.0-b", "159.0.11.0-failed", "159.0.12.0-c", "159.0.13.0-failed"]
            for name in names:
                (runs / name / "payload").mkdir(parents=True)
                (runs / name / "build.log").write_text("keep")
                if "failed" not in name:
                    (runs / name / f"Fenn.Dev_{name.split('-')[0]}_x64.msix").write_bytes(b"m")
            (runs / "latest.json").write_text(json.dumps(
                {"package": str(runs / "159.0.12.0-c" / "Fenn.Dev_159.0.12.0_x64.msix")}))
            listed = {p.relative_to(runs).as_posix() for p, _ in prune.prune(runs)}
            # A failure older than the newest finished run loses its intermediates; the newest failure keeps them.
            self.assertIn("159.0.11.0-failed/payload", listed)
            self.assertNotIn("159.0.13.0-failed/payload", listed)
            listed = {p.relative_to(runs).as_posix()
                      for p, _ in prune.prune(runs, keep_runs=1, installed=(159, 0, 9, 0))}
            # Whole old runs go, except the newest, latest.json's and the installed version's.
            self.assertEqual({p for p in listed if "/" not in p}, {"159.0.10.0-b", "159.0.11.0-failed"})
            prune.prune(runs, apply=True, keep_runs=1, installed=(159, 0, 9, 0))
            self.assertEqual(sorted(p.name for p in runs.iterdir() if p.is_dir()),
                             ["159.0.12.0-c", "159.0.13.0-failed", "159.0.9.0-a"])
            self.assertTrue((runs / "159.0.12.0-c" / "build.log").exists())

    def test_prune_smoke_profiles_keeps_newest_and_results(self):
        with tempfile.TemporaryDirectory() as temp:
            smoke = Path(temp)
            for index in range(4):
                run = smoke / f"run{index}"
                (run / "profile").mkdir(parents=True)
                (run / "profile" / "prefs.js").write_text("x")
                (run / "result.json").write_text("{}")
                os.utime(run, (1000 + index, 1000 + index))
            prune.prune_smoke_profiles(smoke, apply=True, keep=2)
            self.assertEqual([(smoke / f"run{i}" / "profile").exists() for i in range(4)],
                             [False, False, True, True])
            self.assertTrue(all((smoke / f"run{i}" / "result.json").exists() for i in range(4)))

    def fixture(self, root, *, missing=False, tampered=False, alias=None, dependency=True,
                private_runtime=False, lane=package.DEV, signed=None, publisher_display_name=None,
                extra=()):
        manifest = ET.Element(f"{{{package.NS['f']}}}Package")
        ET.SubElement(manifest, f"{{{package.NS['f']}}}Identity", {
            "Name": lane.identity, "Publisher": lane.publisher,
            "Version": "0.1.0.0", "ProcessorArchitecture": "x64"})
        properties = ET.SubElement(manifest, f"{{{package.NS['f']}}}Properties")
        ET.SubElement(properties, f"{{{package.NS['f']}}}PublisherDisplayName").text = (
            publisher_display_name or lane.publisher_display_name)
        dependencies = ET.SubElement(manifest, f"{{{package.NS['f']}}}Dependencies")
        if dependency:
            ET.SubElement(dependencies, f"{{{package.NS['f']}}}PackageDependency", {
                "Name": package.RUNTIME_FRAMEWORK, "MinVersion": "2.5.1.0",
                "Publisher": package.RUNTIME_PUBLISHER})
        apps = ET.SubElement(manifest, f"{{{package.NS['f']}}}Applications")
        app = ET.SubElement(apps, f"{{{package.NS['f']}}}Application", {
            "Id": "App", "Executable": "VFS/ProgramFiles/Dev Root/firefox.exe"})
        ET.SubElement(app, f"{{{package.NS['desktop']}}}ExecutionAlias", {"Alias": alias or lane.alias})
        # Each variant is independent. Avoid immediately overwriting a package
        # that Windows may still have open after the previous validation.
        path = root / f"test-{uuid4().hex}.msix"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("AppxManifest.xml", ET.tostring(manifest))
            # Firefox's own package-root PRI must not be mistaken for the runtime's.
            archive.writestr("Resources.pri", b"firefox resources")
            if not missing:
                archive.writestr("VFS/ProgramFiles/Dev%20Root/firefox.exe", b"bad" if tampered else b"exe")
            if private_runtime:
                archive.writestr("VFS/ProgramFiles/Dev%20Root/Microsoft.UI.Xaml.dll", b"runtime")
            if lane.signed if signed is None else signed:
                archive.writestr("AppxSignature.p7x", b"signature")
            for name in extra:
                archive.writestr(name, b"extra")
        return path, {"firefox.exe": hashlib.sha256(b"exe").hexdigest()}

    def test_store_lane_is_the_partner_center_identity_unsigned(self):
        store = package.STORE
        self.assertEqual((store.identity, store.publisher, store.publisher_display_name),
                         ("FennBrowser.FennBrowser", "CN=4EE000D9-3774-42FF-AC7F-E77E790B3C25", "Fenn Browser"))
        self.assertEqual((store.display_name, store.alias, store.signed, store.dev_defaults),
                         ("Fenn", "fenn.exe", False, False))
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp), lane=store)
            self.assertEqual(self.validate(path, hashes, store)["identity"]["Name"], store.identity)
            path, hashes = self.fixture(Path(temp), lane=store, signed=True)
            with self.assertRaisesRegex(ValueError, "must remain unsigned"):
                self.validate(path, hashes, store)
            path, hashes = self.fixture(Path(temp), lane=store, extra=[
                "VFS/ProgramFiles/Dev%20Root/browser/defaults/preferences/fenn-dev.js"])
            with self.assertRaisesRegex(ValueError, "development defaults"):
                self.validate(path, hashes, store)
            path, hashes = self.fixture(Path(temp), lane=store, publisher_display_name="Fenn Development")
            with self.assertRaisesRegex(ValueError, "PublisherDisplayName"):
                self.validate(path, hashes, store)

    def test_development_lanes_use_the_fenn_identities(self):
        self.assertEqual((package.DEV.identity, package.DEV.publisher, package.DEV.alias),
                         ("Fenn.Dev", "CN=Fenn Development", "fenn-dev.exe"))
        self.assertEqual(package.UNSIGNED.identity, package.DEV.identity)
        self.assertNotEqual(package.UNSIGNED.publisher, package.DEV.publisher)
        self.assertEqual((package.LOOSE.identity, package.LOOSE.alias), ("Fenn.DevLoose", "fenn-loose.exe"))
        self.assertTrue(all(lane.dev_defaults for lane in (package.DEV, package.UNSIGNED, package.LOOSE)))

    def validate(self, path, hashes, lane=package.DEV):
        return package.validate_package(path, "0.1.0.0", hashes, "2.5.1.0", RUNTIME, lane)

    def test_signature_must_match_the_lane(self):
        with tempfile.TemporaryDirectory() as temp:
            for lane in (package.DEV, package.UNSIGNED):
                with self.subTest(lane=lane.publisher):
                    path, hashes = self.fixture(Path(temp), lane=lane)
                    self.assertEqual(self.validate(path, hashes, lane)["identity"]["Publisher"], lane.publisher)
            path, hashes = self.fixture(Path(temp), signed=False)
            with self.assertRaisesRegex(ValueError, "must be signed"):
                self.validate(path, hashes)
            path, hashes = self.fixture(Path(temp), lane=package.UNSIGNED, signed=True)
            with self.assertRaisesRegex(ValueError, "must remain unsigned"):
                self.validate(path, hashes, package.UNSIGNED)
            path, hashes = self.fixture(Path(temp), lane=package.UNSIGNED)
            with self.assertRaisesRegex(ValueError, "Unexpected package identity"):
                self.validate(path, hashes)  # Same name, other publisher: another package family.

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
            with self.assertRaisesRegex(ValueError, "never firefox.exe"):
                self.validate(path, hashes)

    def test_signed_and_unsigned_lanes_share_name_but_not_publisher(self):
        self.assertEqual(package.DEV.identity, package.UNSIGNED.identity)
        self.assertEqual(package.DEV.alias, package.UNSIGNED.alias)
        self.assertTrue(package.DEV.signed)
        self.assertFalse(package.UNSIGNED.signed or package.LOOSE.signed)
        self.assertNotIn("OID.", package.DEV.publisher)  # Must equal the certificate subject.
        self.assertIn("OID.2.25.311729368913984317654407730594956997722=1", package.UNSIGNED.publisher)

    def test_loose_lane_is_a_separate_registrable_identity(self):
        self.assertNotEqual(package.LOOSE.identity, package.DEV.identity)
        self.assertNotIn("OID.", package.LOOSE.publisher)  # Windows refuses the unsigned OID loose.
        self.assertNotEqual(package.LOOSE.alias, package.DEV.alias)
        self.assertNotEqual(package.LOOSE.runs_dir, package.DEV.runs_dir)

    def test_layout_validation_matches_archive_validation(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp))
            layout = Path(temp) / "layout"
            with zipfile.ZipFile(path) as archive:
                for name in archive.namelist():
                    target = layout / unquote(name)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.read(name))
            result = package.validate_layout(layout, "0.1.0.0", hashes, "2.5.1.0", RUNTIME)
            self.assertEqual(result["identity"]["Name"], package.IDENTITY)
            with self.assertRaisesRegex(ValueError, "Unexpected package identity"):
                package.validate_layout(layout, "0.1.0.0", hashes, "2.5.1.0", RUNTIME, package.LOOSE)
            (layout / "VFS/ProgramFiles/Dev Root/firefox.exe").write_bytes(b"bad")
            with self.assertRaisesRegex(ValueError, "differs"):
                package.validate_layout(layout, "0.1.0.0", hashes, "2.5.1.0", RUNTIME)

    def test_manifest_takes_the_lane_name_and_alias(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "AppxManifest.xml"
            path.write_text(f"""<Package xmlns="{package.NS['f']}" xmlns:desktop="{package.NS['desktop']}">
  <Properties><DisplayName>x</DisplayName><Description>x</Description></Properties>
  <Dependencies><TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0"/></Dependencies>
  <Applications><Application><uap:VisualElements xmlns:uap="{package.NS['uap']}" BackgroundColor="#20123A"/><desktop:ExecutionAlias Alias="firefox.exe"/></Application></Applications>
</Package>""", encoding="utf-8")
            package.customize_manifest(path, "2.5.1.0", "described", package.LOOSE)
            manifest = ET.parse(path).getroot()
            self.assertEqual(manifest.find("f:Properties/f:DisplayName", package.NS).text,
                             package.LOOSE.display_name)
            self.assertEqual(manifest.find(".//desktop:ExecutionAlias", package.NS).get("Alias"),
                             package.LOOSE.alias)

    def test_payload_is_stale_only_when_packaged_inputs_change(self):
        with tempfile.TemporaryDirectory() as temp:
            dist = Path(temp)
            (dist / "bin/browser").mkdir(parents=True)
            (dist / package.APP_NAME).mkdir()
            self.assertEqual(package.payload_status(dist, set()), (False, "no clean Firefox package yet"))
            (dist / package.APP_NAME / f"{package.APP_NAME}.exe").write_bytes(b"exe")
            source, pane = dist / "bin/browser/omni-source.js", dist / "bin/FennPane.dll"
            for path in (source, pane):
                path.write_bytes(b"old")
                os.utime(path, (1000, 1000))
            archive = dist / f"{package.APP_NAME}-159.0a1.en-US.win64.zip"
            archive.write_bytes(b"zip")
            os.utime(archive, (2000, 2000))
            other = dist / f"{package.APP_NAME}-159.0a1.en-US.win64.xpt_artifacts.zip"  # Not the package.
            other.write_bytes(b"zip")
            os.utime(other, (5000, 5000))
            staged = {"fennpane.dll"}
            self.assertTrue(package.payload_status(dist, staged)[0])
            os.utime(pane, (3000, 3000))  # Pane-only change: the package takes the pane from its build.
            self.assertTrue(package.payload_status(dist, staged)[0])
            os.utime(source, (3000, 3000))
            self.assertEqual(package.payload_status(dist, staged),
                             (False, "dist/bin/browser/omni-source.js changed after the last mach package"))
            os.utime(archive, (4000, 4000))  # A later mach package covers it.
            self.assertTrue(package.payload_status(dist, staged)[0])

    def test_latest_record_names_the_lane_and_build(self):
        with tempfile.TemporaryDirectory() as temp:
            lane = package.Lane("Id", "CN=Id", "Id", "id.exe", Path(temp))
            package.write_latest(lane, {"version": "159.0.1.0", "package": "x.msix"})
            record = json.loads((Path(temp) / "latest.json").read_text(encoding="utf-8"))
            self.assertEqual(record, {"identity": "Id", "alias": "id.exe", "loose": False, "store": False,
                                      "version": "159.0.1.0", "package": "x.msix"})

    def test_feed_points_at_the_newest_package_and_keeps_one_previous(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            feed_dir, runs = root / "feed", root / "runs"
            runs.mkdir()
            ns = {"a": package.APPINSTALLER_NS}
            for version in ("159.0.8.0", "159.0.9.0", "159.0.10.0"):
                built = runs / f"{package.IDENTITY}_{version}_x64.msix"
                built.write_bytes(version.encode())
                feed = package.publish_feed(built, package.DEV, version, feed_dir)
            xml = ET.parse(feed).getroot()
            self.assertEqual(xml.get("Version"), "159.0.10.0")
            self.assertEqual(xml.get("Uri"), feed.resolve().as_uri())
            main = xml.find("a:MainPackage", ns)
            self.assertEqual((main.get("Name"), main.get("Publisher"), main.get("Version")),
                             (package.IDENTITY, package.SIGNED_PUBLISHER, "159.0.10.0"))
            self.assertEqual(main.get("Uri"), (feed_dir / f"{package.IDENTITY}_159.0.10.0_x64.msix").resolve().as_uri())
            launch = xml.find("a:UpdateSettings/a:OnLaunch", ns)
            self.assertEqual(launch.get("ShowPrompt"), "false")  # Never block an automated launch.
            self.assertIsNotNone(xml.find("a:UpdateSettings/a:AutomaticBackgroundTask", ns))
            # Numeric ordering: 159.0.10.0 is newest and 159.0.9.0 stays as the previous package.
            self.assertEqual(sorted(p.name for p in feed_dir.glob("*.msix")),
                             [f"{package.IDENTITY}_159.0.10.0_x64.msix", f"{package.IDENTITY}_159.0.9.0_x64.msix"])
            with self.assertRaisesRegex(ValueError, "signed lane"):
                package.publish_feed(built, package.UNSIGNED, "159.0.10.0", feed_dir)

    def test_package_pri_must_be_named_for_the_package_and_resolve(self):
        with tempfile.TemporaryDirectory() as temp:
            layout, dump = Path(temp) / "layout", Path(temp) / "dump.xml"
            (layout / "Assets").mkdir(parents=True)

            def write(name):
                dump.write_text(f"""<PriInfo><ResourceMap name="{name}" primary="true">
  <NamedResource name="Square44x44Logo.png"><Candidate type="Path"><Value>Assets\\Square44x44Logo.png</Value></Candidate></NamedResource>
</ResourceMap></PriInfo>""", encoding="utf-8")

            expected = {("Square44x44Logo.png", "", "Assets\\Square44x44Logo.png")}
            write("firefox")  # Upstream's prebuilt map name.
            with self.assertRaisesRegex(ValueError, "resource map is \\['firefox'\\]"):
                package.check_package_pri(dump, layout, package.IDENTITY, expected)
            write(package.IDENTITY)
            self.assertEqual(package.pri_candidates(dump), expected)
            with self.assertRaisesRegex(ValueError, "missing file"):
                package.check_package_pri(dump, layout, package.IDENTITY, expected)
            (layout / "Assets/Square44x44Logo.png").write_bytes(b"png")
            package.check_package_pri(dump, layout, package.IDENTITY, expected)
            # MakePri succeeds even when it merged nothing; that blanked the Start icons once.
            dump.write_text(f'<PriInfo><ResourceMap name="{package.IDENTITY}" primary="true"/></PriInfo>',
                            encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "differ from upstream"):
                package.check_package_pri(dump, layout, package.IDENTITY, expected)

    def test_manifest_claims_the_tested_windows_version(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "AppxManifest.xml"
            path.write_text(f"""<Package xmlns="{package.NS['f']}" xmlns:desktop="{package.NS['desktop']}">
  <Properties><DisplayName>x</DisplayName><Description>x</Description></Properties>
  <Dependencies><TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.22621.1555"/></Dependencies>
  <Applications><Application><uap:VisualElements xmlns:uap="{package.NS['uap']}" BackgroundColor="#20123A"/><desktop:ExecutionAlias Alias="firefox.exe"/></Application></Applications>
</Package>""", encoding="utf-8")
            package.customize_manifest(path, "2.5.1.0")
            family = ET.parse(path).getroot().find("f:Dependencies/f:TargetDeviceFamily", package.NS)
            self.assertEqual((family.get("MinVersion"), family.get("MaxVersionTested")),
                             ("10.0.22000.0", package.MAX_VERSION_TESTED))

    def test_identity_exe_manifest_names_the_identity_package(self):
        identity = load("Register-FennDevIdentity")
        text = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
<assemblyIdentity version="1.0.0.0" name="Firefox" type="win32"></assemblyIdentity>
</assembly>"""
        self.assertIsNone(identity.msix_element(text))
        marked = identity.with_msix_element(text, package.SIGNED_PUBLISHER)
        self.assertEqual(identity.msix_element("﻿" + marked),
                         {"publisher": package.SIGNED_PUBLISHER, "packageName": identity.NAME,
                          "applicationId": identity.APP_ID})
        with self.assertRaisesRegex(ValueError, "already names a package"):
            identity.with_msix_element(marked, package.SIGNED_PUBLISHER)
        manifest = identity.package_manifest(package.SIGNED_PUBLISHER, package.MAX_VERSION_TESTED)
        self.assertIn('Executable="fenn-identity.exe"', manifest)
        self.assertIn("<uap10:AllowExternalContent>true</uap10:AllowExternalContent>", manifest)
        # No framework dependency: dist/bin carries the app-local runtime.
        self.assertNotIn("PackageDependency", manifest)
        # Windows returns the registered manifest without whitespace between elements.
        registered = " ".join(manifest.split("?>", 1)[1].split()).replace("> <", "><")
        self.assertTrue(identity.same_manifest(registered, manifest))
        self.assertFalse(identity.same_manifest(registered.replace("1.0.0.0", "1.0.1.0"), manifest))

    def test_identity_package_reuses_firefox_declarations_for_dist_bin(self):
        identity = load("Register-FennDevIdentity")
        root = "VFS\\ProgramFiles\\Dev Root\\"
        source_text = f"""<Package xmlns="{identity.FOUNDATION}"
  xmlns:uap3="http://schemas.microsoft.com/appx/manifest/uap/windows10/3"
  xmlns:uap5="http://schemas.microsoft.com/appx/manifest/uap/windows10/5"
  xmlns:uap10="http://schemas.microsoft.com/appx/manifest/uap/windows10/10"
  xmlns:com="http://schemas.microsoft.com/appx/manifest/com/windows10"
  xmlns:desktop="http://schemas.microsoft.com/appx/manifest/desktop/windows10">
  <Identity Name="Fenn.Dev" Publisher="CN=x" Version="1.0.0.0" ProcessorArchitecture="x64" />
  <Applications><Application Id="App" Executable="{root}firefox.exe"><Extensions>
    <uap3:Extension Category="windows.protocol"><uap3:Protocol Name="https" /></uap3:Extension>
    <uap3:Extension Category="windows.appExecutionAlias" Executable="{root}firefox.exe" />
    <com:Extension Category="windows.comServer"><com:ComServer><com:SurrogateServer AppId="1">
      <com:Class Id="1" Path="{root}notificationserver.dll" /></com:SurrogateServer></com:ComServer></com:Extension>
    <Extension Category="windows.backgroundTasks" EntryPoint="Host"><BackgroundTasks><Task Type="timer" /></BackgroundTasks></Extension>
    <uap5:Extension Category="windows.startupTask" Executable="{root}firefox.exe" uap10:Parameters="-os-autostart">
      <uap5:StartupTask TaskId="LaunchOnLogin" Enabled="false" DisplayName="Fenn Dev" /></uap5:Extension>
    <desktop:Extension Category="windows.toastNotificationActivation"><desktop:ToastNotificationActivation ToastActivatorCLSID="1" /></desktop:Extension>
  </Extensions></Application></Applications>
  <Extensions>
    <Extension Category="windows.activatableClass.inProcessServer"><InProcessServer><Path>{root}msixcomserver.dll</Path>
      <ActivatableClass ActivatableClassId="Host" /></InProcessServer></Extension>
    <com:Extension Category="windows.comInterface"><com:ComInterface>
      <com:ProxyStub Id="2" Path="{root}AccessibleMarshal.dll" /></com:ComInterface></com:Extension>
  </Extensions>
</Package>"""
        with tempfile.TemporaryDirectory() as temp:
            source, bin_dir = Path(temp) / "AppxManifest.xml", Path(temp) / "bin"
            source.write_text(source_text, encoding="utf-8")
            bin_dir.mkdir()
            for name in ("notificationserver.dll", "msixcomserver.dll"):
                (bin_dir / name).write_bytes(b"dll")
            with self.assertRaisesRegex(FileNotFoundError, "AccessibleMarshal.dll"):
                identity.firefox_declarations(source, bin_dir)
            (bin_dir / "AccessibleMarshal.dll").write_bytes(b"dll")
            declarations = identity.firefox_declarations(source, bin_dir)
            manifest = identity.package_manifest(package.SIGNED_PUBLISHER, package.MAX_VERSION_TESTED, declarations)
            xml = ET.fromstring(manifest.split("?>", 1)[1])
            categories = {e.get("Category") for e in xml.iter() if e.get("Category")}
            self.assertEqual(categories, set(identity.APPLICATION_EXTENSIONS + identity.PACKAGE_EXTENSIONS))
            self.assertNotIn("VFS", manifest)
            self.assertIn('Path="notificationserver.dll"', manifest)
            self.assertIn("<Path>msixcomserver.dll</Path>", manifest)
            startup = next(e for e in xml.iter() if e.get("Category") == "windows.startupTask")
            self.assertEqual(startup.get("Executable"), identity.EXE)
            self.assertEqual(next(iter(startup)).get("DisplayName"), identity.DISPLAY_NAME)
            source.write_text(source_text.replace("windows.backgroundTasks", "windows.other"), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "no longer declares"):
                identity.firefox_declarations(source, bin_dir)

    def test_superseded_loose_runs_are_removed(self):
        with tempfile.TemporaryDirectory() as temp:
            lane = package.Lane("Id", "CN=Id", "Id", "id.exe", Path(temp))
            old, current = Path(temp) / "159.0.1.0-a", Path(temp) / "159.0.2.0-b"
            for run in (old, current):
                (run / "layout").mkdir(parents=True)
            package.remove_superseded_runs(lane, current)
            self.assertEqual([p.name for p in Path(temp).iterdir()], [current.name])

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
