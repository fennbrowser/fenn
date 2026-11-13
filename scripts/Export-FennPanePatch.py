"""Export Fenn's single Firefox patch against the pinned base, using a disposable index."""
from pathlib import Path
import os
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CHECKOUT = ROOT / "upstream/firefox"
BASE = json.loads((ROOT / "configs/toolchain-baseline.json").read_text(encoding="utf-8"))["firefox"]["commit"]
TRACKED = ["browser/components/tabbrowser/NewTabPagePreloading.sys.mjs", "browser/components/newtab/AboutNewTabRedirector.sys.mjs", "browser/components/BrowserContentHandler.sys.mjs", "toolkit/xre/nsAppRunner.cpp", "browser/modules/BrowserWindowTracker.sys.mjs", "accessible/base/FocusManager.cpp", "accessible/base/FocusManager.h", "accessible/windows/msaa/Platform.cpp", "dom/ipc/PBrowser.ipdl", "dom/ipc/BrowserChild.h", "dom/ipc/moz.build", "mozglue/misc/moz.build", "mozglue/misc/PreXULSkeletonUI.cpp", "browser/app/nsBrowserApp.cpp", "browser/components/BrowserGlue.sys.mjs", "browser/components/urlbar/UrlbarValueFormatter.sys.mjs", "browser/base/content/browser-context.js", "browser/base/content/nsContextMenu.sys.mjs", "browser/actors/ContextMenuParent.sys.mjs", "browser/actors/ContextMenuChild.sys.mjs", "browser/base/content/browser-siteProtections.js", "browser/base/content/browser-trustPanel.js", "browser/components/translations/content/selectTranslationsPanel.js", "browser/components/translations/content/fullPageTranslationsPanel.js", "browser/base/content/browser-siteIdentity.js", "browser/base/content/browser-sitePermissionPanel.js", "browser/components/places/PlacesUIUtils.sys.mjs", "browser/components/places/content/editBookmark.js", "browser/base/content/browser-places.js", "browser/base/content/browser.js", "browser/components/customizableui/CustomizableWidgets.sys.mjs", "browser/components/privatebrowsing/ResetPBMPanel.sys.mjs", "browser/components/sessionstore/SessionSaver.sys.mjs", "browser/components/sessionstore/SessionFile.sys.mjs", "browser/components/search/SearchOneOffs.sys.mjs", "browser/components/urlbar/UrlbarSearchOneOffs.sys.mjs", "browser/components/urlbar/UrlbarProviderQuickSuggestContextualOptIn.sys.mjs", "browser/components/urlbar/UrlbarSearchTermsPersistence.sys.mjs", "browser/components/urlbar/content/SearchEngineStore.mjs", "browser/components/urlbar/tests/unit/test_UrlbarQueryContext.js", "browser/components/urlbar/tests/unit/test_UrlbarParentController_unit.js", "browser/components/urlbar/content/UrlbarEventBufferer.mjs", "browser/components/urlbar/content/UrlbarShared.mjs", "browser/components/urlbar/content/UrlbarChildTelemetry.mjs", "browser/components/urlbar/tests/unit/xpcshell.toml", "browser/components/urlbar/tests/UrlbarTestUtils.sys.mjs", "browser/components/urlbar/tests/unit/head.js", "browser/components/urlbar/tests/unit/test_provider_notification.js", "browser/components/urlbar/tests/unit/test_result_highlights.js", "browser/components/urlbar/content/UrlbarChildController.mjs", "browser/components/urlbar/UrlbarParentController.sys.mjs", "browser/components/urlbar/UrlbarProvidersManager.sys.mjs", "browser/components/urlbar/UrlbarPrefs.sys.mjs", "browser/themes/shared/urlbar.css", "browser/themes/shared/browser-shared.css", "browser/base/content/test/browser-about/browser_aboutNewTab_bookmarksToolbarEmpty.js", "browser/components/urlbar/content/SmartbarInput.mjs", "browser/components/downloads/content/downloads.js", "toolkit/content/editMenuOverlay.js", "browser/components/tabbrowser/Tabbrowser.sys.mjs", "toolkit/modules/PopupNotifications.sys.mjs", "browser/components/customizableui/PanelMultiView.sys.mjs", "browser/base/content/browser-pageActions.js", "browser/components/customizableui/CustomizableUI.sys.mjs", "browser/base/content/browser-addons.js", "browser/components/customizableui/content/panelUI.js", "browser/components/tabbrowser/content/tabgroup-menu.js", "browser/locales/en-US/browser/tabbrowser.ftl", "browser/components/tabbrowser/content/tabgroup.mjs", "browser/components/tabbrowser/content/tab-context-menu.js", "browser/components/urlbar/content/UrlbarTelemetryUtils.mjs", "browser/components/urlbar/UrlbarProviderActionsSearchMode.sys.mjs", "browser/components/urlbar/content/SearchModeSwitcher.mjs", "browser/components/urlbar/content/UrlbarInputBase.mjs", "browser/components/urlbar/content/UrlbarView.mjs", "browser/app/firefox.exe.manifest", "browser/app/profile/firefox.js", "browser/moz.configure", "toolkit/mozapps/notificationserver/NotificationComServer.cpp", "python/mozbuild/mozbuild/repackaging/msix.py", "browser/base/jar.mn", "toolkit/components/search/ConfigSearchEngine.sys.mjs", "toolkit/locales/en-US/toolkit/branding/brandings.ftl", "browser/locales/en-US/browser/appExtensionFields.ftl", "browser/locales/en-US/chrome/overrides/appstrings.properties", "browser/base/content/default-bookmarks.html", "browser/app/moz.build", "browser/components/shell/jar.mn", "browser/components/urlbar/QuickSuggest.sys.mjs",
           "browser/base/content/browser-main.js", "dom/base/nsDOMWindowUtils.cpp",
           "dom/interfaces/base/nsIDOMWindowUtils.idl", "widget/nsIWidget.h",
           "widget/windows/nsWindow.h", "widget/windows/WinPointerEvents.cpp", "widget/windows/nsWindow.cpp",
           "widget/windows/WinIMEHandler.cpp", "widget/windows/nsAppShell.cpp", "widget/windows/WinUtils.cpp",
           "widget/windows/CompositorWidgetChild.cpp", "widget/windows/InProcessWinCompositorWidget.cpp", "widget/windows/moz.build",
           "widget/windows/RemoteBackbuffer.cpp", "widget/windows/WinCompositorWidget.cpp", "widget/windows/WinCompositorWindowThread.cpp",
           "widget/windows/WinCompositorWindowThread.h", "widget/windows/WinTaskbar.cpp", "widget/windows/nsWindowGfx.cpp", "xpfe/appshell/AppWindow.cpp", "browser/installer/package-manifest.in",
           "config/rules.mk", "security/manager/ssl/tests/unit/xpcshell.toml"]
ADDED = ["browser/base/content/WinUIBookmarksToolbar.sys.mjs", "browser/base/content/WinUINewTabDocument.sys.mjs", "browser/base/content/WinUINewTabDocumentChild.sys.mjs", "browser/base/content/WinUINewTabDocumentParent.sys.mjs", "browser/base/content/winui-newtab.html", "browser/base/content/WinUINewTabPage.sys.mjs", "browser/base/content/WinUIProcessRecovery.sys.mjs", "browser/base/content/WinUIStartupPaint.sys.mjs", "browser/base/content/WinUIStartupPaintChild.sys.mjs", "browser/base/content/WinUIProcessStartup.sys.mjs", "browser/base/content/WinUIRecoveryIdentity.sys.mjs", "browser/base/content/WinUIAutomaticStartup.sys.mjs", "widget/windows/FennCompositorParent.h", "browser/base/content/WinUIStartupTransfer.sys.mjs", "dom/ipc/FennPageGeometry.cpp", "mozglue/misc/FennStartupWindow.cpp", "mozglue/misc/FennStartupWindow.h", "browser/base/content/WinUIChromeLayout.sys.mjs", "browser/base/content/WinUIChromeLayout.css", "browser/base/content/WinUIBackForwardMenu.sys.mjs",
    "browser/base/content/WinUIAllTabs.sys.mjs", "browser/base/content/WinUIPageContextMenu.sys.mjs", "browser/base/content/WinUISelectedTranslationsPanel.sys.mjs", "browser/base/content/WinUITranslationsPanel.sys.mjs", "browser/base/content/WinUIConnectionPanel.sys.mjs", "browser/base/content/WinUISitePermissionsPanel.sys.mjs", "browser/base/content/WinUINotificationPanel.sys.mjs", "browser/base/content/WinUIBookmarkPanel.sys.mjs", "browser/components/urlbar/content/UrlbarInputIdentity.mjs", "browser/components/urlbar/content/UrlbarBrowserState.mjs", "browser/components/urlbar/content/UrlbarSelectedSearch.mjs", "browser/components/urlbar/content/UrlbarUrlValue.mjs", "browser/components/urlbar/content/UrlbarEditSession.mjs", "browser/components/urlbar/content/UrlbarQuerySession.mjs", "browser/components/urlbar/content/UrlbarCompositionModel.mjs", "browser/components/urlbar/content/UrlbarAutofillModel.mjs", "browser/components/urlbar/content/UrlbarQueryModel.mjs", "browser/components/urlbar/content/UrlbarQueryLifecycle.mjs", "browser/components/urlbar/content/UrlbarSearchModeModel.mjs", "browser/components/urlbar/content/UrlbarSearchModeBinding.mjs", "browser/components/urlbar/content/UrlbarClipboardText.mjs", "browser/components/urlbar/content/UrlbarResultPresentation.mjs", "browser/components/urlbar/content/UrlbarResultActions.mjs", "browser/base/content/WinUIActionStamp.sys.mjs", "browser/base/content/WinUIHistoryMenu.sys.mjs", "browser/base/content/WinUIDownloadsPanel.sys.mjs", "browser/base/content/WinUIProfileMenu.sys.mjs", "browser/base/content/WinUIOverflowMenu.sys.mjs", "browser/base/content/WinUIDeveloperMenu.sys.mjs", "browser/base/content/WinUIProfilerPanel.sys.mjs", "browser/base/content/WinUITabGroupsMenu.sys.mjs", "browser/base/content/WinUIGroupHeader.sys.mjs", "browser/base/content/WinUIPrivacyPanels.sys.mjs", "browser/base/content/WinUIToolbarCustomization.sys.mjs", "browser/base/content/WinUIStockToolbar.sys.mjs", "browser/base/content/WinUIDismissalAcknowledgments.sys.mjs", "browser/base/content/WinUIResultContextMenu.sys.mjs", "browser/base/content/WinUIAddressContextMenu.sys.mjs", "browser/base/content/WinUIOmnibarActions.sys.mjs", "browser/base/content/WinUIPinnedExtensions.sys.mjs", "browser/base/content/WinUIProductMenu.sys.mjs", "browser/base/content/WinUITabStripSession.sys.mjs", "browser/base/content/WinUITabMenuCommands.sys.mjs", "browser/base/content/WinUITabMenuSession.sys.mjs", "browser/base/content/WinUIAddressSession.sys.mjs", "browser/base/content/WinUIAddressToolbar.sys.mjs", "browser/base/content/WinUINavigationState.sys.mjs", "browser/base/content/WinUIAddressToolbar.css", "browser/base/content/browser-winui-pane.js", "widget/windows/WinUIPrototype.cpp", "widget/windows/WinUIPrototype.h", "widget/windows/WinUIAccessibleRoot.h", "widget/windows/WinUIDropTarget.h", "widget/windows/WinUIPageInput.cpp", "widget/windows/WinUIPageInput.h", "browser/base/content/WinUIAddressHost.sys.mjs", "widget/windows/WinUIPaneApi.h"]




TRACKED += ["browser/components/moz.build"]
ADDED = [
    "browser/components/fennadblock/moz.build",
    "browser/components/fennadblock/FennAdblockFetcher.sys.mjs",
    "browser/components/fennadblock/FennAdblockService.sys.mjs",
    "browser/components/fennadblock/FennAdblockUpdates.sys.mjs",
    "browser/components/fennadblock/FennAdblockParent.sys.mjs",
    "browser/components/fennadblock/FennAdblockChild.sys.mjs",
    "browser/components/fennadblock/FennAdblockDom.sys.mjs",
    "browser/components/fennadblock/FennAdblockTestNotice.sys.mjs",
    "browser/components/fennadblock/FennAdblockDiagnostics.sys.mjs",
    "browser/components/fennadblock/FennAdblockProcedural.sys.mjs",
    "browser/components/fennadblock/FennAdblockSelectors.sys.mjs",
    "browser/components/fennadblock/FennAdblockStyles.sys.mjs",
] + ADDED

TRACKED += ["netwerk/protocol/http/nsHttpChannel.cpp"]
TRACKED += ["toolkit/components/nimbus/lib/RemoteSettingsExperimentLoader.sys.mjs"]
TRACKED += ["dom/chrome-webidl/ChromeUtils.webidl", "dom/base/ChromeUtils.h", "js/xpconnect/loader/moz.build"]
ADDED = ["js/xpconnect/loader/FennScriptCompiler.cpp"] + ADDED
TRACKED += ["netwerk/protocol/http/HttpChannelParams.ipdlh", "netwerk/protocol/http/HttpChannelParent.cpp", "netwerk/protocol/http/HttpChannelChild.cpp"]

CONTEXT_LINES = 1


def git(*args, **kwargs):
    return subprocess.run(["git", *args], cwd=CHECKOUT, check=True,
                          stdout=subprocess.PIPE, **kwargs).stdout


def normalize_line_endings():
    """Restore LF in exported sources that a Windows tool rewrote as CRLF.

    Firefox is LF; a CRLF rewrite would turn every line of the file into a patch
    change. Only files whose pinned version (or, for added files, Firefox's
    convention) is LF are touched.
    """
    repaired = []
    for path in [*TRACKED, *ADDED]:
        source = CHECKOUT / path
        if not source.is_file():
            continue
        data = source.read_bytes()
        if b"\r" not in data:
            continue
        if path in TRACKED:
            pinned = subprocess.run(["git", "show", f"{BASE}:{path}"], cwd=CHECKOUT,
                                    capture_output=True).stdout
            if b"\r\n" in pinned:
                continue
        source.write_bytes(data.replace(b"\r\n", b"\n"))
        repaired.append(path)
    if repaired:
        print("Restored LF line endings: " + ", ".join(repaired))


def check_export_scope():
    """Reject tracked source edits that the explicit export lists would omit."""
    changed = git("diff", BASE, "--name-only", "-z").decode("utf-8").split("\0")
    # A new file is untracked until it is added, and `git diff` never lists it; an
    # unlisted one would simply be missing from the exported patch and from every
    # tree reconstructed from it (the CI source-reconstruction job).
    untracked = git("ls-files", "--others", "--exclude-standard", "-z").decode("utf-8").split("\0")
    omitted = sorted((set(changed) | set(untracked)) - set(TRACKED) - set(ADDED) - {""})
    if omitted:
        raise SystemExit("Firefox files missing from export lists: " + ", ".join(omitted))


if __name__ == "__main__":
    if git("rev-parse", "HEAD").decode().strip() != BASE:
        raise SystemExit("Pinned Firefox base changed; review before exporting")
    normalize_line_endings()
    if (ROOT / "bridge/WinUIPaneApi.h").read_bytes() != (CHECKOUT / ADDED[-1]).read_bytes():
        raise SystemExit("The canonical and Firefox pane ABI headers differ")
    check_export_scope()
    with tempfile.TemporaryDirectory(prefix="pane-index-", dir=ROOT / "artifacts") as temporary:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temporary) / "index"))
        git("read-tree", BASE, env=env)
        # One context line keeps a hunk from failing on churn two or three lines away;
        # `git apply` places it by line number and the checks below confirm it.
        patch = git("diff", "--binary", f"-U{CONTEXT_LINES}", "--", *TRACKED, env=env)
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
