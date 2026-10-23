#requires -version 5.1
[CmdletBinding()]
param(
    [switch] $TrustDevelopmentCertificate,
    [switch] $TrustMachineDevelopmentCertificate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# These values are fixed by Build-NotificationPackage.ps1 for this exact distribution.
$expectedThumbprint = '__CERTIFICATE_THUMBPRINT__'
$developmentCertificate = __DEVELOPMENT_CERTIFICATE__
$expectedExeHash = '__EXE_SHA256__'
$expectedPackageHash = '__PACKAGE_SHA256__'
$packageName = 'WinHalo.NotificationIdentity'
$publisher = 'CN=WinHalo Development'

if ($TrustDevelopmentCertificate -and $TrustMachineDevelopmentCertificate) {
    throw 'Choose one certificate-trust scope. Machine trust does not require a second current-user import.'
}
if ($expectedThumbprint.StartsWith('__')) { throw 'Run the build script first; this is an unbound installer template.' }
if ([Environment]::OSVersion.Version -lt [Version]'10.0.19041.0') { throw 'Windows 10 build 19041 or later is required.' }
if (-not [Environment]::Is64BitOperatingSystem) { throw 'This build requires 64-bit Windows.' }
$location = (Get-Item -LiteralPath $PSScriptRoot).FullName
$exePath = Join-Path $location 'WinHalo.exe'
$packagePath = Join-Path $location 'WinHalo.Notifications.msix'
$certificatePath = Join-Path $location 'WinHalo.Signing.cer'
foreach ($item in @($exePath, $packagePath, $certificatePath)) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf)) { throw "Missing release file: $item" }
}
if ((Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash -ne $expectedExeHash -or
    (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash -ne $expectedPackageHash) {
    throw 'Release files changed after packaging. Download or build a complete package again.'
}
$installed = @(Get-AppxPackage -Name $packageName | Where-Object { $_.Name -eq $packageName -and $_.Publisher -eq $publisher })
if ($installed.Count -gt 0) {
    throw 'This identity is already registered. Close WinHalo, run its Uninstall-Notifications.ps1, then install the new build.'
}
$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
try {
    if ($certificate.Thumbprint -ne $expectedThumbprint -or $certificate.Subject -ne $publisher -or $certificate.HasPrivateKey) {
        throw 'The public signing certificate does not match this release.'
    }
    if ($certificate.NotAfter -le [DateTime]::Now) { throw 'The signing certificate has expired; use a new build.' }
    if ($TrustDevelopmentCertificate -or $TrustMachineDevelopmentCertificate) {
        if (-not $developmentCertificate -or $certificate.Issuer -ne $certificate.Subject) {
            throw 'Certificate-trust switches are only valid for this build''s self-signed development certificate.'
        }
    }
    # MSIX deployment checks machine trust. CurrentUser\TrustedPeople alone failed
    # with 0x800B0109 on the test machine. Check before importing into either store.
    $machineTrusted = @(Get-ChildItem -LiteralPath 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq $expectedThumbprint -and $_.Subject -eq $publisher })
    if ($TrustMachineDevelopmentCertificate) {
        $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            throw '-TrustMachineDevelopmentCertificate requires an already elevated PowerShell session. No certificate was imported; this script never launches UAC.'
        }
    }
    if ($developmentCertificate -and $machineTrusted.Count -eq 0 -and -not $TrustMachineDevelopmentCertificate) {
        throw 'This self-signed MSIX needs its matching public certificate in LocalMachine\TrustedPeople. CurrentUser\TrustedPeople is insufficient (0x800B0109); no certificate was imported. After explicit approval, an administrator may run this installer with -TrustMachineDevelopmentCertificate, or import this bundle''s WinHalo.Signing.cer into LocalMachine\TrustedPeople and then let the intended user rerun this installer without trust switches. Never use the Root store.'
    }
    if ($TrustMachineDevelopmentCertificate -and $machineTrusted.Count -eq 0) {
        # Explicit machine-wide trust of this exact public certificate only.
        # The caller must already be elevated; no automatic elevation or Root import.
        Import-Certificate -FilePath $certificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
        Write-Host "Trusted development certificate for this machine: $expectedThumbprint"
    }
    if ($TrustDevelopmentCertificate) {
        # Legacy opt-in remains current-user only; machine trust was checked above.
        $existing = Get-ChildItem -LiteralPath 'Cert:\CurrentUser\TrustedPeople' |
            Where-Object { $_.Thumbprint -eq $expectedThumbprint }
        if (-not $existing) {
            Import-Certificate -FilePath $certificatePath -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople' | Out-Null
            Write-Host "Trusted development certificate for the current user: $expectedThumbprint"
        }
    }
    # Only known image entries are extracted. Never unpack executable or script entries from an MSIX.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
    try {
        $entry = $archive.GetEntry('AppxManifest.xml')
        if ($null -eq $entry) { throw 'The identity package has no manifest.' }
        $reader = [System.IO.StreamReader]::new($entry.Open())
        try { [xml] $manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
        if ($manifest.Package.Identity.Name -ne $packageName -or $manifest.Package.Identity.Publisher -ne $publisher -or
            $manifest.Package.Applications.Application.Id -ne 'WinHalo') { throw 'Unexpected identity package.' }
        $assets = Join-Path $location 'Assets'
        if (Test-Path -LiteralPath $assets) {
            if ((Get-Item -LiteralPath $assets).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Assets must be a regular local directory.'
            }
        } else { [IO.Directory]::CreateDirectory($assets) | Out-Null }
        foreach ($name in @('StoreLogo.png', 'Square150x150Logo.png', 'Square44x44Logo.png')) {
            $image = $archive.GetEntry('Assets/' + $name)
            if ($null -eq $image -or $image.Length -gt 1048576) { throw "Missing or invalid package image: $name" }
            $destination = Join-Path $assets $name
            if ((Test-Path -LiteralPath $destination) -and
                ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Refusing to overwrite an image link: $destination"
            }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($image, $destination, $true)
        }
    } finally { $archive.Dispose() }
    Add-AppxPackage -Path $packagePath -ExternalLocation $location
    Write-Host 'WinHalo identity registered for the current user. No notification permission or startup setting was changed.'
    Write-Host 'Start WinHalo.exe, open Settings, then enable chat messages and request notification access there.'
} finally { $certificate.Dispose() }
