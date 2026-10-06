[CmdletBinding(SupportsShouldProcess)]
param([Parameter(Mandatory = $true)][string]$PackagePath)
$ErrorActionPreference = 'Stop'
$signedPublisher = 'CN=Firefox WinUI Development'
$unsignedPublisher = 'CN=Firefox WinUI Development, OID.2.25.311729368913984317654407730594956997722=1'
$package = (Resolve-Path -LiteralPath $PackagePath).Path
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($package)
try {
    $reader = [IO.StreamReader]::new($archive.GetEntry('AppxManifest.xml').Open())
    try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $signed = [bool]$archive.GetEntry('AppxSignature.p7x')
} finally { $archive.Dispose() }
$identity = $manifest.Package.Identity
$expectedPublisher = if ($signed) { $signedPublisher } else { $unsignedPublisher }
if ($identity.Name -ne 'FirefoxWinUI.Dev' -or $identity.Publisher -ne $expectedPublisher -or
    $identity.ProcessorArchitecture -ne 'x64') {
    throw 'This installer only accepts this project''s x64 development identity, signed or unsigned.'
}
# Signed and unsigned share the name and alias but are different package families.
# Replacing one with the other is an uninstall, which deletes that package's data.
$otherPublisher = if ($signed) { $unsignedPublisher } else { $signedPublisher }
$other = Get-AppxPackage -Name FirefoxWinUI.Dev | Where-Object Publisher -eq $otherPublisher
if ($other) {
    $kind = if ($signed) { 'unsigned' } else { 'signed' }
    throw ("The $kind FirefoxWinUI.Dev $($other.Version) is installed and shares this package's name and alias. " +
        'Removing it deletes its app data, including its browser profile; copy anything you need first. ' +
        "Then run: Get-AppxPackage -Name FirefoxWinUI.Dev | Where-Object Publisher -eq '$otherPublisher' | Remove-AppxPackage")
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
            Add-AppxPackage -Path $package
        } catch {
            throw ("$($_.Exception.Message) If Windows reports an untrusted certificate (0x800B0109), " +
                'run scripts\Initialize-DevSigning.ps1 once from an elevated PowerShell.')
        }
    } else {
        $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            throw 'Windows requires an elevated PowerShell for Add-AppxPackage -AllowUnsigned. Run this script there under your own account.'
        }
        Add-AppxPackage -Path $package -AllowUnsigned
    }
    Get-AppxPackage -Name FirefoxWinUI.Dev | Select-Object Name, Version, Publisher, PackageFamilyName, InstallLocation
}
