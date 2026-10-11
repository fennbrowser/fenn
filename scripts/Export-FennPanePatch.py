"""Export Fenn's single Firefox patch against the pinned base, using a disposable index."""
from pathlib import Path
import os
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CHECKOUT = ROOT / "upstream/firefox"
BASE = json.loads((ROOT / "configs/toolchain-baseline.json").read_text(encoding="utf-8"))["firefox"]["commit"]
TRACKED = ["browser/components/urlbar/UrlbarSearchTermsPersistence.sys.mjs", "browser/components/urlbar/content/SearchEngineStore.mjs", "browser/components/urlbar/tests/unit/test_UrlbarQueryContext.js", "browser/components/urlbar/tests/unit/test_UrlbarParentController_unit.js", "browser/components/urlbar/content/UrlbarEventBufferer.mjs", "browser/components/urlbar/content/UrlbarShared.mjs", "browser/components/urlbar/content/UrlbarChildTelemetry.mjs", "browser/components/urlbar/tests/unit/xpcshell.toml", "browser/components/urlbar/tests/UrlbarTestUtils.sys.mjs", "browser/components/urlbar/tests/unit/head.js", "browser/components/urlbar/tests/unit/test_provider_notification.js", "browser/components/urlbar/tests/unit/test_result_highlights.js", "browser/components/urlbar/content/UrlbarChildController.mjs", "browser/components/urlbar/UrlbarParentController.sys.mjs", "browser/components/urlbar/UrlbarProvidersManager.sys.mjs", "browser/components/urlbar/UrlbarPrefs.sys.mjs", "browser/themes/shared/urlbar.css", "browser/components/urlbar/content/SmartbarInput.mjs", "browser/components/downloads/content/downloads.js", "toolkit/content/editMenuOverlay.js", "browser/components/tabbrowser/Tabbrowser.sys.mjs", "toolkit/modules/PopupNotifications.sys.mjs", "browser/components/customizableui/PanelMultiView.sys.mjs", "browser/base/content/browser-pageActions.js", "browser/components/customizableui/CustomizableUI.sys.mjs", "browser/base/content/browser-addons.js", "browser/components/customizableui/content/panelUI.js", "browser/components/tabbrowser/content/tabgroup-menu.js", "browser/locales/en-US/browser/tabbrowser.ftl", "browser/components/tabbrowser/content/tabgroup.mjs", "browser/components/tabbrowser/content/tab-context-menu.js", "browser/components/urlbar/content/UrlbarTelemetryUtils.mjs", "browser/components/urlbar/UrlbarProviderActionsSearchMode.sys.mjs", "browser/components/urlbar/content/SearchModeSwitcher.mjs", "browser/components/urlbar/content/UrlbarInputBase.mjs", "browser/components/urlbar/content/UrlbarView.mjs", "browser/app/firefox.exe.manifest", "browser/app/profile/firefox.js", "browser/moz.configure", "toolkit/mozapps/notificationserver/NotificationComServer.cpp", "python/mozbuild/mozbuild/repackaging/msix.py", "browser/base/jar.mn", "toolkit/components/search/ConfigSearchEngine.sys.mjs", "toolkit/locales/en-US/toolkit/branding/brandings.ftl", "browser/locales/en-US/browser/appExtensionFields.ftl", "browser/locales/en-US/chrome/overrides/appstrings.properties", "browser/base/content/default-bookmarks.html", "browser/app/moz.build", "browser/components/shell/jar.mn", "browser/components/urlbar/QuickSuggest.sys.mjs",
           "browser/base/content/browser-main.js", "dom/base/nsDOMWindowUtils.cpp",
           "dom/interfaces/base/nsIDOMWindowUtils.idl", "widget/nsIWidget.h",
           "widget/windows/nsWindow.h", "widget/windows/nsWindow.cpp",
           "widget/windows/nsAppShell.cpp",
           "widget/windows/moz.build", "browser/installer/package-manifest.in"]
ADDED = ["browser/components/urlbar/content/UrlbarBrowserState.mjs", "browser/components/urlbar/content/UrlbarSelectedSearch.mjs", "browser/components/urlbar/content/UrlbarUrlValue.mjs", "browser/components/urlbar/content/UrlbarEditSession.mjs", "browser/components/urlbar/content/UrlbarQuerySession.mjs", "browser/components/urlbar/content/UrlbarCompositionModel.mjs", "browser/components/urlbar/content/UrlbarAutofillModel.mjs", "browser/components/urlbar/content/UrlbarQueryModel.mjs", "browser/components/urlbar/content/UrlbarQueryLifecycle.mjs", "browser/components/urlbar/content/UrlbarSearchModeModel.mjs", "browser/components/urlbar/content/UrlbarSearchModeBinding.mjs", "browser/components/urlbar/content/UrlbarClipboardText.mjs", "browser/components/urlbar/content/UrlbarResultPresentation.mjs", "browser/components/urlbar/content/UrlbarResultActions.mjs", "browser/base/content/WinUIActionStamp.sys.mjs", "browser/base/content/WinUIHistoryMenu.sys.mjs", "browser/base/content/WinUIToolbarCustomization.sys.mjs", "browser/base/content/WinUIDismissalAcknowledgments.sys.mjs", "browser/base/content/WinUIResultContextMenu.sys.mjs", "browser/base/content/WinUIAddressContextMenu.sys.mjs", "browser/base/content/WinUIOmnibarActions.sys.mjs", "browser/base/content/WinUIPinnedExtensions.sys.mjs", "browser/base/content/WinUITabStripSession.sys.mjs", "browser/base/content/WinUITabMenuSession.sys.mjs", "browser/base/content/WinUIUrlbarSession.sys.mjs", "browser/base/content/WinUIAddressToolbar.sys.mjs", "browser/base/content/WinUIAddressToolbar.css", "browser/base/content/browser-winui-pane.js", "widget/windows/WinUIPrototype.cpp", "widget/windows/WinUIPrototype.h", "widget/windows/WinUIPaneApi.h"]


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
        patch = git("diff", "--binary", "--", *TRACKED, env=env)
        for path in ADDED:
            result = subprocess.run(["git", "diff", "--no-index", "--binary", "--", "/dev/null", path],
                                    cwd=CHECKOUT, stdout=subprocess.PIPE)
            if result.returncode != 1:
                raise SystemExit(f"Could not export {path}")
            patch += result.stdout
        git("apply", "--cached", "--check", "-", input=patch, env=env)
    destination = ROOT / "patches/0001-fenn-winui-pane.patch"
    destination.write_bytes(patch)
    git("apply", "--reverse", "--check", str(destination))
    print(f"Exported, forward-checked against the pinned base, and reverse-checked {destination}")
