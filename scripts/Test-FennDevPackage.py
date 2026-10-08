"""Smoke an installed development package with its defaults and the shared Marionette/UIA harness.

The package carries no Windows App SDK runtime, so it runs only with package
identity and its framework dependency; an extracted payload cannot attach the pane.
"""
import argparse
import ctypes
from ctypes import wintypes as W
import importlib.util
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
FRAMEWORK_PREFIX = "Microsoft.WindowsAppRuntime.2_"


def loaded_modules(pid):
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = W.HANDLE
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    psapi.EnumProcessModulesEx.argtypes = [W.HANDLE, ctypes.POINTER(W.HMODULE), W.DWORD,
                                           ctypes.POINTER(W.DWORD), W.DWORD]
    psapi.GetModuleFileNameExW.argtypes = [W.HANDLE, W.HMODULE, W.LPWSTR, W.DWORD]
    handle = kernel.OpenProcess(0x0410, False, pid)  # QUERY_INFORMATION | VM_READ
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        modules = (W.HMODULE * 4096)()
        needed = W.DWORD()
        if not psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3):
            raise ctypes.WinError(ctypes.get_last_error())
        paths = []
        for module in modules[:needed.value // ctypes.sizeof(W.HMODULE)]:
            buffer = ctypes.create_unicode_buffer(32768)
            if psapi.GetModuleFileNameExW(handle, module, buffer, len(buffer)):
                paths.append(Path(buffer.value))
        return paths
    finally:
        kernel.CloseHandle(handle)


def runtime_modules(pid):
    """Every module the framework supplies must load from that framework: one runtime
    per process, in every lane (MSIX, identity package, bootstrapped unpackaged)."""
    paths = loaded_modules(pid)
    xaml = [p for p in paths if p.name.lower() == "microsoft.ui.xaml.dll"]
    if len(xaml) != 1 or not xaml[0].parent.name.startswith(FRAMEWORK_PREFIX):
        raise AssertionError(f"WinUI did not load from the {FRAMEWORK_PREFIX} framework: {xaml}")
    framework = xaml[0].parent
    supplied = {p.name.lower() for p in framework.iterdir() if p.is_file()}
    runtime = [p for p in paths if p.name.lower() in supplied]
    stray = [str(p) for p in runtime if p.parent != framework]
    if stray:
        raise AssertionError(f"Windows App SDK modules loaded outside {framework}: {stray}")
    return {"framework": str(framework), "modules": sorted(p.name for p in runtime)}


def package_full_name(pid):
    """The process's package full name, or None when it has no package identity."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenProcess.restype = W.HANDLE
    kernel.CloseHandle.argtypes = [W.HANDLE]
    kernel.GetPackageFullName.argtypes = [W.HANDLE, ctypes.POINTER(W.UINT), W.LPWSTR]
    handle = kernel.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        size = W.UINT(0)
        result = kernel.GetPackageFullName(handle, ctypes.byref(size), None)
        if result == 15700:  # APPMODEL_ERROR_NO_PACKAGE
            return None
        if result != 122:
            raise OSError(result, "GetPackageFullName")
        buffer = ctypes.create_unicode_buffer(size.value)
        result = kernel.GetPackageFullName(handle, ctypes.byref(size), buffer)
        if result:
            raise OSError(result, "GetPackageFullName")
        return buffer.value
    finally:
        kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--unpackaged", action="store_true",
                        help="expect no package identity (dist/bin fenn.exe, bootstrapped onto the framework)")
    parser.add_argument("--package-name", default="Fenn.Dev",
                        help="identity name the running browser must report")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("bridge", ROOT / "scripts/Test-FirefoxInertBridge.py")
    bridge = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bridge)
    directory = ROOT / "artifacts/msix-dev-smoke" / uuid.uuid4().hex
    profile = directory / "profile"
    profile.mkdir(parents=True)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    (profile / "user.js").write_text(f'user_pref("marionette.port", {port});\n', encoding="utf-8")
    print(f"Evidence: {directory}", flush=True)
    with (directory / "process.log").open("wb") as log:
        process = subprocess.Popen([str(args.exe.resolve()), "--no-remote", "--profile", str(profile),
                                    "--marionette", "about:blank"],
                                   stdout=log, stderr=subprocess.STDOUT,
                                   env=dict(os.environ, MOZ_REMOTE_ALLOW_SYSTEM_ACCESS="1", FENN_PANE_DIAGNOSTICS="1"))
        client = None
        try:
            client = bridge.Marionette(port, process)
            client.command("WebDriver:NewSession")

            def chrome(script):
                client.command("Marionette:SetContext", {"value": "chrome"})
                return client.command("WebDriver:ExecuteScript", {"script": script, "args": []})["value"]

            deadline = time.monotonic() + 15
            while not chrome("return !!window.gWinUIPane?.attached && gWinUIPane.tabStrip;"):
                if time.monotonic() > deadline:
                    details = chrome("""return {
                        enabled: Services.prefs.getBoolPref('fenn.winui.pane.enabled'),
                        strip: Services.prefs.getBoolPref('fenn.winui.tabstrip.enabled'),
                        attached: window.gWinUIPane?.attached};""")
                    raise AssertionError(f"Packaged defaults did not attach the native strip: {details}")
                time.sleep(.1)
            state = chrome("""return {pid: Services.appinfo.processID,
                hwnd: window.docShell.treeOwner.QueryInterface(Ci.nsIBaseWindow).nativeHandle,
                profile: Services.dirsvc.get('ProfD', Ci.nsIFile).path,
                tabs: gBrowser.tabs.length};""")
            package = package_full_name(state["pid"])
            if args.unpackaged:
                if package:
                    raise AssertionError(f"Unpackaged browser unexpectedly has package identity: {package}")
            elif not (package and package.startswith(f"{args.package_name}_")):
                raise AssertionError(f"Browser lacks expected package identity: {package}")
            identity = chrome("""return (async () => {
                let { SearchService } = ChromeUtils.importESModule(
                    "moz-src:///toolkit/components/search/SearchService.sys.mjs");
                await SearchService.init();
                let urls = [];
                for (let engine of await SearchService.getAppProvidedEngines()) {
                    for (let type of ["text/html", "application/x-suggestions+json"]) {
                        for (let source of ["default", "newtab_search_widget"]) {
                            let spec = engine.getSubmission("fenn", type, source)?.uri.spec;
                            if (spec) urls.push(spec);
                        }
                    }
                }
                let [view] = await new Localization(["toolkit/branding/brandings.ftl", "browser/browser.ftl"]).formatValues(["quickactions-firefoxview"]);
                let { QuickSuggest } = ChromeUtils.importESModule(
                    "moz-src:///browser/components/urlbar/QuickSuggest.sys.mjs");
                let suggest = QuickSuggest.intendedDefaultPrefs("US", "en-US");
                let newtab = "browser.newtabpage.activity-stream.";
                let sponsored = {
                    suggest: suggest["suggest.quicksuggest.sponsored"],
                    amp: suggest["amp.featureGate"],
                    ...Object.fromEntries(["showSponsored", "system.showSponsored", "showSponsoredTopSites",
                        "feeds.section.topstories", "feeds.system.topstories"].map(
                        name => [name, Services.prefs.getBoolPref(newtab + name, null)])),
                    contile: Services.prefs.getBoolPref("browser.topsites.contile.enabled"),
                };
                return {urls, view, sponsored,
                        customIcons: Services.prefs.getBoolPref("browser.shell.customIcon.enabled")};
            })();""")
            attributed = [u for u in identity["urls"]
                          if re.search(r"firefox-b|[?&](pc|form|channel|campid|mkrid|fr|t|tt|segment)=|MOZ[A-Z]|mozff", u)]
            if not identity["urls"] or attributed:
                raise AssertionError(f"Search URLs carry Mozilla attribution: {attributed or 'no engines'}")
            if identity["view"] != "Open Fenn View" or identity["customIcons"]:
                raise AssertionError(f"Firefox-branded feature names or launcher icons remain: {identity}")
            if any(value is not False for value in identity["sponsored"].values()):
                raise AssertionError(f"Sponsored content or stories are on by default: {identity['sponsored']}")
            runtime = runtime_modules(state["pid"])
            command = ["powershell", "-NoProfile", "-Mta", "-File",
                       str(ROOT / "tests/native/SelectPaneTab.ps1"), "-BrowserProcessId", str(state["pid"]),
                       "-WindowHandle", str(int(state["hwnd"], 16)), "-TabStrip"]
            rows = subprocess.check_output(command + ["-Mode", "Inspect"], timeout=30).decode("utf-8-sig")
            subprocess.run(command + ["-Mode", "Add"], check=True, timeout=30)
            deadline = time.monotonic() + 10
            while chrome("return gBrowser.tabs.length;") != state["tabs"] + 1:
                if time.monotonic() > deadline:
                    raise AssertionError("Native new-tab activation failed")
                time.sleep(.1)
            client.command("Marionette:SetContext", {"value": "content"})
            client.command("WebDriver:Navigate", {"url": (ROOT / "tests/smoke/browser.html").as_uri()})
            value = client.command("WebDriver:ExecuteScript", {
                "script": "return document.getElementById('js').textContent;", "args": []})["value"]
            if value != "JavaScript executed":
                raise AssertionError(value)
            shutdown = client.command("Marionette:Quit", {"flags": ["eAttemptQuit"]})
            code = process.wait(timeout=30)
            if code != 0 or shutdown.get("forced"):
                raise AssertionError((code, shutdown))
            (directory / "result.json").write_text(json.dumps({
                "packageFullName": package, "runtime": runtime, "state": state, "rows": json.loads(rows),
                "searchUrls": identity["urls"], "exitCode": code, "scope": "unpackaged" if args.unpackaged else "installed identity"}, indent=2), encoding="utf-8")
            print(f"PASS: default native strip, framework-only runtime ({len(runtime['modules'])} modules), "
                  f"UIA new tab, page JavaScript, no Mozilla search attribution ({len(identity['urls'])} URLs), "
                  f"Fenn feature names, no sponsored content or stories, clean exit; package={package}")
        finally:
            if client:
                client.sock.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
