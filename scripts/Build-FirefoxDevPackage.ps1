param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+\.0$')]
    [string]$Version,
    [switch]$SkipFirefoxPackage,
    [switch]$KeepIntermediates,
    [string]$SdkBin = 'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$mach = Join-Path $PSScriptRoot 'Invoke-Mach.ps1'
if (-not $SkipFirefoxPackage) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $mach package
    if ($LASTEXITCODE) { throw "Firefox packaging failed: $LASTEXITCODE" }
}
$packageArgs = @((Join-Path $PSScriptRoot 'Package-FirefoxDev.py'), '--version', $Version, '--sdk-bin', $SdkBin)
if ($KeepIntermediates) { $packageArgs += '--keep-intermediates' }
& powershell -NoProfile -ExecutionPolicy Bypass -File $mach python --virtualenv build @packageArgs
if ($LASTEXITCODE) { throw "Development MSIX generation failed: $LASTEXITCODE" }
