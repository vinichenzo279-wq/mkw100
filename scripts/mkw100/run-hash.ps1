# Headless oracle race (result hash) on a stage folder; run-094.ps1 with -StageDir.
param([Parameter(Mandatory)][string]$Name, [string]$StageDir='MKW100-perf/play-perf', [int]$Players=100, [switch]$Items, [switch]$Perf, [switch]$Thirty, [string]$Course='0x08', [switch]$Diverse, [switch]$Auto, [switch]$Teams, [switch]$Battle, [switch]$Gp, [int]$Results=0, [int]$FinishAt=0, [switch]$Final, [string]$Extra='')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$stage=Join-Path $root $StageDir
if (Get-Process WiiCompiled,RetroRewind -ErrorAction SilentlyContinue) { throw 'Another game/test is running.' }
$config=Join-Path $stage 'UserData/Config.toml'
$original=[IO.File]::ReadAllBytes($config)
$nand=Join-Path $stage "perf-results/nand-$Name"
if (Test-Path $nand) { throw 'Use a new case name to preserve previous evidence.' }
Copy-Item (Join-Path $root 'Work/OracleSeedNAND') $nand -Recurse
$previous=@{}
foreach ($key in @('MKW100_TEST_ITEMS','MKW100_TEST_PERF','MKW100_TEST_30FPS','MKW100_TEST_RESULTS','MKW100_TEST_FINISH','MKW100_TEST_FINAL')) { $previous[$key]=[Environment]::GetEnvironmentVariable($key) }
try {
    $text=[Text.Encoding]::UTF8.GetString($original)
    $text=[regex]::Replace($text,'(?m)^nand_root\s*=.*$',('nand_root = "'+$nand.Replace('\','/')+'"'))
    [IO.File]::WriteAllText($config,$text,[Text.UTF8Encoding]::new($false))
    $env:MKW100_TEST_ITEMS=if ($Items) {'1'} else {$null}
    $env:MKW100_TEST_PERF=if ($Perf) {'1'} else {$null}
    $env:MKW100_TEST_30FPS=if ($Thirty) {'1'} else {$null}
    $env:MKW100_TEST_RESULTS=if ($Results -gt 0) {"$Results"} else {$null}
    $env:MKW100_TEST_FINISH=if ($FinishAt -gt 0) {"$FinishAt"} else {$null}
    $env:MKW100_TEST_FINAL=if ($Final) {'1'} else {$null}
    $arguments=@($(if ($Auto) {'--race-auto'} else {'--race-cpu'}),"--race-players=$Players",'--headless')
    $arguments+="--race=$(if ($Battle -and $Course -eq '0x08') {'0x21'} else {$Course})"
    if ($Diverse) { $arguments+='--race-roster=diverse' }
    if ($Teams) { $arguments+='--race-teams' }
    if ($Battle) { $arguments+='--race-battle' }
    if ($Gp) { $arguments+='--race-gp' }
    if ($Extra) { $arguments+=$Extra }
    if (!$Perf) { $arguments+='--lockstep' }
    $stdout=Join-Path $stage "perf-results/$Name.stdout.txt"
    $stderr=Join-Path $stage "perf-results/$Name.stderr.txt"
    $p=Start-Process (Join-Path $stage 'WiiCompiled.exe') -WorkingDirectory $stage -WindowStyle Hidden -ArgumentList $arguments -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $handle=$p.Handle
    if (!$p.WaitForExit(600000)) { $p.Kill(); throw 'Case timed out' }
    $code=$p.ExitCode
    $p.Dispose()
    $log=[IO.File]::ReadAllText($stdout)+"`n"+[IO.File]::ReadAllText($stderr)
    if ($code -ne 0 -or $log -match 'unmapped|stale generated|\[race-oracle\] FAIL') { throw "Case $Name failed (exit $code)" }
    if ($Perf) {
        if ($log -notmatch '\[race-perf\]') { throw 'Missing performance result' }
        if ($Thirty -and $log -notmatch 'alternate-frame rendering enabled') { throw '30 FPS path was not activated' }
        ($log -split "`n" | Select-String '\[race-perf\]') | Set-Content (Join-Path $stage "perf-results/$Name.result.txt")
    } else {
        $rows=[regex]::Matches($log,'\[race-oracle\] racer\s+(\d+) place\s+(\d+)')
        $places=@($rows | ForEach-Object {[int]$_.Groups[2].Value} | Sort-Object -Unique)
        if ($rows.Count -ne $Players -or $places.Count -ne $Players -or $places[0] -ne 1 -or $places[-1] -ne $Players) { throw 'Incomplete classification' }
        if ($Items) {
            $hits=[regex]::Matches($log,'lightning: slot=(\d+) place=(\d+) duration=(\d+) applied=1')
            $expandedHits=@($hits | Where-Object {[int]$_.Groups[1].Value -ge 12 -and [int]$_.Groups[2].Value -gt 12})
            if ($log -notmatch 'mechanics test injected' -or $log -notmatch "POW deployment: owner=\d+ racers=$Players" -or !$expandedHits.Count) { throw 'Expanded item exercise did not run' }
        }
        ($log -split "`n" | Select-String '\[race-oracle\] result') | Set-Content (Join-Path $stage "perf-results/$Name.result.txt")
    }
    Get-Content (Join-Path $stage "perf-results/$Name.result.txt")
} finally {
    [IO.File]::WriteAllBytes($config,$original)
    foreach ($key in $previous.Keys) { [Environment]::SetEnvironmentVariable($key,$previous[$key]) }
}


