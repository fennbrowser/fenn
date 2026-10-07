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


def framework_file_names():
    """Names of every file in the installed Windows App SDK 2 frameworks."""
    output = subprocess.run(["powershell", "-NoProfile", "-Command",
        "Get-AppxPackage -Name 'Microsoft.WindowsAppRuntime.2' | ForEach-Object InstallLocation"],
        check=True, capture_output=True, text=True).stdout.splitlines()
    return {p.name.lower() for location in output if location.strip()
            for p in Path(location.strip()).iterdir() if p.is_file()}


def runtime_modules(pid, app_local=None):
    """One runtime per process. By default every module the framework supplies must
    load from that framework; with app_local, every such module must load from that
    directory and none from a framework."""
    paths = loaded_modules(pid)
    xaml = [p for p in paths if p.name.lower() == "microsoft.ui.xaml.dll"]
    if app_local:
        if len(xaml) != 1 or xaml[0].parent != app_local:
            raise AssertionError(f"WinUI did not load from {app_local}: {xaml}")
        home, supplied = app_local, framework_file_names()
    else:
        if len(xaml) != 1 or not xaml[0].parent.name.startswith(FRAMEWORK_PREFIX):
            raise AssertionError(f"WinUI did not load from the {FRAMEWORK_PREFIX} framework: {xaml}")
        home = xaml[0].parent
        supplied = {p.name.lower() for p in home.iterdir() if p.is_file()}
    runtime = [p for p in paths if p.name.lower() in supplied]
    stray = [str(p) for p in runtime if p.parent != home]
    if stray:
        raise AssertionError(f"Windows App SDK modules loaded outside {home}: {stray}")
    return {"framework" if not app_local else "appLocal": str(home), "modules": sorted(p.name for p in runtime)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--app-local-runtime", action="store_true",
                        help="expect the runtime beside --exe (the identity lane), not from the framework")
    parser.add_argument("--package-name", default="FirefoxWinUI.Dev",
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
                                   env=dict(os.environ, MOZ_REMOTE_ALLOW_SYSTEM_ACCESS="1", FIREFOX_WINUI_PANE_DIAGNOSTICS="1"))
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
                        enabled: Services.prefs.getBoolPref('widget.windows.winui-prototype.pane.enabled'),
                        strip: Services.prefs.getBoolPref('widget.windows.winui-prototype.tabstrip.enabled'),
                        attached: window.gWinUIPane?.attached};""")
                    raise AssertionError(f"Packaged defaults did not attach the native strip: {details}")
                time.sleep(.1)
            state = chrome("""return {pid: Services.appinfo.processID,
                hwnd: window.docShell.treeOwner.QueryInterface(Ci.nsIBaseWindow).nativeHandle,
                profile: Services.dirsvc.get('ProfD', Ci.nsIFile).path,
                tabs: gBrowser.tabs.length};""")
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
            kernel.OpenProcess.restype = W.HANDLE
            kernel.CloseHandle.argtypes = [W.HANDLE]
            kernel.GetPackageFullName.argtypes = [W.HANDLE, ctypes.POINTER(W.UINT), W.LPWSTR]
            handle = kernel.OpenProcess(0x1000, False, state["pid"])
            if not handle:
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                size = W.UINT(0)
                result = kernel.GetPackageFullName(handle, ctypes.byref(size), None)
                if result == 15700:  # APPMODEL_ERROR_NO_PACKAGE
                    package = None
                elif result == 122:
                    buffer = ctypes.create_unicode_buffer(size.value)
                    result = kernel.GetPackageFullName(handle, ctypes.byref(size), buffer)
                    if result:
                        raise OSError(result, "GetPackageFullName")
                    package = buffer.value
                else:
                    raise OSError(result, "GetPackageFullName")
            finally:
                kernel.CloseHandle(handle)
            if not (package and package.startswith(f"{args.package_name}_")):
                raise AssertionError(f"Browser lacks expected package identity: {package}")
            runtime = runtime_modules(state["pid"], args.exe.resolve().parent if args.app_local_runtime else None)
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
                "exitCode": code, "scope": "installed identity"}, indent=2), encoding="utf-8")
            print(f"PASS: default native strip, {'app-local' if args.app_local_runtime else 'framework-only'} runtime ({len(runtime['modules'])} modules), "
                  f"UIA new tab, page JavaScript, clean exit; package={package}")
        finally:
            if client:
                client.sock.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
