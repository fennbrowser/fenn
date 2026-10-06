[CmdletBinding(SupportsShouldProcess)]
param([Parameter(Mandatory = $true)][string]$PackagePath)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackagePath).Path
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($package)
try {
    $reader = [IO.StreamReader]::new($archive.GetEntry('AppxManifest.xml').Open())
    try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $identity = $manifest.Package.Identity
    if ($identity.Name -ne 'FirefoxWinUI.Dev' -or
        $identity.Publisher -ne 'CN=Firefox WinUI Development, OID.2.25.311729368913984317654407730594956997722=1' -or
        $identity.ProcessorArchitecture -ne 'x64') {
        throw 'This installer only accepts this project''s unsigned x64 development identity.'
    }
} finally { $archive.Dispose() }
# The package carries no Windows App SDK runtime. Sideloading does not fetch the
# framework, so name the missing dependency instead of failing deep in deployment.
foreach ($dependency in @($manifest.Package.Dependencies.PackageDependency)) {
    $installed = Get-AppxPackage -Name $dependency.Name | Where-Object {
        $_.Architecture -eq 'X64' -and $_.Publisher -eq $dependency.Publisher -and
        [version]$_.Version -ge [version]$dependency.MinVersion }
    if (-not $installed) {
        throw "Install the x64 $($dependency.Name) framework >= $($dependency.MinVersion) (Windows App Runtime) first."
    }
}
if ($PSCmdlet.ShouldProcess("$($identity.Name) $($identity.Version)", 'Install development MSIX for current user')) {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Windows requires an elevated PowerShell for Add-AppxPackage -AllowUnsigned. Run this script there under your own account.'
    }
    Add-AppxPackage -Path $package -AllowUnsigned
    Get-AppxPackage -Name FirefoxWinUI.Dev | Select-Object Name, Version, PackageFamilyName, InstallLocation
}
