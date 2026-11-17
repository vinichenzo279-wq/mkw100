# Runs run-perf.ps1 cases one after another, each once no game/test has run for 20 s.
#   run-perf-queue.ps1 -Cases 'play-perf|b128n1|128|', 'play-rr-perf|r128n1|128|MKW100_TEST_X=1'
# Case: stage|name|players|env[|profile]
param([Parameter(Mandatory)][string[]]$Cases)
$ErrorActionPreference='Stop'
$runner=Join-Path $PSScriptRoot 'run-perf.ps1'
foreach ($c in $Cases) {
    $stage,$name,$players,$env,$prof=$c -split '\|'
    $free=0
    while ($free -lt 4) {
        # No game/test and no compile/link (other sessions' builds skew the timings).
        if (Get-Process WiiCompiled,RetroRewind,clang,clang++,ld.lld,ninja,cl,link,x86_64-w64-mingw32-clang++ -ErrorAction SilentlyContinue) { $free=0 } else { $free++ }
        Start-Sleep -Seconds 5
    }
    "== $name"
    try {
        $cargs=@{ Stage=$stage; Name=$name; Players=[int]$players; Env=$env }
        if ($prof) { $cargs["Profile"]=$true }
        & $runner @cargs
    } catch { "FAILED $name : $_" }
}
