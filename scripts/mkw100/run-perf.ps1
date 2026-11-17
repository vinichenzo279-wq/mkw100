# Windowed perf run (MKW100_TEST_PERF) on a stage folder, optionally with the stack profiler.
# Results land in -Out: <Name>.stdout.txt / .stderr.txt / .result.txt / .prof.txt.
#   run-perf.ps1 -Stage ..\play-perf -Name base128a -Players 128 -Profile -Env 'MKW100_TEST_KART_LOD=0'
# Refuses to start while any WiiCompiled.exe / RetroRewind.exe runs. Restores Config.toml afterwards.
param([Parameter(Mandatory)][string]$Stage, [Parameter(Mandatory)][string]$Name, [int]$Players=128,
      [string]$Course='0x08', [switch]$Profile, [switch]$Items, [switch]$Headless, [string]$Env='',
      [string]$Out='', [string]$Seed='', [string]$Extra='')
$ErrorActionPreference='Stop'
$Stage=(Resolve-Path $Stage).Path
if (!$Out) { $Out=Join-Path $Stage 'perf-results' }
New-Item -ItemType Directory -Force $Out | Out-Null
$exe=Get-ChildItem $Stage -Filter *.exe | Where-Object Name -in 'WiiCompiled.exe','RetroRewind.exe' | Select-Object -First 1
if (Get-Process WiiCompiled,RetroRewind -ErrorAction SilentlyContinue) { throw 'Another game/test is running.' }
$stdout=Join-Path $Out "$Name.stdout.txt"
if (Test-Path $stdout) { throw 'Use a new case name.' }
if (!$Seed) { $Seed=Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent | Split-Path -Parent) 'Work/OracleSeedNAND' }
$config=Join-Path $Stage 'UserData/Config.toml'
$original=[IO.File]::ReadAllBytes($config)
$nand=Join-Path $Out "nand-$Name"
Copy-Item $Seed $nand -Recurse
$vars=@{ 'MKW100_TEST_PERF'='1'; 'MKW_PERF_STATS'='1' }
if ($Items) { $vars['MKW100_TEST_ITEMS']='1' }
if ($Profile) { $vars['MKW100_TEST_PROFILE']=(Join-Path $Out "$Name.prof.txt") }
foreach ($kv in ($Env -split ';' | Where-Object { $_ })) { $k,$v=$kv -split '=',2; $vars[$k]=$v }
$previous=@{}
foreach ($k in $vars.Keys) { $previous[$k]=[Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k,$vars[$k]) }
try {
    $text=[Text.Encoding]::UTF8.GetString($original)
    $text=[regex]::Replace($text,'(?m)^nand_root\s*=.*$',('nand_root = "'+$nand.Replace('\','/')+'"'))
    [IO.File]::WriteAllText($config,$text,[Text.UTF8Encoding]::new($false))
    $arguments=@('--race-cpu',"--race-players=$Players","--race=$Course")
    if ($Headless) { $arguments+='--headless' }
    if ($Extra) { $arguments+=($Extra -split ' ') }
    $p=Start-Process $exe.FullName -WorkingDirectory $Stage -ArgumentList $arguments -RedirectStandardOutput $stdout -RedirectStandardError (Join-Path $Out "$Name.stderr.txt") -PassThru
    $handle=$p.Handle
    # Other processes' CPU while the case runs, sampled every 5 s: a busy machine skews the timing.
    $busy=@(); $prevCpu=$null
    while (!$p.WaitForExit(5000)) {
        $others=@(Get-Process | Where-Object { $_.Id -ne $p.Id -and $_.CPU -ne $null } | Select-Object Id,CPU)
        if ($prevCpu) {
            $sum=0; foreach ($o in $others) { if ($prevCpu.ContainsKey($o.Id)) { $sum+=$o.CPU-$prevCpu[$o.Id] } }
            $busy+=[math]::Round($sum/5,2)
        }
        $prevCpu=@{}; foreach ($o in $others) { $prevCpu[$o.Id]=$o.CPU }
        if ((Get-Date)-$p.StartTime -gt [TimeSpan]::FromMinutes(10)) { $p.Kill(); throw 'Case timed out' }
    }
    $code=$p.ExitCode
    $p.Dispose()
    $log=[IO.File]::ReadAllText($stdout)
    $lines=@($log -split "`n" | Select-String '\[race-perf\]|\[perf\]|busy' | Select-Object -Last 8)
    $lines+="[run] other processes' CPU (cores, 5 s samples): $($busy -join ' ')"
    $lines | Set-Content (Join-Path $Out "$Name.result.txt")
    "exit $code"; $lines
} finally {
    [IO.File]::WriteAllBytes($config,$original)
    foreach ($k in $previous.Keys) { [Environment]::SetEnvironmentVariable($k,$previous[$k]) }
    Remove-Item $nand -Recurse -Force -ErrorAction SilentlyContinue
}
