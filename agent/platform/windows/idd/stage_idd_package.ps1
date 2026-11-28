#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DllPath,
    [Parameter(Mandatory)][string]$InfPath,
    [Parameter(Mandatory)][string]$StageDir,
    [Parameter(Mandatory)][string]$Inf2CatExe,
    [Parameter(Mandatory)][string]$SignToolExe,
    [string]$SignSha1 = ''
)
$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
$fullStage = [IO.Path]::GetFullPath($StageDir)
foreach ($stale in @('ultraterimal_idd.dll', 'ultraterimal_idd.inf', 'ultraterimal_idd.cat', 'ultraterimal_idd.cer')) {
    Remove-Item -LiteralPath (Join-Path $fullStage $stale) -Force -ErrorAction SilentlyContinue
}

$stagedDll = Join-Path $fullStage 'ultraterimal_idd.dll'
$stagedInf = Join-Path $fullStage 'ultraterimal_idd.inf'
$stagedCat = Join-Path $fullStage 'ultraterimal_idd.cat'
$stagedCer = Join-Path $fullStage 'ultraterimal_idd.cer'

Copy-Item -LiteralPath $DllPath -Destination $stagedDll -Force
Copy-Item -LiteralPath $InfPath -Destination $stagedInf -Force

if ($SignSha1) {
    & $SignToolExe sign /fd SHA256 /sha1 $SignSha1 $stagedDll
    if ($LASTEXITCODE -ne 0) { throw 'Failed to sign ultraterimal_idd.dll with provided SHA1 thumbprint' }
    & $Inf2CatExe "/driver:$fullStage" /os:10_VB_X64
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $stagedCat)) { throw 'Inf2Cat failed to generate ultraterimal_idd.cat' }
    & $SignToolExe sign /fd SHA256 /sha1 $SignSha1 $stagedCat
    if ($LASTEXITCODE -ne 0) { throw 'Failed to sign ultraterimal_idd.cat with provided SHA1 thumbprint' }
    $existingCert = Get-ChildItem -Path Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -ieq $SignSha1 } | Select-Object -First 1
    if ($existingCert) {
        [IO.File]::WriteAllBytes($stagedCer, $existingCert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    }
} else {
    $rsa = [System.Security.Cryptography.RSA]::Create(2048)
    $cert = $null
    $tempPfx = Join-Path ([IO.Path]::GetDirectoryName($fullStage)) ('idd_ephemeral_' + [Guid]::NewGuid().ToString('N') + '.pfx')
    try {
        $subject = [System.Security.Cryptography.X509Certificates.X500DistinguishedName]::new('CN=Ultraterimal IDD Driver')
        $req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            $subject,
            $rsa,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $req.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
        $req.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
                [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
        $ekuOids = [System.Security.Cryptography.OidCollection]::new()
        [void]$ekuOids.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
        $req.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($ekuOids, $true))
        $req.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($req.PublicKey, $false))

        $now = [DateTimeOffset]::UtcNow
        $cert = $req.CreateSelfSigned($now.AddDays(-1), $now.AddYears(10))
        $pfxPassword = [Guid]::NewGuid().ToString('N') + [Guid]::NewGuid().ToString('N')
        $pfxBytes = $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $pfxPassword)
        $cerBytes = $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
        [IO.File]::WriteAllBytes($tempPfx, $pfxBytes)
        [Array]::Clear($pfxBytes, 0, $pfxBytes.Length)

        # Sign the UMDF binary first, generate the catalog over the signed binary, then sign the catalog.
        & $SignToolExe sign /fd SHA256 /f $tempPfx /p $pfxPassword $stagedDll
        if ($LASTEXITCODE -ne 0) { throw 'Failed to sign ultraterimal_idd.dll' }
        & $Inf2CatExe "/driver:$fullStage" /os:10_VB_X64
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $stagedCat)) { throw 'Inf2Cat failed to generate ultraterimal_idd.cat' }
        & $SignToolExe sign /fd SHA256 /f $tempPfx /p $pfxPassword $stagedCat
        if ($LASTEXITCODE -ne 0) { throw 'Failed to sign ultraterimal_idd.cat' }
        [IO.File]::WriteAllBytes($stagedCer, $cerBytes)
    } finally {
        Remove-Item -LiteralPath $tempPfx -Force -ErrorAction SilentlyContinue
        if ($cert) { $cert.Dispose() }
        if ($rsa) { $rsa.Dispose() }
    }
}

foreach ($signedFile in @($stagedDll, $stagedCat)) {
    $sig = Get-AuthenticodeSignature -LiteralPath $signedFile
    if (-not $sig -or $sig.Status -eq 'NotSigned' -or -not $sig.SignerCertificate) {
        throw "IDD driver artifact is missing an Authenticode signature: $signedFile"
    }
}
