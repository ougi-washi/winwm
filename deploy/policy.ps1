#Requires -RunAsAdministrator
[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('Restore')][string]$Action,
      [Parameter(Mandatory)][string]$AccountSid)
$ErrorActionPreference='Stop'
$sid=New-Object Security.Principal.SecurityIdentifier($AccountSid)
if (!$sid.IsAccountSid()) {throw 'A user account SID is required.'}
foreach ($directory in 'winwm Policy','Native Desktop Policy') {
    $state=Join-Path $env:ProgramFiles $directory
    $original=Join-Path $state "original-$sid.xml"
    if (!(Test-Path -LiteralPath $original)) {continue}
    $lock=[IO.File]::Open((Join-Path $state 'lock'),'OpenOrCreate','ReadWrite','None')
    try {
        if (!(Test-Path -LiteralPath $original)) {continue}
        $expected=Join-Path $state "expected-$sid.xml"
        $next=Join-Path $state "next-$sid.xml"
        $audit=Join-Path $state "audit-$sid.xml"
        $known=@([IO.File]::ReadAllText($original))
        foreach ($path in $expected,$next) {
            if (Test-Path -LiteralPath $path) {$known+=[IO.File]::ReadAllText($path)}
        }
        $current=[string](Get-AppLockerPolicy -Local -Xml)
        if ($known -cnotcontains $current) {throw "Local application policy changed independently. Compare the saved XML in $state before restoring."}
        if ($current -cne $known[0]) {Set-AppLockerPolicy -XmlPolicy $original}
        foreach ($path in $expected,$next,$audit,$original) {
            if (Test-Path -LiteralPath $path) {Remove-Item -LiteralPath $path}
        }
        Write-Output 'Previous local application-control policy restored.'
    } finally {$lock.Dispose()}
}
