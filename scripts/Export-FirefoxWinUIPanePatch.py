"""Export the pane as patch 2, relative to patch 1, using a disposable index."""
from pathlib import Path
import os
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CHECKOUT = ROOT / "upstream/firefox"
BASE = json.loads((ROOT / "configs/toolchain-baseline.json").read_text(encoding="utf-8"))["firefox"]["commit"]
TRACKED = ["browser/components/tabbrowser/content/tabgroup-menu.js", "browser/locales/en-US/browser/tabbrowser.ftl", "browser/components/tabbrowser/content/tabgroup.mjs", "browser/components/tabbrowser/content/tab-context-menu.js", "browser/components/urlbar/content/UrlbarTelemetryUtils.mjs", "browser/components/urlbar/UrlbarProviderActionsSearchMode.sys.mjs", "browser/components/urlbar/content/SearchModeSwitcher.mjs", "browser/components/urlbar/content/UrlbarInputBase.mjs", "browser/components/urlbar/content/UrlbarView.mjs", "browser/app/Makefile.in", "browser/app/firefox.exe.manifest", "browser/app/profile/firefox.js", "browser/base/jar.mn",
           "browser/base/content/browser-main.js", "dom/base/nsDOMWindowUtils.cpp",
           "dom/interfaces/base/nsIDOMWindowUtils.idl", "widget/nsIWidget.h",
           "widget/windows/nsWindow.h", "widget/windows/nsWindow.cpp",
           "widget/windows/nsAppShell.cpp", "widget/windows/WinUIPrototype.cpp",
           "widget/windows/WinUIPrototype.h"]
ADDED = ["browser/base/content/WinUITabStripSession.sys.mjs", "browser/base/content/WinUITabMenuSession.sys.mjs", "browser/base/content/WinUIUrlbarSession.sys.mjs", "browser/base/content/WinUIAddressToolbar.sys.mjs", "browser/base/content/WinUIAddressToolbar.css", "browser/base/content/browser-winui-pane.js", "widget/windows/WinUIPaneApi.h"]


def git(*args, **kwargs):
    return subprocess.run(["git", *args], cwd=CHECKOUT, check=True,
                          stdout=subprocess.PIPE, **kwargs).stdout


if __name__ == "__main__":
    if git("rev-parse", "HEAD").decode().strip() != BASE:
        raise SystemExit("Pinned Firefox base changed; review before exporting")
    if (ROOT / "bridge/WinUIPaneApi.h").read_bytes() != (CHECKOUT / ADDED[-1]).read_bytes():
        raise SystemExit("The canonical and Firefox pane ABI headers differ")
    with tempfile.TemporaryDirectory(prefix="pane-index-", dir=ROOT / "artifacts") as temporary:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temporary) / "index"))
        git("read-tree", BASE, env=env)
        git("apply", "--cached", str(ROOT / "patches/0001-inert-winui-bridge.patch"), env=env)
        patch = git("diff", "--binary", "--", *TRACKED, env=env)
        for path in ADDED:
            result = subprocess.run(["git", "diff", "--no-index", "--binary", "--", "/dev/null", path],
                                    cwd=CHECKOUT, stdout=subprocess.PIPE)
            if result.returncode != 1:
                raise SystemExit(f"Could not export {path}")
            patch += result.stdout
        git("apply", "--cached", "--check", "-", input=patch, env=env)
    destination = ROOT / "patches/0002-embedded-winui-pane.patch"
    destination.write_bytes(patch)
    git("apply", "--reverse", "--check", str(destination))
    print(f"Exported, forward-checked after patch 1, and reverse-checked {destination}")
