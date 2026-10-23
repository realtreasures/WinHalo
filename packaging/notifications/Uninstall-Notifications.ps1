#requires -version 5.1
[CmdletBinding()]
param(
    [switch] $RemoveDevelopmentCertificate,
    [switch] $RemoveMachineDevelopmentCertificate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$expectedThumbprint = '__CERTIFICATE_THUMBPRINT__'
$developmentCertificate = __DEVELOPMENT_CERTIFICATE__
$packageName = 'WinHalo.NotificationIdentity'
$publisher = 'CN=WinHalo Development'
if ($expectedThumbprint.StartsWith('__')) { throw 'Run the build script first; this is an unbound uninstaller template.' }
$certificate = $null
try {
    if ($RemoveDevelopmentCertificate -or $RemoveMachineDevelopmentCertificate) {
        if (-not $developmentCertificate) { throw 'Certificate removal is only supported for a self-signed development build.' }
        $publicPath = Join-Path $PSScriptRoot 'WinHalo.Signing.cer'
        $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($publicPath)
        if ($certificate.Thumbprint -ne $expectedThumbprint -or $certificate.Subject -ne $publisher -or
            $certificate.Issuer -ne $publisher -or $certificate.HasPrivateKey) {
            throw 'The public development certificate does not match this release.'
        }
        if ($RemoveMachineDevelopmentCertificate) {
            $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
            if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
                throw '-RemoveMachineDevelopmentCertificate requires an already elevated PowerShell session. Nothing was removed; this script never launches UAC.'
            }
        }
    }
    # Validate requested certificate scope before removing anything. Exact package
    # name and publisher only; no other packages, executables or preferences removed.
    $packages = @(Get-AppxPackage -Name $packageName | Where-Object { $_.Name -eq $packageName -and $_.Publisher -eq $publisher })
    foreach ($package in $packages) { Remove-AppxPackage -Package $package.PackageFullName }
    if ($RemoveDevelopmentCertificate -or $RemoveMachineDevelopmentCertificate) {
        $stores = @()
        if ($RemoveDevelopmentCertificate) { $stores += 'Cert:\CurrentUser\TrustedPeople' }
        if ($RemoveMachineDevelopmentCertificate) { $stores += 'Cert:\LocalMachine\TrustedPeople' }
        foreach ($store in $stores) {
            $trusted = Get-ChildItem -LiteralPath $store |
                Where-Object { $_.Thumbprint -eq $expectedThumbprint -and $_.Subject -eq $publisher -and $_.Issuer -eq $publisher }
            foreach ($item in $trusted) { Remove-Item -LiteralPath $item.PSPath }
        }
    }
    Write-Host 'WinHalo identity removed for the current user. Application files and user settings were retained.'
} finally { if ($null -ne $certificate) { $certificate.Dispose() } }
