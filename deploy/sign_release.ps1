param([Parameter(Mandatory)][string]$Bin,
      [Parameter(Mandatory)][string]$CertificateThumbprint,
      [Parameter(Mandatory)][uri]$TimestampServer)
$ErrorActionPreference='Stop'
$cert=Get-Item "Cert:\CurrentUser\My\$CertificateThumbprint"
if (!$cert.HasPrivateKey) {throw 'A code-signing private key is required.'}
foreach ($name in 'winwm.exe','winwm_recovery.exe','winwm_install.exe') {
    $file=Join-Path $Bin $name
    $signature=Set-AuthenticodeSignature -FilePath $file -Certificate $cert -HashAlgorithm SHA256 -TimestampServer $TimestampServer.AbsoluteUri
    if ($signature.Status -ne 'Valid') {throw "Signature verification failed for $name : $($signature.Status)"}
}
