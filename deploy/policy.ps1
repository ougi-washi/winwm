#Requires -RunAsAdministrator
[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('Audit','Enforce','Restore')][string]$Action,
      [Parameter(Mandatory)][string]$AccountSid)
$ErrorActionPreference='Stop'
$sid=New-Object Security.Principal.SecurityIdentifier($AccountSid)
if (!$sid.IsAccountSid()) {throw 'A user account SID is required.'}
$state=Join-Path $env:ProgramFiles 'winwm Policy'
$legacyState=Join-Path $env:ProgramFiles 'Native Desktop Policy'
if ((Test-Path (Join-Path $legacyState "original-$sid.xml")) -and !(Test-Path (Join-Path $state "original-$sid.xml"))) {
    $state=$legacyState
}
[IO.Directory]::CreateDirectory($state)|Out-Null
$original=Join-Path $state "original-$sid.xml"
$audit=Join-Path $state "audit-$sid.xml"
$expected=Join-Path $state "expected-$sid.xml"
$next=Join-Path $state "next-$sid.xml"
function Current { return [string](Get-AppLockerPolicy -Local -Xml) }
function Store([string]$path,[string]$text) {
    $tmp="$path.$([Guid]::NewGuid()).tmp"
    [IO.File]::WriteAllText($tmp,$text)
    if ([IO.File]::Exists($path)) {[IO.File]::Replace($tmp,$path,[System.Management.Automation.Language.NullString]::Value)}
    else {[IO.File]::Move($tmp,$path)}
}
$lock=[IO.File]::Open((Join-Path $state 'lock'),'OpenOrCreate','ReadWrite','None')
try {
    $current=Current
    if (Test-Path $original) {
        $known=@([IO.File]::ReadAllText($original))
        foreach ($p in $expected,$next) {if (Test-Path $p) {$known+=[IO.File]::ReadAllText($p)}}
        if ($known -cnotcontains $current) {throw "Local application policy changed independently. Compare the saved XML in $state before restoring."}
    }
    if ($Action -eq 'Restore') {
        if (!(Test-Path $original)) {return}
        Set-AppLockerPolicy -XmlPolicy $original
        foreach ($p in $expected,$next,$audit,$original) {if (Test-Path $p) {Remove-Item -LiteralPath $p}}
        Write-Output 'Previous local application-control policy restored.'
        return
    }
    if ((Get-Service AppIDSvc).Status -ne 'Running') {throw 'Application Identity must be running for policy testing and enforcement.'}
    if ($Action -eq 'Audit') {
        if (!(Test-Path $original)) {
            $fragment=Join-Path $state "fragment-$sid.xml"
            & (Join-Path $PSScriptRoot 'new_policy.ps1') -AccountSid $AccountSid -OutputFile $fragment -Mode AuditOnly
            [xml]$merged=$current
            foreach ($collection in ([xml](Get-Content -Raw $fragment)).AppLockerPolicy.RuleCollection) {
                $old=@($merged.AppLockerPolicy.RuleCollection|Where-Object Type -eq $collection.Type)
                foreach ($entry in $old) {[void]$merged.DocumentElement.RemoveChild($entry)}
                [void]$merged.DocumentElement.AppendChild($merged.ImportNode($collection,$true))
            }
            Store $audit $merged.OuterXml
            Store $original $current
            Remove-Item -LiteralPath $fragment
        }
        Store $next ([IO.File]::ReadAllText($audit))
    } else {
        if (!(Test-Path $audit)) {throw 'Apply Audit and review the managed session before enforcing.'}
        [xml]$merged=Get-Content $audit -Raw
        ($merged.AppLockerPolicy.RuleCollection|Where-Object Type -eq 'Exe').EnforcementMode='Enabled'
        Store $next $merged.OuterXml
    }
    Set-AppLockerPolicy -XmlPolicy $next
    Store $expected (Current)
    Write-Output "$Action policy applied for $AccountSid."
} finally {$lock.Dispose()}

