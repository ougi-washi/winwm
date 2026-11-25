param([ValidateRange(1,3600)][int]$Seconds=600)
$ErrorActionPreference='Stop'
$session=[Diagnostics.Process]::GetCurrentProcess().SessionId
$candidates=Get-CimInstance Win32_Process -Filter "SessionId=$session AND (Name='winwm.exe' OR Name='winwm_recovery.exe')"
$desktop=@($candidates | Where-Object { $_.Name -eq 'winwm.exe' -and $_.CommandLine -match '^\s*(?:"[^"]+"|\S+)(?:\s+--preview)?\s*$' })
$recovery=@($candidates | Where-Object { $_.Name -eq 'winwm_recovery.exe' -and $_.CommandLine -match '\s+"?/watch\d*:' })
if ($desktop.Count -ne 1 -or $recovery.Count -ne 1) {
    throw 'Expected one winwm desktop and one recovery process in this session.'
}
$processes=@(Get-Process -Id $desktop[0].ProcessId,$recovery[0].ProcessId)
$start=@{};foreach ($p in $processes) {$start[$p.Id]=$p.TotalProcessorTime.TotalSeconds}
$clock=[Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Seconds $Seconds
$elapsed=$clock.Elapsed.TotalSeconds
$counters=Get-CimInstance Win32_PerfRawData_PerfProc_Process -Filter "IDProcess=$($desktop[0].ProcessId) OR IDProcess=$($recovery[0].ProcessId)"
foreach ($p in $processes) {
    $p.Refresh()
    if ($p.HasExited) {throw 'A measured process exited.'}
    $row=$counters|Where-Object IDProcess -eq $p.Id
    if (!$row) {throw "No performance counters for $($p.ProcessName) ($($p.Id))."}
    [pscustomobject]@{Process=$p.ProcessName;PID=$p.Id;Seconds=$elapsed;
        PercentOfOneLogicalCore=100*($p.TotalProcessorTime.TotalSeconds-$start[$p.Id])/$elapsed;
        PrivateWorkingSetMiB=[math]::Round($row.WorkingSetPrivate/1MB,2)}
}

