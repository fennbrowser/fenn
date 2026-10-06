param(
    # Default: one build step past the installed and newest built versions.
    [ValidatePattern('^\d+\.\d+\.\d+\.0$')]
    [string]$Version,
    [switch]$SkipFirefoxPackage,
    [switch]$KeepIntermediates,
    # Register the layout as FirefoxWinUI.DevLoose under Developer Mode instead of packing.
    [switch]$Loose,
    # Build the unsigned fallback (elevated -AllowUnsigned install) instead of signing.
    [switch]$Unsigned,
    [string]$SdkBin = 'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$mach = Join-Path $PSScriptRoot 'Invoke-Mach.ps1'
if (-not $SkipFirefoxPackage) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $mach package
    if ($LASTEXITCODE) { throw "Firefox packaging failed: $LASTEXITCODE" }
}
$packageArgs = @((Join-Path $PSScriptRoot 'Package-FirefoxDev.py'), '--sdk-bin', $SdkBin)
if ($Version) { $packageArgs += '--version', $Version }
if ($KeepIntermediates) { $packageArgs += '--keep-intermediates' }
if ($Loose) { $packageArgs += '--loose' }
if ($Unsigned) { $packageArgs += '--unsigned' }
& powershell -NoProfile -ExecutionPolicy Bypass -File $mach python --virtualenv build @packageArgs
if ($LASTEXITCODE) { throw "Development MSIX generation failed: $LASTEXITCODE" }
