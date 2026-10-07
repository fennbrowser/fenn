# Fenn

Fenn is a web browser for Windows, based on Mozilla Firefox, that hosts native
WinUI 3 controls (tabs, toolbar and address bar) inside Firefox's window while
keeping Firefox responsible for browsing, security and policy.

**Status: pre-release.** There is no release build yet. This repository is
published from the project's development repository; its history is a series
of snapshots.

## What is here

| Path | Contents |
| --- | --- |
| `patches/` | Changes applied to a pinned Firefox revision (`configs/toolchain-baseline.json`). |
| `native/` | The native WinUI pane (C++/WinRT) and the Rust shell core. |
| `bridge/` | The C ABI headers between Firefox and the native code. |
| `branding/` | Fenn's branding directory, copied into the Firefox tree before each build. |
| `configs/` | The mozconfig, toolchain pins and resource-index configuration. |
| `packaging/` | Defaults for development MSIX packages. |
| `scripts/` | Bootstrap, build, packaging and staging scripts. |
| `tests/` | Unit and contract tests. |

## Building

Building needs Windows 11 x64, Visual Studio with the C++ and Windows SDK
workloads (`configs/visualstudio.vsconfig`), MozillaBuild, Rust and the Windows
App SDK runtime (`scripts/Install-WindowsAppRuntime.ps1`). In outline:

1. `python scripts/Bootstrap-Firefox.py prepare` checks out the pinned Firefox
   revision into `upstream/firefox` and applies the patches.
2. `scripts/Build-WinUIPane.ps1` builds the native pane.
3. `scripts/Invoke-Mach.ps1 build` builds Firefox with Fenn's branding
   (`configs/mozconfig`).
4. `scripts/Build-FirefoxDevPackage.ps1` builds a development MSIX package.

## License

Mozilla Public License 2.0. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).
