[CmdletBinding()]
param(
    # The WiiCompiled installation folder (holds Base, Toolkit, BuildWorkspace, GameAssets).
    [string]$InstallRoot,
    [ValidateRange(1,16)][int]$Jobs = 4,
    # Also build Retro Rewind and stage it into play-rr\. A RetroRewind6 folder (the one holding
    # Binaries\Code.pul, or the folder above it), or 'auto' for the one the WiiCompiled installer
    # recorded. Remembered for later runs of this checkout; -NoRetroRewind turns it off again.
    [string]$RetroRewind,
    [switch]$NoRetroRewind,
    # Or build Item Fever and stage it into play-if\: the unzipped Item Fever download (the folder
    # holding ItemFever\ and riivolution\) or its ItemFever folder. One checkout builds one mod
    # (both use the mod executable); remembered like -RetroRewind, -NoItemFever turns it off.
    [string]$ItemFever,
    [switch]$NoItemFever,
    [switch]$AllowSourceDependencies,
    [switch]$PreflightOnly
)
# Builds MKW100 from this checkout and stages it into play\ (base game) and, optionally, play-rr\
# (Retro Rewind) or play-if\ (Item Fever). Rerun after every git pull; settings and saves in the play folders are kept.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Repo = $PSScriptRoot
# Per-checkout choices (gitignored), so a double-click after git pull rebuilds the same products.
$OptionsFile = Join-Path $Repo 'install-options.json'
$saved = $null
if (Test-Path -LiteralPath $OptionsFile) { $saved = Get-Content -LiteralPath $OptionsFile -Raw | ConvertFrom-Json }
$interactive = $false

if (!$InstallRoot -and $saved -and $saved.InstallRoot) { $InstallRoot = $saved.InstallRoot }
if (!$InstallRoot) {
    $InstallRoot = Read-Host 'WiiCompiled installation folder (contains Base, Toolkit, BuildWorkspace, GameAssets)'
    $interactive = $true
}
$InstallRoot = (Resolve-Path -LiteralPath $InstallRoot.Trim('"')).Path
$dvd = Join-Path $InstallRoot 'GameAssets\DATA'
if (!(Test-Path -LiteralPath $dvd)) { throw "Missing game data: $dvd (is this the WiiCompiled installation folder?)" }

# The Retro Rewind folder the WiiCompiled installer recorded, if any.
function Get-RecordedRetroRewind {
    $state = Join-Path $InstallRoot 'install-state.json'
    if (!(Test-Path -LiteralPath $state)) { return $null }
    $json = Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
    if ($json.PSObject.Properties['RetroRewindRoot'] -and $json.RetroRewindRoot) { return [string]$json.RetroRewindRoot }
    return $null
}
# Accepts the RetroRewind6 folder itself or the folder holding it; returns the RetroRewind6 path.
function Resolve-RetroRewindPack([string]$Path) {
    $Path = [IO.Path]::GetFullPath($Path.Trim('"'))
    foreach ($candidate in @($Path, (Join-Path $Path 'RetroRewind6'))) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'Binaries\Code.pul')) { return $candidate }
    }
    throw "Not a Retro Rewind folder (no Binaries\Code.pul): $Path"
}

if ($NoRetroRewind) {
    $RetroRewind = ''
} elseif (!$RetroRewind -and !$ItemFever -and $saved -and $saved.PSObject.Properties['RetroRewind'] -and $saved.RetroRewind) {
    $RetroRewind = $saved.RetroRewind
} elseif (!$RetroRewind -and $interactive) {
    $recorded = Get-RecordedRetroRewind
    if ($recorded) {
        $answer = Read-Host "Also build Retro Rewind from $recorded ? [y/N]"
        if ($answer -match '^(y|yes)$') { $RetroRewind = $recorded }
    } else {
        $RetroRewind = Read-Host 'Retro Rewind folder (RetroRewind6) to also build it, or Enter to skip'
    }
}
# Item Fever: the folder holding ItemFever\ and riivolution\, or ItemFever\ itself; returns ItemFever\.
function Resolve-ItemFeverPack([string]$Path) {
    $Path = [IO.Path]::GetFullPath($Path.Trim('"'))
    foreach ($candidate in @((Join-Path $Path 'ItemFever'), $Path)) {
        $xml = Join-Path (Split-Path -Parent $candidate) 'riivolution\ItemFever.xml'
        if ((Test-Path -LiteralPath (Join-Path $candidate 'Binaries\Code.pul')) -and (Test-Path -LiteralPath $xml)) {
            return $candidate
        }
    }
    throw "Not an Item Fever download (needs ItemFever\Binaries\Code.pul and riivolution\ItemFever.xml beside it): $Path"
}

if ($NoItemFever) {
    $ItemFever = ''
} elseif (!$ItemFever -and !$RetroRewind -and $saved -and $saved.PSObject.Properties['ItemFever'] -and $saved.ItemFever) {
    $ItemFever = $saved.ItemFever
} elseif (!$ItemFever -and !$RetroRewind -and $interactive) {
    $ItemFever = Read-Host 'Item Fever folder (the unzipped download) to build it instead, or Enter to skip'
}
if ($ItemFever -and $RetroRewind) {
    throw 'One checkout builds one mod: pass -RetroRewind or -ItemFever (use a second clone for the other, or -NoRetroRewind / -NoItemFever).'
}
if ($ItemFever) { $ItemFever = Resolve-ItemFeverPack $ItemFever }

if ($RetroRewind -eq 'auto') {
    $RetroRewind = Get-RecordedRetroRewind
    if (!$RetroRewind) { throw 'No Retro Rewind folder recorded in install-state.json; pass -RetroRewind <RetroRewind6 folder>.' }
}
if ($RetroRewind) { $RetroRewind = Resolve-RetroRewindPack $RetroRewind }

[pscustomobject]@{ InstallRoot = $InstallRoot; RetroRewind = $RetroRewind; ItemFever = $ItemFever } |
    ConvertTo-Json | Set-Content -LiteralPath $OptionsFile -Encoding UTF8

$buildArgs = @{ InstallRoot = $InstallRoot; Jobs = $Jobs; AllowSourceDependencies = $AllowSourceDependencies; PreflightOnly = $PreflightOnly }
if ($RetroRewind) { $buildArgs.RetroRewind = $RetroRewind }
if ($ItemFever) { $buildArgs.ItemFever = $ItemFever }
& (Join-Path $Repo 'Build-MKW100.ps1') @buildArgs
if ($LASTEXITCODE -ne 0) { throw 'Build failed; see build-logs.' }
if ($PreflightOnly) { return }

$native = Join-Path $Repo 'native-build-win'

function Assert-NotRunning([string]$Stage, [string]$ExeName) {
    $exe = [IO.Path]::GetFullPath((Join-Path $Stage $ExeName))
    $running = @(Get-Process ([IO.Path]::GetFileNameWithoutExtension($ExeName)) -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -ieq $exe) })
    if ($running.Count -gt 0) { throw "Close $exe, then rerun the installer to finish staging." }
}

# Executable, runtime DLLs, DSP coefficients, position icons and the portable marker.
function Copy-Runtime([string]$Stage, [string]$ExeName) {
    Assert-NotRunning $Stage $ExeName
    New-Item -ItemType Directory -Force $Stage | Out-Null
    # The install's Base DLLs (MSVC runtime and friends), then the DLLs this build linked against.
    Get-ChildItem -LiteralPath (Join-Path $InstallRoot 'Base') -Filter '*.dll' -File | Copy-Item -Destination $Stage -Force
    Get-ChildItem -LiteralPath $native -Filter '*.dll' -File | Copy-Item -Destination $Stage -Force
    Copy-Item -LiteralPath (Join-Path $native $ExeName) -Destination $Stage -Force
    $dsp = Join-Path $InstallRoot 'Base\dsp_coef.bin'
    if (Test-Path -LiteralPath $dsp) { Copy-Item -LiteralPath $dsp -Destination $Stage -Force }
    $icons = Join-Path $native 'rank_icons'
    if (Test-Path -LiteralPath $icons) {
        New-Item -ItemType Directory -Force (Join-Path $Stage 'rank_icons') | Out-Null
        Get-ChildItem -LiteralPath $icons -File | Copy-Item -Destination (Join-Path $Stage 'rank_icons') -Force
    }
    # Runtime assets (the HUD speedometer's layout), mounted over the disc from mkw100_assets\.
    $assets = Join-Path $Repo 'runtime\assets\mkw100\MKW100'
    if (Test-Path -LiteralPath $assets) {
        $dest = Join-Path $Stage 'mkw100_assets\MKW100'
        New-Item -ItemType Directory -Force $dest | Out-Null
        Get-ChildItem -LiteralPath $assets -File | Copy-Item -Destination $dest -Force
    }
    Set-Content -LiteralPath (Join-Path $Stage 'portable.txt') -Value ''
}

function TomlPath([string]$Path) { return $Path.Replace('\', '/').Replace('"', '\"') }

# A first-run Config.toml; an existing one (your settings) is never overwritten.
function Write-DefaultConfig([string]$Stage, [int]$Players, [string]$ExtraPaths) {
    $userData = Join-Path $Stage 'UserData'
    New-Item -ItemType Directory -Force $userData | Out-Null
    $config = Join-Path $userData 'Config.toml'
    $nand = Join-Path $userData 'NAND'
    # The runtime stops with a "NAND root" error when the folder is missing; a new one starts empty.
    New-Item -ItemType Directory -Force $nand | Out-Null
    if (Test-Path -LiteralPath $config) { return }
    $text = @"
[paths]
dvd_root = "$(TomlPath $dvd)"
nand_root = "$(TomlPath $nand)"
$ExtraPaths
[network]
enabled = false
[discord]
enabled = false
[local_vs]
players = $Players
unlock_all = true
instant_boxes = true
render_30fps = true
"@
    # UTF-8 without a BOM (Windows PowerShell's Set-Content -Encoding UTF8 would add one).
    [IO.File]::WriteAllText($config, $text, (New-Object Text.UTF8Encoding $false))
}

# <Stage>\<pack name>: a real folder whose subfolders are junctions to your pack (files copied).
# The runtime treats the folder above the pack as the virtual SD card, so the mod's Riivolution
# save lands in <Stage>\riivolution\save instead of beside your Dolphin/WheelWizard copy.
function Mirror-Pack([string]$Pack, [string]$Stage) {
    $dest = Join-Path $Stage (Split-Path -Leaf $Pack)
    New-Item -ItemType Directory -Force $dest | Out-Null
    foreach ($child in Get-ChildItem -LiteralPath $Pack -Force) {
        $target = Join-Path $dest $child.Name
        if ($child.PSIsContainer) {
            $existing = Get-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
            if ($existing -and $existing.LinkType) {
                [IO.Directory]::Delete($target, $false)   # removes the link only, never the pack's files
            } elseif ($existing) {
                Write-Warning "Keeping real folder $target (not linking it to $($child.FullName))"
                continue
            }
            New-Item -ItemType Junction -Path $target -Target $child.FullName | Out-Null
        } else {
            Copy-Item -LiteralPath $child.FullName -Destination $target -Force
        }
    }
    return $dest
}

# A mod product: native-build-win\RetroRewind.exe (the mod target) plus its pack. One checkout
# builds at most one mod product (Retro Rewind or Item Fever), so the other needs its own clone.
function Install-ModProduct([string]$Name, [string]$Stage, [string]$Pack, [int]$Players, [string]$ConfigKey) {
    Copy-Runtime $Stage 'RetroRewind.exe'
    $mirror = Mirror-Pack $Pack $Stage
    $extra = if ($ConfigKey) { "$ConfigKey = `"$(TomlPath $mirror)`"" } else { '' }
    Write-DefaultConfig $Stage $Players $extra
    Write-Host "$Name staged in $Stage (pack: $Pack)."
}

$play = Join-Path $Repo 'play'
Copy-Runtime $play 'WiiCompiled.exe'
Write-DefaultConfig $play 100 ''
Write-Host "MKW100 staged in $play."

if ($RetroRewind) {
    # Retro Rewind is capped at 100 racers (runtime/src/hle/local_vs.cpp MaxRacers).
    Install-ModProduct -Name 'Retro Rewind' -Stage (Join-Path $Repo 'play-rr') -Pack $RetroRewind -Players 100 -ConfigKey 'retro_rewind_root'
}

if ($ItemFever) {
    $stageIf = Join-Path $Repo 'play-if'
    Install-ModProduct -Name 'Item Fever' -Stage $stageIf -Pack $ItemFever -Players 100 -ConfigKey ''
    # The pack's XML sits beside the pack, as in the download (recomp.yml: ../riivolution/ItemFever.xml).
    New-Item -ItemType Directory -Force (Join-Path $stageIf 'riivolution') | Out-Null
    Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $ItemFever) 'riivolution\ItemFever.xml') `
        -Destination (Join-Path $stageIf 'riivolution') -Force
}

$launchers = if ($RetroRewind) { 'Play-MKW100.cmd or Play-RetroRewind.cmd' }
             elseif ($ItemFever) { 'Play-MKW100.cmd or Play-ItemFever.cmd' } else { 'Play-MKW100.cmd' }
Write-Host "Installed. Run $launchers. Settings and saves are kept on updates."
