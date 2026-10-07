param(
    # Default: one build step past the installed and newest built versions.
    [ValidatePattern('^\d+\.\d+\.\d+\.0$')]
    [string]$Version,
    # The clean Firefox payload is refreshed with `mach package` only when dist/bin
    # changed since the last one. -SkipFirefoxPackage never refreshes it;
    # -RepackageFirefox always does.
    [switch]$SkipFirefoxPackage,
    [switch]$RepackageFirefox,
    [switch]$KeepIntermediates,
    # Register the layout as Fenn.DevLoose under Developer Mode instead of packing.
    [switch]$Loose,
    # Build the unsigned fallback (elevated -AllowUnsigned install) instead of signing.
    [switch]$Unsigned,
    # Build the unsigned release package (FennBrowser.FennBrowser) for upload to
    # Partner Center, in artifacts/msix-store. The Store signs it; it is not installable here.
    [switch]$Store,
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
if ($SkipFirefoxPackage -and $RepackageFirefox) { throw 'Use -SkipFirefoxPackage or -RepackageFirefox, not both.' }
if ($Store -and ($Loose -or $Unsigned -or $Publish -or $Install -or $Smoke)) {
    throw '-Store builds an upload package only; it cannot be combined with -Loose, -Unsigned, -Publish, -Install or -Smoke.'
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$mach = Join-Path $PSScriptRoot 'Invoke-Mach.ps1'
$python = 'C:\mozilla-build\python3\python.exe'
$packager = Join-Path $PSScriptRoot 'Package-FirefoxDev.py'
$started = Get-Date

$repackage = [bool]$RepackageFirefox
if (-not $SkipFirefoxPackage -and -not $RepackageFirefox) {
    & $python $packager --payload-status
    if ($LASTEXITCODE -eq 10) { $repackage = $true }
    elseif ($LASTEXITCODE) { throw "Firefox payload check failed: $LASTEXITCODE" }
}
if ($repackage) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $mach package
    if ($LASTEXITCODE) { throw "Firefox packaging failed: $LASTEXITCODE" }
}
$packageArgs = @($packager, '--sdk-bin', $SdkBin)
if ($Version) { $packageArgs += '--version', $Version }
if ($KeepIntermediates) { $packageArgs += '--keep-intermediates' }
if ($Loose) { $packageArgs += '--loose' }
if ($Unsigned) { $packageArgs += '--unsigned' }
if ($Store) { $packageArgs += '--store' }
if ($Publish) { $packageArgs += '--publish' }
& powershell -NoProfile -ExecutionPolicy Bypass -File $mach python --virtualenv build @packageArgs
if ($LASTEXITCODE) { throw "MSIX generation failed: $LASTEXITCODE" }
if (-not ($Install -or $Smoke)) { return }

# The packager records what it just produced; never act on an older build's record.
$runs = Join-Path $projectRoot $(if ($Loose) { 'artifacts/msix-loose' } else { 'artifacts/msix-dev' })
$record = Get-Item (Join-Path $runs 'latest.json')
if ($record.LastWriteTime -lt $started) { throw "No new build record in $runs" }
$latest = Get-Content $record.FullName -Raw | ConvertFrom-Json
if ($Install -and -not $latest.loose) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Install-FirefoxDevPackage.ps1') `
        -PackagePath $(if ($latest.feed) { $latest.feed } else { $latest.package })
    if ($LASTEXITCODE) { throw "Installing $($latest.package) failed: $LASTEXITCODE" }
}
if ($Smoke) {
    $installed = @(Get-AppxPackage -Name $latest.identity | Where-Object Version -eq $latest.version)
    if (-not $installed) { throw "$($latest.identity) $($latest.version) is not installed; add -Install." }
    & $python (Join-Path $PSScriptRoot 'Test-FirefoxDevPackage.py') `
        --exe (Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\$($latest.alias)") --package-name $latest.identity
    if ($LASTEXITCODE) { throw "Smoke test of $($latest.identity) $($latest.version) failed: $LASTEXITCODE" }
}
