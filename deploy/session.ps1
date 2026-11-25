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
$x64=($env:PROCESSOR_ARCHITECTURE -eq 'AMD64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'AMD64')
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
function Restore-Entry($entry) {
    $now=Get-RegValue $entry
    $kind=if ($entry.kind) {$entry.kind} else {'String'}
    if ($now.present -and $now.kind -eq $kind -and
        ($now.value -ceq $entry.value -or ($null -ne $entry.oldOwned -and $now.value -ceq $entry.oldOwned))) {
        Set-RegValue $entry $entry.before
    }
}
function Restore-Activation {
    $record=Get-Content $journal -Raw | ConvertFrom-Json
    if ($record.sid -ne $sessionSid) {throw 'Activation belongs to a different user'}
    $entries=@($record.entries)
    [array]::Reverse($entries)
    foreach ($entry in $entries) {Restore-Entry $entry}
    Remove-Item -LiteralPath $journal
}
if ($Action -eq 'Rollback') {
    if (Test-Path $journal) {Restore-Activation}
    Write-Output 'Activation rollback complete.'
    return
}
[pscustomobject]@{Edition=$version.EditionID; Build=$version.CurrentBuild; X64=$x64; Account=$sessionSid; ConfigRoot=$root}
if ($Action -eq 'Preflight') {return}
if ([int]$version.CurrentBuild -lt 19045 -or !$x64) {
    throw 'A supported x64 Windows installation is required.'
}
if ($Action -eq 'Stage') {
    $names=@('winwm.exe','winwm_recovery.exe')
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
            if ([IO.Path]::GetExtension($provider) -ine '.exe') {
                throw "Invalid provider: $provider"
            }
        }
        $configDir=Split-Path -Parent $nativeConfig
        [IO.Directory]::CreateDirectory($configDir) | Out-Null
        Write-Atomic (Join-Path $configDir 'companions.jsonc') (@{version=1;file_browser=$fb;task_manager=$tm}|ConvertTo-Json)
        Write-Atomic $nativeConfig "source = companions.jsonc`n"
    }
    $check=Start-Process -FilePath (Join-Path $destination 'winwm.exe') -ArgumentList 'ctl','prepare' -PassThru -Wait -WindowStyle Hidden
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
$recoveryExe=Join-Path $stage.path 'winwm_recovery.exe'
if (!(Test-Path -LiteralPath $recoveryExe -PathType Leaf)) {throw "Missing $recoveryExe. Stage the complete release first."}
$check=Start-Process -FilePath $hostExe -ArgumentList 'ctl','validate-startup' -PassThru -Wait -WindowStyle Hidden
if ($check.ExitCode) {throw 'The staged release failed startup validation.'}
$startup=@{path='Software\Microsoft\Windows\CurrentVersion\Run';name='winwm';value=('"'+$hostExe+'"')}
$startup.before=Get-RegValue $startup
$legacy=@()
if (Test-Path $journal) {
    $old=Get-Content $journal -Raw|ConvertFrom-Json
    if ($old.sid -ne $sessionSid) {throw 'Activation journal belongs to a different user.'}
    $match=@($old.entries|Where-Object {$_.path -eq $startup.path -and $_.name -eq $startup.name})
    if ($match.Count -gt 1) {throw 'Activation journal mismatch.'}
    if ($match.Count -eq 1) {
        $current=$startup.before
        if ($current.present -and ($current.kind -ne 'String' -or
            ($current.value -cne $match[0].value -and $current.value -cne $match[0].oldOwned))) {
            throw 'Startup entry changed independently. Roll back before activating again.'
        }
        $startup.before=$match[0].before
        if ($current.present) {$startup.oldOwned=$current.value}
    }
    $legacy=@($old.entries|Where-Object {$_.path -ne $startup.path -or $_.name -ne $startup.name})
}
if ($startup.before.present -and !$startup.ContainsKey('oldOwned')) {
    throw 'The winwm startup entry already belongs to another deployment.'
}
# Journal both sides before migrating an older deployment away from shell replacement.
Write-Atomic $journal (@{sid=$sessionSid;entries=@($startup)+$legacy}|ConvertTo-Json -Depth 6)
try {
    Set-RegValue $startup @{present=$true;kind='String';value=$startup.value}
    if ($legacy.Count) {
        [array]::Reverse($legacy)
        foreach ($entry in $legacy) {Restore-Entry $entry}
        Write-Atomic $journal (@{sid=$sessionSid;entries=@($startup)}|ConvertTo-Json -Depth 6)
    }
} catch {Restore-Activation;throw}
Write-Output 'winwm will start at the next sign-in.'

