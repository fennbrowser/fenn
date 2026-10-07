<#
.SYNOPSIS
Installs the shared Windows App SDK runtime the pane needs, for the current user.

.DESCRIPTION
Every lane runs the pane on Microsoft.WindowsAppRuntime.2, with no app-local
runtime. An unpackaged Firefox, the native
contract-test harness and CI join it through the bootstrapper, which also needs
the runtime's Main and DDLM packages. This installs all three from the
Microsoft-signed MSIX files in the pane build's NuGet cache
(Microsoft.WindowsAppSDK.Runtime.<SDK version>, restored by
Build-WinUIPane.ps1 -Restore), framework first. A package already installed at
the same or a newer version is left alone. No elevation is needed.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet('x64', 'x86', 'arm64')][string]$Architecture = 'x64',
    [string]$PackagesDirectory
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if (-not $PackagesDirectory) { $PackagesDirectory = Join-Path $root '.cache\nuget\native' }
[xml]$packagesConfig = Get-Content -Raw -LiteralPath (Join-Path $root 'native\WinUIPane\packages.config')
$sdkVersion = $packagesConfig.SelectSingleNode("/packages/package[@id='Microsoft.WindowsAppSDK']").GetAttribute('version')
$msixDirectory = Join-Path $PackagesDirectory "Microsoft.WindowsAppSDK.Runtime.$sdkVersion\tools\MSIX\win10-$Architecture"
if (-not (Test-Path -LiteralPath $msixDirectory)) { throw "Missing $msixDirectory; restore the pane's NuGet packages first." }

Add-Type -AssemblyName System.IO.Compression.FileSystem
function Get-MsixIdentity([string]$Path) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $reader = [IO.StreamReader]::new($archive.GetEntry('AppxManifest.xml').Open())
        try { ([xml]$reader.ReadToEnd()).Package.Identity } finally { $reader.Dispose() }
    } finally { $archive.Dispose() }
}

# Main and DDLM depend on the framework, so it goes first.
foreach ($file in 'Microsoft.WindowsAppRuntime.2.msix', 'Microsoft.WindowsAppRuntime.Main.2.msix',
                  'Microsoft.WindowsAppRuntime.DDLM.2.msix') {
    $path = Join-Path $msixDirectory $file
    $identity = Get-MsixIdentity $path
    $present = Get-AppxPackage -Name $identity.Name | Where-Object {
        "$($_.Architecture)" -eq $identity.ProcessorArchitecture -and [version]$_.Version -ge [version]$identity.Version }
    if ($present) {
        Write-Host "Present: $($present[0].PackageFullName)"
    } elseif ($PSCmdlet.ShouldProcess("$($identity.Name) $($identity.Version) $($identity.ProcessorArchitecture)", 'Install')) {
        Add-AppxPackage -Path $path
        Write-Host "Installed: $($identity.Name) $($identity.Version)"
    }
}
