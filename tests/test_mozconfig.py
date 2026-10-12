"""Keeps configs/mozconfig.release a superset of configs/mozconfig."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
RELEASE_ONLY = {"ac_add_options --enable-release", "ac_add_options MOZ_PGO=1",
                "ac_add_options --enable-lto=cross"}


def options(name):
    lines = (ROOT / "configs" / name).read_text(encoding="utf-8").splitlines()
    return [line.strip() for line in lines if line.strip() and not line.lstrip().startswith("#")]


def objdir(lines):
    found = [line.split("=", 1)[1] for line in lines if line.startswith("mk_add_options MOZ_OBJDIR=")]
    assert len(found) == 1, found
    return found[0]


class ReleaseMozconfigTests(unittest.TestCase):
    def test_release_repeats_every_development_option(self):
        development, release = options("mozconfig"), options("mozconfig.release")
        shared = [line for line in development if not line.startswith("mk_add_options MOZ_OBJDIR=")]
        self.assertEqual([line for line in shared if line not in release], [])

    def test_release_adds_only_the_optimizations(self):
        development, release = options("mozconfig"), options("mozconfig.release")
        extra = {line for line in release if line not in development
                 and not line.startswith("mk_add_options MOZ_OBJDIR=")}
        self.assertEqual(extra, RELEASE_ONLY)

    def test_release_builds_into_its_own_objdir(self):
        self.assertEqual(objdir(options("mozconfig")), "@TOPSRCDIR@/../../build/firefox")
        self.assertEqual(objdir(options("mozconfig.release")), "@TOPSRCDIR@/../../build/firefox-release")


if __name__ == "__main__":
    unittest.main()
