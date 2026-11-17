param(
    [Parameter(Mandatory)][string]$Name,
    [int]$Players=24,
    [string]$Course='0x08',
    [string]$RRSettings='',
    [int]$Series=0,          # MKW100_TEST_SERIES: races to play (a KO series ends earlier when decided)
    [switch]$SpareLocal,     # MKW100_TEST_KO_SPARE_LOCAL
    [switch]$Auto,           # --race-auto (scripted local racer; needed for the results pages of a series)
    [int]$Laps=0,
    [int]$Minutes=10,
    [string]$Extra='',
    [string]$Stage='',       # default: <repo>/play-rr-ko (a Retro Rewind stage with RetroRewind.exe)
    [string]$SeedNand=''     # default: <repo>/../Work/OracleSeedNAND
)
# Headless Retro Rewind KO / Lap KO / KO Royale cases (docs/mkw100/rr-ko-modes.md). Evidence goes
# to <repo>/build-logs/ko/<Name>.*; settings index i of RRGameSettings.pul is value byte 0x2C+i.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$stage=if ($Stage) { $Stage } else { Join-Path $repo 'play-rr-ko' }
$logs=Join-Path $repo 'build-logs/ko'
New-Item -ItemType Directory -Force $logs | Out-Null
$seed=if ($SeedNand) { $SeedNand } else { Join-Path $repo '../Work/OracleSeedNAND' }
if (Get-Process WiiCompiled,RetroRewind -ErrorAction SilentlyContinue) { throw 'Another game/test is running.' }
$config=Join-Path $stage 'UserData/Config.toml'
$original=[IO.File]::ReadAllBytes($config)
$nand=Join-Path $logs "nand-$Name"
if (Test-Path $nand) { throw 'Use a new case name to preserve previous evidence.' }
Copy-Item $seed $nand -Recurse
if ($RRSettings) {
    $pulDir=Join-Path $nand 'shared2/Pulsar/RetroRewind6'
    New-Item -ItemType Directory -Force $pulDir | Out-Null
    $pul=[IO.File]::ReadAllBytes((Join-Path $stage 'UserData/NAND/shared2/Pulsar/RetroRewind6/RRGameSettings.pul'))
    foreach ($pair in $RRSettings.Split(',')) { $kv=$pair.Split('='); $pul[0x2C+[int]$kv[0]]=[byte][int]$kv[1] }
    [IO.File]::WriteAllBytes((Join-Path $pulDir 'RRGameSettings.pul'),$pul)
}
$keys=@('MKW100_TEST_SERIES','MKW100_TEST_KO_SPARE_LOCAL')
$previous=@{}
foreach ($key in $keys) { $previous[$key]=[Environment]::GetEnvironmentVariable($key) }
try {
    $text=[Text.Encoding]::UTF8.GetString($original)
    $text=[regex]::Replace($text,'(?m)^nand_root\s*=.*$',('nand_root = "'+$nand.Replace('\','/')+'"'))
    [IO.File]::WriteAllText($config,$text,[Text.UTF8Encoding]::new($false))
    $env:MKW100_TEST_SERIES=if ($Series -gt 0) {"$Series"} else {$null}
    $env:MKW100_TEST_KO_SPARE_LOCAL=if ($SpareLocal) {'1'} else {$null}
    $arguments=@($(if ($Auto) {'--race-auto'} else {'--race-cpu'}),"--race-players=$Players",'--headless',"--race=$Course",'--lockstep')
    if ($Laps -gt 0) { $arguments+="--race-laps=$Laps" }
    if ($Extra) { $arguments+=$Extra.Split(' ') }
    $stdout=Join-Path $logs "$Name.stdout.txt"
    $stderr=Join-Path $logs "$Name.stderr.txt"
    $p=Start-Process (Join-Path $stage 'RetroRewind.exe') -WorkingDirectory $stage -WindowStyle Hidden -ArgumentList $arguments -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $handle=$p.Handle
    if (!$p.WaitForExit($Minutes*60000)) { $p.Kill(); "TIMEOUT after $Minutes min" }
    $code=$p.ExitCode
    $p.Dispose()
    $log=[IO.File]::ReadAllText($stdout)+"`n"+[IO.File]::ReadAllText($stderr)
    "exit $code"
    ($log -split "`n" | Select-String 'rr-ko|race-oracle\] (result|finished|series|FAIL)|unmapped|stale generated|crash' | Select-Object -First 200) | ForEach-Object { $_.Line }
} finally {
    [IO.File]::WriteAllBytes($config,$original)
    foreach ($key in $previous.Keys) { [Environment]::SetEnvironmentVariable($key,$previous[$key]) }
}
