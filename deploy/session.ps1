[CmdletBinding()]
param(
    [ValidateSet('Preflight','Stage','Activate','Rollback')][string]$Action='Preflight',
    [string]$Bin=(Join-Path $PSScriptRoot '..\bin'),
    [string]$FileBrowser,
    [string]$TaskManager
)
$ErrorActionPreference='Stop'
$root=Join-Path $env:LOCALAPPDATA 'winwm'
$configDir=Join-Path $env:USERPROFILE '.config\winwm'
$journal=Join-Path $root 'activation.json'
$runKey='Software\Microsoft\Windows\CurrentVersion\Run'
$manifest=Join-Path $root 'staged.json'
$sessionSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$version=Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$x64=($env:PROCESSOR_ARCHITECTURE -eq 'AMD64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'AMD64')
$utf8=New-Object System.Text.UTF8Encoding($false)
function Write-Atomic($path,$text,[switch]$IfMissing) {
    $tmp="$path.$([Guid]::NewGuid()).tmp"
    try {
        [IO.File]::WriteAllText($tmp,$text,$utf8)
        if (!$IfMissing -and [IO.File]::Exists($path)) {
            [IO.File]::Replace($tmp,$path,[System.Management.Automation.Language.NullString]::Value)
        } else {
            try { [IO.File]::Move($tmp,$path) }
            catch [IO.IOException] { if (!$IfMissing -or ![IO.File]::Exists($path)) {throw} }
        }
    } finally { [IO.File]::Delete($tmp) }
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
function Get-ActivationEntry {
    $record=Get-Content -LiteralPath $journal -Raw | ConvertFrom-Json
    if ($record.sid -ne $sessionSid) {throw 'Activation belongs to a different user.'}
    $entries=@($record.entries)
    if ($entries.Count -ne 1 -or $entries[0].path -ne $runKey -or $entries[0].name -ne 'winwm') {
        throw 'The activation journal does not identify this winwm startup entry.'
    }
    $entries[0]
}
function Restore-Activation {
    Restore-Entry (Get-ActivationEntry)
    Remove-Item -LiteralPath $journal
}
if ($Action -eq 'Rollback') {
    if (Test-Path $journal) {Restore-Activation}
    Write-Output 'Activation rollback complete.'
    return
}
[pscustomobject]@{Edition=$version.EditionID; Build=$version.CurrentBuild; X64=$x64; Account=$sessionSid; ConfigRoot=$configDir}
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
    $configPath=Join-Path $configDir 'winwm.lua'
    if (($FileBrowser -or $TaskManager) -and !(Test-Path -LiteralPath $configPath)) {
        $settings=foreach ($provider in @{file_browser=$FileBrowser;task_manager=$TaskManager}.GetEnumerator()) {
            if (!$provider.Value) {continue}
            $exe=Get-Item -LiteralPath $provider.Value
            if ($exe.PSIsContainer -or $exe.Extension -ine '.exe') {throw "Invalid provider: $($exe.FullName)"}
            $provider.Key+' = "'+$exe.FullName.Replace('\','\\')+'"'
        }
        $template=[IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\assets\winwm.lua'))
        $override='hl.config({ programs = { '+($settings -join ', ')+' } })'
        [IO.Directory]::CreateDirectory((Split-Path -Parent $configPath)) | Out-Null
        Write-Atomic $configPath ($template+[Environment]::NewLine+$override+[Environment]::NewLine) -IfMissing
    }
    $check=Start-Process -FilePath (Join-Path $destination 'winwm.exe') -ArgumentList 'ctl','validate-startup' -PassThru -Wait -WindowStyle Hidden
    if ($check.ExitCode) {throw 'The staged release failed startup validation.'}
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
$check=Start-Process -FilePath $hostExe -ArgumentList 'ctl','validate-activation' -PassThru -Wait -WindowStyle Hidden
if ($check.ExitCode) {throw 'The release or Windows shell configuration prevents activation.'}
$startup=@{path=$runKey;name='winwm';value=('"'+$hostExe+'"')}
$startup.before=Get-RegValue $startup
if (Test-Path -LiteralPath $journal) {
    $entry=Get-ActivationEntry
    $current=$startup.before
    if ($current.present -and ($current.kind -ne 'String' -or
        ($current.value -cne $entry.value -and $current.value -cne $entry.oldOwned))) {
        throw 'Startup entry changed independently. Roll back before activating again.'
    }
    $startup.before=$entry.before
    if ($current.present) {$startup.oldOwned=$current.value}
} elseif ($startup.before.present) {
    throw 'The winwm startup entry already belongs to another deployment.'
}
Write-Atomic $journal (@{sid=$sessionSid;entries=@($startup)}|ConvertTo-Json -Depth 6)
try {
    Set-RegValue $startup @{present=$true;kind='String';value=$startup.value}
} catch {Restore-Activation;throw}
Write-Output 'winwm will start at the next sign-in.'
