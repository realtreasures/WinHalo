#requires -version 5.1
[CmdletBinding(DefaultParameterSetName = 'Development')]
param(
    [Parameter(Mandatory = $true)] [string] $ExePath,
    [string] $OutputDirectory,
    [string] $SdkBinDirectory = 'D:\Windows Kits\10\bin\10.0.26100.0\x64',
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')] [string] $Version = '0.1.0.0',
    [Parameter(Mandatory = $true, ParameterSetName = 'Development')] [switch] $DevelopmentCertificate,
    [Parameter(Mandatory = $true, ParameterSetName = 'Certificate')] [string] $CertificatePath,
    [Parameter(ParameterSetName = 'Certificate')] [Security.SecureString] $CertificatePassword
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot '..\..\artifacts\chat-package'
}
$packageName = 'WinHalo.NotificationIdentity'
$publisher = 'CN=WinHalo Development'
$applicationId = 'WinHalo'
# Keep the source script ASCII-compatible so Windows PowerShell 5.1 can read it without a UTF-8 BOM.
$readmeName = (-join [char[]](0x4F7F, 0x7528, 0x8BF4, 0x660E)) + '.txt'
$sourceExe = Get-Item -LiteralPath $ExePath
if ($sourceExe.PSIsContainer -or $sourceExe.Extension -ne '.exe') { throw 'ExePath must reference a built WinHalo executable.' }
$versionParts = ([Version] $Version)
if ($versionParts.Major -gt 65535 -or $versionParts.Minor -gt 65535 -or $versionParts.Build -gt 65535 -or
    $versionParts.Revision -gt 65535) {
    throw 'Each MSIX version component must fit an unsigned 16-bit integer.'
}
$sourceHash = (Get-FileHash -LiteralPath $sourceExe.FullName -Algorithm SHA256).Hash
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$iconPath = Join-Path $repo 'WinHalo.ico'
foreach ($name in @('makeappx.exe', 'signtool.exe', 'mt.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $SdkBinDirectory $name) -PathType Leaf)) { throw "Windows SDK tool missing: $name" }
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
$buildId = [Guid]::NewGuid().ToString('N')
$stage = Join-Path $output ('build-' + $buildId)
$release = Join-Path $stage 'WinHalo-Chat'
$content = Join-Path $stage 'identity-content'
$validation = Join-Path $stage 'validation-content'
foreach ($directory in @($release, $content, $validation)) { [IO.Directory]::CreateDirectory($directory) | Out-Null }
$makeappx = Join-Path $SdkBinDirectory 'makeappx.exe'
$signtool = Join-Path $SdkBinDirectory 'signtool.exe'
$mt = Join-Path $SdkBinDirectory 'mt.exe'

function Invoke-SdkTool([string] $Path, [string[]] $Arguments) {
    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Windows SDK tool failed ($LASTEXITCODE): $([IO.Path]::GetFileName($Path))" }
}

# Verify the existing executable's embedded identity; never inject into or sign the source executable.
$exeManifest = Join-Path $stage 'exe-embedded.manifest'
Invoke-SdkTool $mt @('-nologo', ('-inputresource:' + $sourceExe.FullName + ';#1'), ('-out:' + $exeManifest))
[xml] $embedded = [IO.File]::ReadAllText($exeManifest)
$ns = [Xml.XmlNamespaceManager]::new($embedded.NameTable)
$ns.AddNamespace('m', 'urn:schemas-microsoft-com:msix.v1')
$identity = $embedded.SelectSingleNode('//m:msix', $ns)
if ($null -eq $identity -or $identity.packageName -ne $packageName -or $identity.publisher -ne $publisher -or
    $identity.applicationId -ne $applicationId) {
    throw 'Rebuild WinHalo with the matching msix identity in WinHalo.manifest before packaging.'
}

Add-Type -AssemblyName System.Drawing
$assets = Join-Path $content 'Assets'
[IO.Directory]::CreateDirectory($assets) | Out-Null
$icon = [Drawing.Icon]::new($iconPath, [Drawing.Size]::new(256, 256))
$iconBitmap = $icon.ToBitmap()
try {
    foreach ($spec in @(@('StoreLogo.png', 50), @('Square150x150Logo.png', 150), @('Square44x44Logo.png', 44))) {
        $size = [int] $spec[1]
        $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.Clear([Drawing.Color]::Transparent)
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.DrawImage($iconBitmap, [Drawing.Rectangle]::new(0, 0, $size, $size))
            $bitmap.Save((Join-Path $assets $spec[0]), [Drawing.Imaging.ImageFormat]::Png)
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
} finally { $iconBitmap.Dispose(); $icon.Dispose() }
[xml] $manifest = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'AppxManifest.xml'))
$manifest.Package.Identity.Version = $Version
$manifest.Save((Join-Path $content 'AppxManifest.xml'))

$certificate = $null
$rsa = $null
$password = $null
$passwordMemory = [IntPtr]::Zero
try {
    if ($DevelopmentCertificate) {
        # Private material is isolated from release/ZIP and ACL-limited. This does not touch any Certificate Store.
        $private = Join-Path $output ('.private\' + $buildId)
        [IO.Directory]::CreateDirectory($private) | Out-Null
        $acl = [Security.AccessControl.DirectorySecurity]::new()
        $acl.SetAccessRuleProtection($true, $false)
        $currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
        $inherit = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
        foreach ($sid in @($currentSid, [Security.Principal.SecurityIdentifier]::new('S-1-5-18'))) {
            $rule = [Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', $inherit,
                [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
            $acl.AddAccessRule($rule)
        }
        Set-Acl -LiteralPath $private -AclObject $acl
        $random = New-Object byte[] 32
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $rng.GetBytes($random) } finally { $rng.Dispose() }
        $password = [Convert]::ToBase64String($random)
        [Array]::Clear($random, 0, $random.Length)
        # RSACng(size) creates an unnamed ephemeral key on both Windows PowerShell 5.1 and PowerShell 7.
        $rsa = [Security.Cryptography.RSACng]::new(3072)
        $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new($publisher, $rsa,
            [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
        $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
            [Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
        $eku = [Security.Cryptography.OidCollection]::new()
        [void] $eku.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
        $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $true))
        $certificate = $request.CreateSelfSigned([DateTimeOffset]::Now.AddMinutes(-5), [DateTimeOffset]::Now.AddDays(90))
        $pfxPath = Join-Path $private 'WinHalo.Development.pfx'
        [IO.File]::WriteAllBytes($pfxPath, $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $password))
        $secret = ConvertTo-SecureString -String $password -AsPlainText -Force
        # Windows DPAPI encryption is bound to this user; no plaintext password file is produced.
        ConvertFrom-SecureString -SecureString $secret | Set-Content -LiteralPath (Join-Path $private 'password.dpapi') -Encoding ASCII
    } else {
        $pfxPath = (Get-Item -LiteralPath $CertificatePath).FullName
        if ($null -ne $CertificatePassword) {
            $passwordMemory = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($CertificatePassword)
            $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordMemory)
        } else { $password = '' }
        # Read without persisting an imported private key into the current user's profile or stores.
        $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($pfxPath, $password,
            [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    }
    if (-not $certificate.HasPrivateKey -or $certificate.Subject -ne $publisher) {
        throw "Signing certificate must contain a private key and match the manifest publisher: $publisher"
    }
    if ($certificate.NotAfter -le [DateTime]::Now) { throw 'The signing certificate has expired.' }
    $publicCertificatePath = Join-Path $release 'WinHalo.Signing.cer'
    [IO.File]::WriteAllBytes($publicCertificatePath, $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    $releaseExe = Join-Path $release 'WinHalo.exe'
    Copy-Item -LiteralPath $sourceExe.FullName -Destination $releaseExe
    Invoke-SdkTool $signtool @('sign', '/fd', 'SHA256', '/f', $pfxPath, '/p', $password, $releaseExe)
    $signature = Get-AuthenticodeSignature -LiteralPath $releaseExe
    if ($null -eq $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
        throw 'The staged executable did not receive the expected Authenticode signature.'
    }
    # A complete layout validates XML, logos, capabilities and executable references with normal MakeAppx validation.
    Copy-Item -LiteralPath (Join-Path $content 'AppxManifest.xml') -Destination $validation
    Copy-Item -LiteralPath $assets -Destination $validation -Recurse
    Copy-Item -LiteralPath $releaseExe -Destination (Join-Path $validation 'WinHalo.exe')
    Invoke-SdkTool $makeappx @('pack', '/d', $validation, '/p', (Join-Path $stage 'validated-layout.msix'), '/o')
    $packagePath = Join-Path $release 'WinHalo.Notifications.msix'
    # The real identity package excludes the EXE. /nv is required for that external file reference (Microsoft sparse-package guidance).
    Invoke-SdkTool $makeappx @('pack', '/d', $content, '/p', $packagePath, '/nv', '/o')
    Invoke-SdkTool $signtool @('sign', '/fd', 'SHA256', '/f', $pfxPath, '/p', $password, $packagePath)
    $thumbprint = $certificate.Thumbprint
    $exeHash = (Get-FileHash -LiteralPath $releaseExe -Algorithm SHA256).Hash
    $packageHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
    foreach ($name in @('Install-Notifications.ps1', 'Uninstall-Notifications.ps1', $readmeName)) {
        $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot $name))
        $text = $text.Replace('__CERTIFICATE_THUMBPRINT__', $thumbprint).
            Replace('__DEVELOPMENT_CERTIFICATE__', ('$' + $DevelopmentCertificate.IsPresent.ToString().ToLowerInvariant())).
            Replace('__EXE_SHA256__', $exeHash).Replace('__PACKAGE_SHA256__', $packageHash)
        # BOM keeps Chinese readable in Windows PowerShell 5.1.
        [IO.File]::WriteAllText((Join-Path $release $name), $text, [Text.UTF8Encoding]::new($true))
    }
    $allowList = @('WinHalo.exe', 'WinHalo.Notifications.msix', 'WinHalo.Signing.cer',
        'Install-Notifications.ps1', 'Uninstall-Notifications.ps1', $readmeName)
    $files = @(Get-ChildItem -LiteralPath $release -File)
    if ($files.Count -ne $allowList.Count -or @($files | Where-Object { $_.Name -notin $allowList }).Count -ne 0 -or
        @(Get-ChildItem -LiteralPath $release -Directory).Count -ne 0) { throw 'Unexpected release contents.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipPath = Join-Path $output ('WinHalo-Chat-' + $Version + '-' + $buildId.Substring(0, 8) + '.zip')
    [IO.Compression.ZipFile]::CreateFromDirectory($release, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $false)
    if ((Get-FileHash -LiteralPath $sourceExe.FullName -Algorithm SHA256).Hash -ne $sourceHash) {
        throw 'The source executable changed during packaging. Do not distribute this output.'
    }
    Write-Host "ZIP: $zipPath"
    Write-Host "Release directory: $release"
    Write-Host "Public certificate thumbprint: $thumbprint"
    Write-Host 'Build only: no certificate trusted, no package registered, no permission requested.'
    if ($DevelopmentCertificate) { Write-Host "Private signing material remains outside the ZIP: $private" }
} finally {
    if ($null -ne $certificate) { $certificate.Dispose() }
    if ($null -ne $rsa) { $rsa.Dispose() }
    if ($passwordMemory -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordMemory) }
    $password = $null
}
