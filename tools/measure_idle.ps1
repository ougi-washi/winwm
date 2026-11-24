param([ValidateRange(1,3600)][int]$Seconds=600)
$ErrorActionPreference='Stop'
$session=[Diagnostics.Process]::GetCurrentProcess().SessionId
$processes=@(Get-Process -Name winwm | Where-Object SessionId -eq $session)
if ($processes.Count -ne 2) {throw 'Expected the winwm host and session processes in this session.'}
$start=@{};foreach ($p in $processes) {$start[$p.Id]=$p.TotalProcessorTime.TotalSeconds}
$clock=[Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Seconds $Seconds
$elapsed=$clock.Elapsed.TotalSeconds
$counters=Get-CimInstance Win32_PerfRawData_PerfProc_Process
foreach ($p in $processes) {
    $p.Refresh()
    if ($p.HasExited) {throw 'A measured process exited.'}
    $row=$counters|Where-Object IDProcess -eq $p.Id
    [pscustomobject]@{Process=$p.ProcessName;PID=$p.Id;Seconds=$elapsed;
        PercentOfOneLogicalCore=100*($p.TotalProcessorTime.TotalSeconds-$start[$p.Id])/$elapsed;
        PrivateWorkingSetMiB=[math]::Round($row.WorkingSetPrivate/1MB,2)}
}

