[CmdletBinding()]
param(
    [ValidateSet('Preflight','Stage','Activate','Rollback')][string]$Action='Preflight',
    [string]$Bin=(Join-Path $PSScriptRoot '..\bin'),
    [string]$FileBrowser,
    [string]$TaskManager
)
$ErrorActionPreference='Stop'
$root=Join-Path $env:LOCALAPPDATA 'winwm'
$legacyRoot=Join-Path $env:LOCALAPPDATA 'native_desktop'
$journal=Join-Path $root 'activation.json'
$priorJournal=Join-Path $legacyRoot 'activation.json'
if (!(Test-Path $journal) -and (Test-Path $priorJournal)) {
    if ($Action -eq 'Rollback') {$journal=$priorJournal}
    if ($Action -eq 'Activate') {throw 'Roll back the previous managed shell before activating winwm.'}
}
$manifest=Join-Path $root 'staged.json'
$sessionSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$version=Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$light=Get-ItemPropertyValue 'HKLM:\SOFTWARE\Microsoft\PolicyManager\current\device\WindowsLogon' 'OverrideShellProgram' -ErrorAction SilentlyContinue
$x64=($env:PROCESSOR_ARCHITECTURE -eq 'AMD64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'AMD64')
$editionOk=$version.EditionID -match '^(Professional|Enterprise|Education|IoTEnterprise)'
$utf8=New-Object System.Text.UTF8Encoding($false)
function Write-Atomic($path,$text) {
    $tmp="$path.$([Guid]::NewGuid()).tmp"
    [IO.File]::WriteAllText($tmp,$text,$utf8)
    if ([IO.File]::Exists($path)) { [IO.File]::Replace($tmp,$path,[System.Management.Automation.Language.NullString]::Value) }
    else { [IO.File]::Move($tmp,$path) }
}
function Get-RegValue($entry) {
    $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($entry.path)
    try {
        if (!$key -or $key.GetValueNames() -notcontains $entry.name) {return @{present=$false}}
        $kind=$key.GetValueKind($entry.name).ToString()
        if ($kind -notin 'String','ExpandString' -and !($entry.kind -eq 'DWord' -and $kind -eq 'DWord')) {throw "Unexpected registry type at $($entry.path)"}
        return @{present=$true;kind=$kind;value=$key.GetValue($entry.name,$null,'DoNotExpandEnvironmentNames')}
    } finally {if ($key) {$key.Dispose()}}
}
function Set-RegValue($entry,$v) {
    $key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($entry.path)
    try {
        if ($v.present) {$key.SetValue($entry.name,$v.value,[Microsoft.Win32.RegistryValueKind]$v.kind)}
        else {$key.DeleteValue($entry.name,$false)}
    } finally {$key.Dispose()}
}
function Restore-Activation {
    $record=Get-Content $journal -Raw | ConvertFrom-Json
    if ($record.sid -ne $sessionSid) {throw 'Activation belongs to a different user'}
    $entries=@($record.entries)
    [array]::Reverse($entries)
    foreach ($entry in $entries) {
        $now=Get-RegValue $entry
        if ($now.present -and ($now.value -ceq $entry.value -or ($null -ne $entry.oldOwned -and $now.value -ceq $entry.oldOwned))) {Set-RegValue $entry $entry.before}
    }
    Remove-Item -LiteralPath $journal
}
if ($Action -eq 'Rollback') {
    if (Test-Path $journal) {Restore-Activation}
    Write-Output 'Previous user shell and folder verbs restored for the next sign-in.'
    return
}
[pscustomobject]@{Edition=$version.EditionID; Build=$version.CurrentBuild; X64=$x64;
    PerUserShellAvailable=[bool]$editionOk; LightweightShell=$light; Account=$sessionSid; ConfigRoot=$root}
if ($Action -eq 'Preflight') {return}
if (!$editionOk -or [int]$version.CurrentBuild -lt 19045 -or $light -eq 1 -or !$x64) {
    throw 'This Windows configuration has no qualified activation path.'
}
if ($Action -eq 'Stage') {
    $names=@('winwm.exe')
    $hashes=foreach ($name in $names) {(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Bin $name)).Hash}
    $tag=($hashes | ForEach-Object {$_.Substring(0,12)}) -join '-'
    $destination=Join-Path $root "releases\$tag"
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    foreach ($name in $names) {
        $to=Join-Path $destination $name
        if (!(Test-Path $to)) {Copy-Item -LiteralPath (Join-Path $Bin $name) -Destination $to}
        if ((Get-FileHash $to).Hash -ne (Get-FileHash (Join-Path $Bin $name)).Hash) {throw "Release hash mismatch: $name"}
    }
    $config=Join-Path $legacyRoot 'config.jsonc'
    $nativeConfig=Join-Path $env:USERPROFILE '.config\winwm\winwm.conf'
    if (!(Test-Path $nativeConfig) -and !(Test-Path $config)) {
        if (!$FileBrowser -or !$TaskManager) {throw 'Specify -FileBrowser and -TaskManager with the installed executable paths.'}
        $fb=(Get-Item -LiteralPath $FileBrowser).FullName
        $tm=(Get-Item -LiteralPath $TaskManager).FullName
        foreach ($provider in $fb,$tm) {
            if ([IO.Path]::GetExtension($provider) -ine '.exe' -or [IO.Path]::GetFileName($provider) -ieq 'explorer.exe') {
                throw "Invalid provider: $provider"
            }
        }
        $configDir=Split-Path -Parent $nativeConfig
        [IO.Directory]::CreateDirectory($configDir) | Out-Null
        Write-Atomic (Join-Path $configDir 'companions.jsonc') (@{version=1;file_browser=$fb;task_manager=$tm}|ConvertTo-Json)
        Write-Atomic $nativeConfig "source = companions.jsonc`n"
    }
    $check=Start-Process -FilePath (Join-Path $destination 'winwm.exe') -ArgumentList 'ctl','prepare' -PassThru -Wait
    if ($check.ExitCode) {throw 'winwm configuration validation failed.'}
    $previous=$null
    if (Test-Path $manifest) {
        $prior=Get-Content $manifest -Raw|ConvertFrom-Json
        $previous=if ($prior.path -eq $destination) {$prior.previous} else {$prior.path}
    }
    Write-Atomic $manifest (@{path=$destination;previous=$previous}|ConvertTo-Json)
    if ($previous) {Write-Atomic (Join-Path $root 'previous.txt') (Join-Path $previous 'winwm.exe')}
    Write-Output "Staged $destination. The active shell policy has not been modified."
    return
}
if (!(Test-Path $manifest)) {throw 'Stage the binaries before activation.'}
$stage=Get-Content $manifest -Raw|ConvertFrom-Json
$hostExe=Join-Path $stage.path 'winwm.exe'
if (!(Test-Path -LiteralPath $hostExe -PathType Leaf)) {throw "Missing $hostExe"}
$check=Start-Process -FilePath $hostExe -ArgumentList 'ctl','prepare' -PassThru -Wait
if ($check.ExitCode) {throw 'Config validation failed'}
# The account must already be covered by an enforced executable rule. AuditOnly
# evaluation can return Denied during simulation, so check enforcement separately.
$effective=Get-AppLockerPolicy -Effective -Xml -ErrorAction Stop
$policy=[xml]$effective
$collection=@($policy.AppLockerPolicy.RuleCollection | Where-Object Type -eq 'Exe')
if ($collection.Count -ne 1 -or $collection[0].EnforcementMode -ne 'Enabled' -or
    (Get-Service AppIDSvc).Status -ne 'Running') {throw 'Explorer exclusion is not enforced for this account.'}
$actual=Get-AppLockerPolicy -Effective
$result=Test-AppLockerPolicy -PolicyObject $actual -Path (Join-Path $env:WINDIR 'explorer.exe') -User $sessionSid
if (@($result | Where-Object PolicyDecision -eq 'Denied').Count -ne 1) {throw 'Explorer is allowed by effective policy.'}
foreach ($binary in @($hostExe)) {
    $decision=Test-AppLockerPolicy -PolicyObject $actual -Path $binary -User $sessionSid
    if ([string]$decision.PolicyDecision -notlike 'Allowed*') {throw "Application policy blocks $binary"}
}
$entries=@()
foreach ($type in 'Directory','Drive') {
    $base="Software\Classes\$type\shell"
    $entries+=@{path="$base\winwm\command";name='';value=('"'+$hostExe+'" ctl run open_folder "%1\."')}
    $entries+=@{path=$base;name='';value='winwm'}
}
$entries+=@{path='Software\Microsoft\Windows\CurrentVersion\Policies\System';name='Shell';value=('"'+$hostExe+'"')}
foreach ($name in 'AppsUseLightTheme','SystemUsesLightTheme') {
    $entries+=@{path='Software\Microsoft\Windows\CurrentVersion\Themes\Personalize';name=$name;value=0;kind='DWord'}
}
if (Test-Path $journal) {
    $old=Get-Content $journal -Raw|ConvertFrom-Json
    if ($old.sid -ne $sessionSid) {throw 'Activation journal belongs to a different user.'}
    foreach ($entry in $entries) {
        $match=@($old.entries|Where-Object {$_.path -eq $entry.path -and $_.name -eq $entry.name})
        if ($match.Count -eq 0 -and $entry.kind -eq 'DWord') {
            $entry.before=Get-RegValue $entry
            continue
        }
        if ($match.Count -ne 1) {throw 'Activation journal mismatch.'}
        $current=Get-RegValue $entry
        if (!$current.present -or $current.value -ne $match[0].value) {throw 'Activation changed externally. Roll back before retrying.'}
        $entry.before=$match[0].before
        $entry.oldOwned=$match[0].value
    }
} else {foreach ($entry in $entries) {$entry.before=Get-RegValue $entry}}
Write-Atomic $journal (@{sid=$sessionSid;entries=$entries}|ConvertTo-Json -Depth 6)
try {
    foreach ($entry in $entries) {
        $kind=if ($entry.kind) {$entry.kind} else {'String'}
        Set-RegValue $entry @{present=$true;kind=$kind;value=$entry.value}
    }
} catch {Restore-Activation;throw}
Write-Output 'Per-user shell and folder verbs activated for the next sign-in.'

