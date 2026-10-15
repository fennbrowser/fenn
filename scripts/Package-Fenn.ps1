param(
    # Default: one build step past the installed and newest built versions.
    [ValidatePattern('^\d+\.\d+\.\d+\.0$')]
    [string]$Version,
    # The clean Firefox payload is refreshed with `mach package` only when dist/bin
    # changed since the last one. -ReusePayload never refreshes it;
    # -RefreshPayload always does.
    [switch]$ReusePayload,
    [switch]$RefreshPayload,
    [switch]$KeepIntermediates,
    # Register the layout as Fenn.DevLoose under Developer Mode instead of packing.
    [switch]$RegisterInPlace,
    # Build the unsigned fallback (elevated -AllowUnsigned install) instead of signing.
    [switch]$Unsigned,
    # Build the unsigned release package (FennBrowser.FennBrowser) for upload to
    # Partner Center, in artifacts/msix-store. The Store signs it; it is not installable here.
    [switch]$Store,
    # Package the PGO/LTO release build (build/firefox-release, configs/mozconfig.release)
    # in a development lane, to install and test the build that ships. -Store always does.
    [switch]$ReleaseBuild,
    # Publish the signed package to the local App Installer feed (artifacts/msix-feed);
    # an installation made from the feed then updates itself from it.
    [switch]$Publish,
    # Install the new package (the loose lane registers itself), then smoke-test it.
    # With -Publish, -Install installs through the feed.
    [switch]$Install,
    [switch]$Smoke,
    [string]$SdkBin = 'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64'
)
$ErrorActionPreference = 'Stop'
if ($ReusePayload -and $RefreshPayload) { throw 'Use -ReusePayload or -RefreshPayload, not both.' }
if ($Store -and ($RegisterInPlace -or $Unsigned -or $Publish -or $Install -or $Smoke)) {
    throw '-Store builds an upload package only; it cannot be combined with -RegisterInPlace, -Unsigned, -Publish, -Install or -Smoke.'
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$mach = Join-Path $PSScriptRoot 'Invoke-Mach.ps1'
$python = 'C:\mozilla-build\python3\python.exe'
$packager = Join-Path $PSScriptRoot 'Package-FennDev.py'
$started = Get-Date
# Development builds iterate; only the release build is optimized for shipping.
$release = $Store -or $ReleaseBuild
$buildArgs = @()
if ($Store) { $buildArgs += '--store' }
elseif ($ReleaseBuild) { $buildArgs += '--release-build' }

$repackage = [bool]$RefreshPayload
if (-not $ReusePayload -and -not $RefreshPayload) {
    & $python $packager --payload-status @buildArgs
    if ($LASTEXITCODE -eq 10) { $repackage = $true }
    elseif ($LASTEXITCODE) { throw "Firefox payload check failed: $LASTEXITCODE" }
}
if ($repackage) {
    # mach.sh selects the mozconfig, and with it the object directory, from FENN_MOZCONFIG.
    $previousMozconfig = $env:FENN_MOZCONFIG
    $env:FENN_MOZCONFIG = $(if ($release) { 'mozconfig.release' } else { 'mozconfig' })
    try { & powershell -NoProfile -ExecutionPolicy Bypass -File $mach package }
    finally { $env:FENN_MOZCONFIG = $previousMozconfig }
    if ($LASTEXITCODE) { throw "Firefox packaging failed: $LASTEXITCODE" }
}
$packageArgs = @($packager, '--sdk-bin', $SdkBin) + $buildArgs
if ($Version) { $packageArgs += '--version', $Version }
if ($KeepIntermediates) { $packageArgs += '--keep-intermediates' }
if ($RegisterInPlace) { $packageArgs += '--loose' }
if ($Unsigned) { $packageArgs += '--unsigned' }
if ($Publish) { $packageArgs += '--publish' }
& powershell -NoProfile -ExecutionPolicy Bypass -File $mach python --virtualenv build @packageArgs
if ($LASTEXITCODE) { throw "MSIX generation failed: $LASTEXITCODE" }
if (-not ($Install -or $Smoke)) { return }

# The packager records what it just produced; never act on an older build's record.
$runs = Join-Path $projectRoot $(if ($RegisterInPlace) { 'artifacts/msix-loose' } else { 'artifacts/msix-dev' })
$record = Get-Item (Join-Path $runs 'latest.json')
if ($record.LastWriteTime -lt $started) { throw "No new build record in $runs" }
$latest = Get-Content $record.FullName -Raw | ConvertFrom-Json
if ($Install -and -not $latest.loose) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Install-FennDevPackage.ps1') `
        -PackagePath $(if ($latest.feed) { $latest.feed } else { $latest.package })
    if ($LASTEXITCODE) { throw "Installing $($latest.package) failed: $LASTEXITCODE" }
}
if ($Smoke) {
    $installed = @(Get-AppxPackage -Name $latest.identity | Where-Object Version -eq $latest.version)
    if (-not $installed) { throw "$($latest.identity) $($latest.version) is not installed; add -Install." }
    & $python (Join-Path $PSScriptRoot 'Test-FennDevPackage.py') `
        --exe (Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\$($latest.alias)") --package-name $latest.identity
    if ($LASTEXITCODE) { throw "Smoke test of $($latest.identity) $($latest.version) failed: $LASTEXITCODE" }
}
