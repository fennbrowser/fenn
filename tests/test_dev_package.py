"""Development packaging failure paths; no browser build or Windows install needed."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import tempfile
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


class DevPackageTests(unittest.TestCase):
    def test_version_bounds_and_upgrade_format(self):
        self.assertEqual(package.version_string("0.1.0.0"), "0.1.0.0")
        for value in ("0.0.0.0", "1.2.3", "1.2.3.4", "01.2.3.0", "1.65536.0.0", "../1.0.0.0"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                package.version_string(value)

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

    def test_pri_file_references_must_resolve_at_package_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dump = root / "pri.xml"
            dump.write_text('<PriInfo><Candidate type="Path"><Value>asset.png</Value></Candidate></PriInfo>')
            with self.assertRaisesRegex(ValueError, "package-root resource"):
                package.validate_pri_layout(root, dump)
            (root / "asset.png").write_bytes(b"asset")
            package.validate_pri_layout(root, dump)

    def fixture(self, root, *, missing=False, tampered=False, alias="firefox-winui-dev.exe"):
        manifest = ET.Element(f"{{{package.NS['f']}}}Package")
        ET.SubElement(manifest, f"{{{package.NS['f']}}}Identity", {
            "Name": package.IDENTITY, "Publisher": package.PUBLISHER,
            "Version": "0.1.0.0", "ProcessorArchitecture": "x64"})
        apps = ET.SubElement(manifest, f"{{{package.NS['f']}}}Applications")
        app = ET.SubElement(apps, f"{{{package.NS['f']}}}Application", {
            "Id": "App", "Executable": "VFS/ProgramFiles/Dev Root/firefox.exe"})
        ET.SubElement(app, f"{{{package.NS['desktop']}}}ExecutionAlias", {"Alias": alias})
        path = root / "test.msix"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("AppxManifest.xml", ET.tostring(manifest))
            if not missing:
                archive.writestr("VFS/ProgramFiles/Dev%20Root/firefox.exe", b"bad" if tampered else b"exe")
        return path, {"firefox.exe": hashlib.sha256(b"exe").hexdigest()}

    def test_url_encoded_msix_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp))
            result = package.validate_package(path, "0.1.0.0", hashes)
            self.assertEqual(result["identity"]["Name"], package.IDENTITY)

    def test_missing_manifest_executable_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp), missing=True)
            with self.assertRaisesRegex(ValueError, "missing payload"):
                package.validate_package(path, "0.1.0.0", hashes)

    def test_payload_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp), tampered=True)
            with self.assertRaisesRegex(ValueError, "differs"):
                package.validate_package(path, "0.1.0.0", hashes)

    def test_firefox_alias_collision_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path, hashes = self.fixture(Path(temp), alias="firefox.exe")
            with self.assertRaisesRegex(ValueError, "must not claim"):
                package.validate_package(path, "0.1.0.0", hashes)


if __name__ == "__main__":
    unittest.main()
