param([switch]$NativeAddressBar, [switch]$IndependentAddress, [switch]$NativeNavigationToolbar, [switch]$NativeHistoryMenu, [switch]$NativeTabStrip, [switch]$ReplaceFirefoxTabStrip, [switch]$ContainerDots, [switch]$TabGlows, [switch]$GroupHalo, [switch]$GroupGlowAnimations, [switch]$GroupGlowFadeOut, [switch]$GroupHover, [switch]$CaptureCrash, [string]$DiagnosticsLog,
      [ValidateSet(32, 40, 48)][int]$NativeTabStripHeight,
      [ValidateSet('Mica', 'MicaAlt', 'Acrylic')][string]$TabStripBackdrop,
      [ValidateSet('None', 'Small', 'Medium', 'Large')][string]$TabGroupReach,
      # Run the build with package identity (Fenn.DevIdentity) through
      # dist/bin/fenn-identity.exe (scripts/Register-FennDevIdentity.py).
      [switch]$PackageIdentity)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$browser = Join-Path $projectRoot 'build/firefox/dist/bin/fenn.exe'
$pane = Join-Path (Split-Path $browser) 'FennPane.dll'
if (!(Test-Path $browser) -or !(Test-Path $pane)) {
    throw 'Build Firefox and run Build-WinUIPane.ps1 -Stage first.'
}
if ($PackageIdentity) {
    # Registers the identity package once and refreshes the marked executable after builds.
    & 'C:\mozilla-build\python3\python.exe' (Join-Path $PSScriptRoot 'Register-FennDevIdentity.py')
    if ($LASTEXITCODE) { throw "Package identity setup failed: $LASTEXITCODE" }
    $browser = Join-Path (Split-Path $browser) 'fenn-identity.exe'
}
$demoProfile = Join-Path $projectRoot ('artifacts/winui-demo/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $demoProfile | Out-Null
# Firefox's own defaults (browser/app/profile/firefox.js) are the project's
# defaults in every lane. Write a preference only for a flag that was passed,
# so -X:$false opts out and an omitted flag keeps the shared default.
if ($PSBoundParameters.ContainsKey('ReplaceFirefoxTabStrip')) {
    # Seed once in prefs.js so the toolbar option can persist changes on restart.
    "user_pref(`"fenn.winui.tabstrip.replaceFirefoxTabs`", $($ReplaceFirefoxTabStrip.IsPresent.ToString().ToLowerInvariant()));" |
        Set-Content (Join-Path $demoProfile 'prefs.js')
}
$userPrefs = @(
    'user_pref("browser.shell.checkDefaultBrowser", false);',
    'user_pref("browser.startup.homepage_override.mstone", "ignore");')
$switchPrefs = [ordered]@{
    NativeTabStrip = 'pane.enabled'; ContainerDots = 'tabstrip.containerDot'; TabGlows = 'tabstrip.glows'
    GroupHover = 'tabstrip.groupHover'; GroupHalo = 'tabstrip.groupHalo'
    GroupGlowAnimations = 'tabstrip.groupGlowAnimations'; GroupGlowFadeOut = 'tabstrip.groupGlowFadeOut'
    NativeAddressBar = 'address.toolbar.enabled'; IndependentAddress = 'address.independent'
    NativeHistoryMenu = 'navigation.history.menu.enabled'; NativeNavigationToolbar = 'navigation.toolbar.enabled'
}
# The native History menu lives on the native navigation toolbar.
if ($NativeHistoryMenu -and -not $PSBoundParameters.ContainsKey('NativeNavigationToolbar')) {
    $PSBoundParameters['NativeNavigationToolbar'] = [switch]$true
}
foreach ($flag in $switchPrefs.Keys) {
    if ($PSBoundParameters.ContainsKey($flag)) {
        $value = ([bool]$PSBoundParameters[$flag]).ToString().ToLowerInvariant()
        $userPrefs += "user_pref(`"fenn.winui.$($switchPrefs[$flag])`", $value);"
    }
}
if ($PSBoundParameters.ContainsKey('NativeTabStripHeight')) {
    $userPrefs += "user_pref(`"fenn.winui.tabstrip.height`", $NativeTabStripHeight);"
}
if ($PSBoundParameters.ContainsKey('TabStripBackdrop')) {
    $backdrop = @{ Mica = 'mica'; MicaAlt = 'mica-alt'; Acrylic = 'acrylic' }[$TabStripBackdrop]
    $userPrefs += "user_pref(`"fenn.winui.tabstrip.backdrop`", `"$backdrop`");"
}
if ($PSBoundParameters.ContainsKey('TabGroupReach')) {
    $userPrefs += "user_pref(`"fenn.winui.tabstrip.groupReach`", `"$($TabGroupReach.ToLowerInvariant())`");"
}
$userPrefs | Set-Content (Join-Path $demoProfile 'user.js')
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
if ($DiagnosticsLog) {
    # Pane diagnostics go to stderr; the browser inherits the redirected handle.
    # Scoped to the browser process: the script runs in the caller's session.
    $previous = $env:FENN_PANE_DIAGNOSTICS
    $env:FENN_PANE_DIAGNOSTICS = '1'
    $log = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DiagnosticsLog)
    Write-Host "Pane diagnostics: $log"
    try {
        Start-Process -FilePath $browser -NoNewWindow -Wait -RedirectStandardError $log -ArgumentList @(
            '--no-remote', '--profile', "`"$demoProfile`"", "`"$(Join-Path $projectRoot 'tests/smoke/browser.html')`"")
    } finally { $env:FENN_PANE_DIAGNOSTICS = $previous }
    exit
}
& $browser --no-remote --profile $demoProfile (Join-Path $projectRoot 'tests/smoke/browser.html')
