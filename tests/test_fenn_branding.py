"""Checks for the project-owned Fenn branding and its staging into Firefox."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BRANDING = ROOT / "branding/fenn"
spec = importlib.util.spec_from_file_location("stage", ROOT / "scripts/Stage-FennBranding.py")
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

# What the Windows build reads from a branding directory: splash.rc icons,
# FirefoxBranding() tiles, content/jar.mn, NSIS_BRANDING_FILES and the MSIX assets.
WINDOWS_FILES = [
    "configure.sh", "moz.build", "branding.nsi", "pref/firefox-branding.js",
    "firefox.ico", "firefox64.ico", "document.ico", "document_pdf.ico", "newtab.ico", "newwindow.ico", "pbmode.ico",
    "firefox.VisualElementsManifest.xml", "private_browsing.VisualElementsManifest.xml",
    "VisualElements_150.png", "VisualElements_70.png", "PrivateBrowsing_150.png", "PrivateBrowsing_70.png",
    "wizHeader.bmp", "wizHeaderRTL.bmp", "wizWatermark.bmp",
    "content/jar.mn", "content/moz.build", "content/aboutDialog.css", "content/about.png",
    "content/about-logo.png", "content/about-logo@2x.png", "content/about-logo.svg", "content/about-wordmark.svg",
    "content/about-logo-private.png", "content/about-logo-private@2x.png", "content/document_pdf.svg",
    "content/firefox-wordmark.svg", "locales/jar.mn", "locales/moz.build",
    "locales/en-US/brand.ftl", "locales/en-US/brand.properties",
    *(f"default{s}.png" for s in (16, 22, 24, 32, 48, 64, 128, 256)),
    *(f"msix/Assets/{n}" for n in (
        "Document44x44.png", "LargeTile.scale-200.png", "MedTile.scale-200.png", "SmallTile.scale-200.png",
        "Square150x150Logo.scale-200.png", "Square44x44Logo.altform-lightunplated_targetsize-256.png",
        "Square44x44Logo.altform-unplated_targetsize-256.png", "Square44x44Logo.scale-200.png",
        "Square44x44Logo.targetsize-256.png", "StoreLogo.scale-200.png", "Wide310x150Logo.scale-200.png")),
]


class FennBrandingTests(unittest.TestCase):
    def test_windows_build_inputs_exist(self):
        missing = [f for f in WINDOWS_FILES if not (BRANDING / f).is_file()]
        self.assertEqual(missing, [])

    def test_names_are_fenn(self):
        ftl = (BRANDING / "locales/en-US/brand.ftl").read_text(encoding="utf-8")
        for term in ("-brand-shorter-name", "-brand-short-name", "-brand-shortcut-name",
                     "-brand-full-name", "-brand-product-name"):
            self.assertIn(f"{term} = Fenn\n", ftl)
        self.assertNotIn("Nightly", ftl)
        self.assertIn("MOZ_APP_DISPLAYNAME=Fenn\n", (BRANDING / "configure.sh").read_text(encoding="utf-8"))
        properties = (BRANDING / "locales/en-US/brand.properties").read_text(encoding="utf-8")
        self.assertNotIn("Nightly", properties)

    def test_tile_sizes_match_upstream(self):
        sizes = {"msix/Assets/Wide310x150Logo.scale-200.png": (620, 300),
                 "msix/Assets/Square44x44Logo.targetsize-256.png": (256, 256),
                 "VisualElements_150.png": (300, 300), "content/about-logo@2x.png": (384, 384)}
        for name, size in sizes.items():
            header = (BRANDING / name).read_bytes()[16:24]
            self.assertEqual(struct.unpack(">II", header), size, name)

    def test_executable_name_matches_the_mozconfig(self):
        mozconfig = (ROOT / "configs/mozconfig").read_text(encoding="utf-8")
        self.assertIn("ac_add_options --with-app-name=fenn\n", mozconfig)

        def load(name):
            spec = importlib.util.spec_from_file_location(name, ROOT / f"scripts/{name}.py")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return module

        self.assertEqual(load("Package-FirefoxDev").APP_NAME, "fenn")
        self.assertEqual(load("Test-FirefoxInertBridge").EXE.name, "fenn.exe")
        identity = load("Register-FirefoxDevIdentity")
        self.assertEqual((identity.APP_EXE, identity.EXE), ("fenn.exe", "fenn-identity.exe"))

    def test_app_manifest_tracks_firefoxs(self):
        with tempfile.TemporaryDirectory() as temp:
            checkout = Path(temp)
            (checkout / ".git/info").mkdir(parents=True)
            (checkout / "browser/app").mkdir(parents=True)
            source = checkout / "browser/app/firefox.exe.manifest"
            source.write_bytes(b"<assembly>one</assembly>")
            self.assertEqual(stage.stage_app_manifest(checkout), ["updated browser/app/fenn.exe.manifest"])
            self.assertEqual(stage.stage_app_manifest(checkout), [])  # Unchanged stays untouched.
            source.write_bytes(b"<assembly>two</assembly>")  # An upstream change carries over.
            stage.stage_app_manifest(checkout)
            self.assertEqual((checkout / "browser/app/fenn.exe.manifest").read_bytes(), b"<assembly>two</assembly>")
            exclude = (checkout / ".git/info/exclude").read_text(encoding="utf-8")
            self.assertEqual(exclude.count("/browser/app/fenn.exe.manifest"), 1)

    def test_stage_mirrors_and_prunes(self):
        with tempfile.TemporaryDirectory() as temp:
            source, checkout = Path(temp) / "source", Path(temp) / "checkout"
            (source / "content").mkdir(parents=True)
            (source / "content/a.txt").write_text("a")
            (source / "b.txt").write_text("b")
            (checkout / ".git/info").mkdir(parents=True)
            target = checkout / stage.RELATIVE
            (target / "old").mkdir(parents=True)
            (target / "old/stale.txt").write_text("stale")
            (target / "b.txt").write_text("b")
            changes = stage.stage(source, checkout)
            self.assertEqual(sorted(changes), ["removed old/stale.txt", "updated content/a.txt"])
            self.assertFalse((target / "old").exists())
            self.assertEqual(stage.stage(source, checkout), [])  # Unchanged files stay untouched.
            exclude = (checkout / ".git/info/exclude").read_text(encoding="utf-8")
            self.assertEqual(exclude.count(f"/{stage.RELATIVE}/"), 1)


if __name__ == "__main__":
    unittest.main()
