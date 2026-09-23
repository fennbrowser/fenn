param([switch]$NativeAddress, [switch]$NativeTabStrip,
      [ValidateSet(32, 40, 48)][int]$NativeTabStripHeight = 40)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$browser = Join-Path $projectRoot 'build/firefox/dist/bin/firefox.exe'
$pane = Join-Path (Split-Path $browser) 'FirefoxWinUIPane.dll'
if (!(Test-Path $browser) -or !(Test-Path $pane)) {
    throw 'Build Firefox and run Build-WinUIPane.ps1 -Stage first.'
}
$demoProfile = Join-Path $projectRoot ('artifacts/winui-demo/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $demoProfile | Out-Null
@'
user_pref("widget.windows.winui-prototype.pane.enabled", true);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
'@ | Set-Content (Join-Path $demoProfile 'user.js')
if ($NativeTabStrip) { Add-Content (Join-Path $demoProfile 'user.js') 'user_pref("widget.windows.winui-prototype.tabstrip.enabled", true);' }
Add-Content (Join-Path $demoProfile 'user.js') "user_pref(`"widget.windows.winui-prototype.tabstrip.height`", $NativeTabStripHeight);"
if ($NativeAddress) { Add-Content (Join-Path $demoProfile 'user.js') 'user_pref("widget.windows.winui-prototype.address.enabled", true);' }
Write-Host "Starting the WinUI prototype with an isolated profile: $demoProfile"
& $browser --no-remote --profile $demoProfile (Join-Path $projectRoot 'tests/smoke/browser.html')
