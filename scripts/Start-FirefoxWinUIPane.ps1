param([switch]$NativeAddress, [switch]$NativeTabStrip, [switch]$ContainerDots, [switch]$CaptureCrash,
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
if ($ContainerDots) { Add-Content (Join-Path $demoProfile 'user.js') 'user_pref("widget.windows.winui-prototype.tabstrip.containerDot", true);' }
if ($NativeAddress) { Add-Content (Join-Path $demoProfile 'user.js') 'user_pref("widget.windows.winui-prototype.address.enabled", true);' }
Write-Host "Starting the WinUI prototype with an isolated profile: $demoProfile"
if ($CaptureCrash) {
    $debugger = Join-Path $projectRoot '.cache/mozbuild/vs/Windows Kits/10/Debuggers/x64/cdb.exe'
    if (!(Test-Path $debugger)) { throw "Crash capture requires the project debugger: $debugger" }
    Write-Host "Full crash dumps: $(Join-Path $demoProfile 'crash-full*.dmp')"
    # Process-scoped capture; do not change Windows-wide WER settings. Relative
    # debugger output paths also avoid nested quoting inside its event command.
    Push-Location $demoProfile
    try {
        # Firefox's launcher starts the browser as a child process.
        # Run setup at the initial breakpoint, before browser interaction. Also
        # capture unhandled access violations; not every XAML failure is stowed.
        & $debugger -hd -o -G -logo debugger.log -c 'sxd -c2 ".dump /ma /u crash-full.dmp; gn" av; sxe -c ".dump /ma /u crash-full.dmp; gn" bpe; sxe -c ".dump /ma /u crash-full.dmp; gn" -c2 ".dump /ma /u crash-full.dmp; gn" 0xc0000374; sxe -c ".dump /ma /u crash-full.dmp; gn" -c2 ".dump /ma /u crash-full.dmp; gn" 0xc000027b; sxi ibp; g' `
            $browser --no-remote --profile $demoProfile (Join-Path $projectRoot 'tests/smoke/browser.html')
    } finally { Pop-Location }
    exit $LASTEXITCODE
}
& $browser --no-remote --profile $demoProfile (Join-Path $projectRoot 'tests/smoke/browser.html')
