[CmdletBinding(SupportsShouldProcess)]
# An .msix installs that package. An .appinstaller feed (Build-FirefoxDevPackage.ps1
# -Publish) installs its package and makes the feed the installation's automatic
# update source.
param([Parameter(Mandatory = $true)][string]$PackagePath)
$ErrorActionPreference = 'Stop'
$signedPublisher = 'CN=Fenn Development'
$unsignedPublisher = 'CN=Fenn Development, OID.2.25.311729368913984317654407730594956997722=1'
$package = (Resolve-Path -LiteralPath $PackagePath).Path
$feed = $null
if ([IO.Path]::GetExtension($package) -eq '.appinstaller') {
    $feed = $package
    [xml]$feedXml = Get-Content -LiteralPath $feed -Raw
    $main = $feedXml.AppInstaller.MainPackage
    $package = ([Uri]$main.Uri).LocalPath
    if ($main.Name -ne 'Fenn.Dev' -or $main.Publisher -ne $signedPublisher) {
        throw 'This feed is not for the signed Fenn.Dev package.'
    }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($package)
try {
    $reader = [IO.StreamReader]::new($archive.GetEntry('AppxManifest.xml').Open())
    try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $signed = [bool]$archive.GetEntry('AppxSignature.p7x')
} finally { $archive.Dispose() }
$identity = $manifest.Package.Identity
$expectedPublisher = if ($signed) { $signedPublisher } else { $unsignedPublisher }
if ($identity.Name -ne 'Fenn.Dev' -or $identity.Publisher -ne $expectedPublisher -or
    $identity.ProcessorArchitecture -ne 'x64') {
    throw 'This installer only accepts this project''s x64 development identity, signed or unsigned.'
}
if ($feed -and $main.Version -ne $identity.Version) {
    throw "Feed names version $($main.Version) but its package is $($identity.Version)."
}
# Signed and unsigned share the name and alias but are different package families.
# Replacing one with the other is an uninstall, which deletes that package's data;
# the other family also has another Firefox install hash, so another profile.
$otherPublisher = if ($signed) { $unsignedPublisher } else { $signedPublisher }
$other = Get-AppxPackage -Name Fenn.Dev | Where-Object Publisher -eq $otherPublisher
if ($other) {
    $kind = if ($signed) { 'unsigned' } else { 'signed' }
    throw ("The $kind Fenn.Dev $($other.Version) is installed and shares this package's name and alias. " +
        'Removing it deletes its package data, and this package starts in a different browser profile; ' +
        'copy anything you need first. ' +
        "Then run: Get-AppxPackage -Name Fenn.Dev | Where-Object Publisher -eq '$otherPublisher' | Remove-AppxPackage")
}
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
    if ($signed) {
        try {
            if ($feed) { Add-AppxPackage -AppInstallerFile $feed } else { Add-AppxPackage -Path $package }
        } catch {
            if ($_.Exception.Message -match '0x800B0109') {
                throw ("$($_.Exception.Message) The signing certificate is not trusted; " +
                    'run scripts\Initialize-DevSigning.ps1 once from an elevated PowerShell.')
            }
            throw
        }
    } else {
        $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            throw 'Windows requires an elevated PowerShell for Add-AppxPackage -AllowUnsigned. Run this script there under your own account.'
        }
        Add-AppxPackage -Path $package -AllowUnsigned
    }
    Get-AppxPackage -Name Fenn.Dev | Select-Object Name, Version, Publisher, PackageFamilyName, InstallLocation
}
