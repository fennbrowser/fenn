# One-time, elevated setup for signed development MSIX packages.
#
# Creates (or reuses) a self-signed code-signing certificate whose subject is
# the signed development publisher, with its private key non-exportable in your
# CurrentUser\My store, and trusts only its public certificate in
# LocalMachine\TrustedPeople. After this, Package-Fenn.ps1 signs the
# MSIX and Install-FennDevPackage.ps1 installs and updates it without
# elevation. Run from an elevated PowerShell under your own account, so the key
# lands in your user store. -Remove deletes both again.
[CmdletBinding(SupportsShouldProcess)]
param([switch]$Remove, [int]$ValidYears = 2)
$ErrorActionPreference = 'Stop'
$subject = 'CN=Fenn Development'
$friendlyName = 'Fenn development package signing'
$codeSigning = '1.3.6.1.5.5.7.3.3'

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Trusting the certificate machine-wide needs an elevated PowerShell. Run this once there under your own account.'
}

$owned = @(Get-ChildItem Cert:\CurrentUser\My | Where-Object {
    $_.Subject -eq $subject -and $_.FriendlyName -eq $friendlyName })
$trusted = @(Get-ChildItem Cert:\LocalMachine\TrustedPeople | Where-Object {
    $_.Subject -eq $subject -and $_.FriendlyName -eq $friendlyName })

if ($Remove) {
    foreach ($certificate in $owned + $trusted) {
        if ($PSCmdlet.ShouldProcess("$($certificate.PSParentPath) $($certificate.Thumbprint)", 'Remove certificate')) {
            Remove-Item -LiteralPath $certificate.PSPath
        }
    }
    return
}

$certificate = $owned | Where-Object {
    $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(30) -and
    $_.EnhancedKeyUsageList.ObjectId -contains $codeSigning } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $certificate) {
    if (-not $PSCmdlet.ShouldProcess($subject, 'Create self-signed code-signing certificate in CurrentUser\My')) { return }
    $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -FriendlyName $friendlyName -CertStoreLocation Cert:\CurrentUser\My `
        -KeyExportPolicy NonExportable -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 3072 `
        -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears($ValidYears)
}
if (-not ($trusted.Thumbprint -contains $certificate.Thumbprint)) {
    if ($PSCmdlet.ShouldProcess("$subject $($certificate.Thumbprint)", 'Trust public certificate in LocalMachine\TrustedPeople')) {
        $store = [Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'LocalMachine')
        $store.Open('ReadWrite')
        try {
            # Public part only: the private key never leaves CurrentUser\My.
            $public = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificate.RawData)
            $public.FriendlyName = $friendlyName
            $store.Add($public)
        } finally { $store.Close() }
    }
}
[pscustomobject]@{ Subject = $certificate.Subject; Thumbprint = $certificate.Thumbprint; NotAfter = $certificate.NotAfter }
