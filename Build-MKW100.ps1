[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallRoot,
    [int]$Jobs = 6,
    [switch]$PreflightOnly,
    [switch]$AllowSourceDependencies,
    # A Retro Rewind release folder (RetroRewind6, holding Binaries\Code.pul): also builds the
    # RetroRewind product, offline (no Retro-WFC payload), as scripts/mkw100/build_rr.sh does on
    # macOS. Both executables come from one set of shards, but mod-aware shards differ from
    # base-only ones: switching -RetroRewind/-ItemFever on or off recompiles everything, so keep
    # passing it for a checkout once you use it (Install-MKW100.ps1 remembers it).
    [string]$RetroRewind,
    # An Item Fever pack folder (ItemFever, holding Binaries\Code.pul and riivolution\ItemFever.xml):
    # builds the mod product from it instead of Retro Rewind. Both build the RetroRewind target
    # (the mod product), so one checkout holds the base game plus one of them.
    [string]$ItemFever,
    # Experimental (docs/mkw100/rr-item-fever.md), with -RetroRewind: an Item Fever pack folder whose
    # items join Retro Rewind. Builds the RetroRewind product from one combined Kamek module
    # (scripts/mkw100/combine_pul.py, plan projects/mkwii/mkw100/rr-item-fever.txt).
    [string]$ItemFeverItems,
    # A Formula Kart Wii download folder (holding fkw\fkw\FormulaKartWiiP.bin and riivolution\fkw.xml):
    # builds the mod product from it. FKW is a Riivolution patch, not a Pulsar pack; its payload is
    # converted to a Kamek chunk first (scripts/mkw100/fkw_to_kamek.py, needs python on PATH).
    [string]$FormulaKart,
    # An unzipped MKWii 8P Splitscreen release (mkwii-8p-splitscreen-1.2.0, holding payload\mkw\Code.pul):
    # builds the mod product from its "Mario Kart Wii" module (5-8 local players). The character pages'
    # menu controls are generated from the install's disc files (scripts/mkw100/ss8_stage_pack.py, needs python).
    [string]$SplitScreen8
)
# Standalone, isolated Windows fork build. Never calls LocalBuild.ps1, never
# modifies original Install\Base or the older R13/R23 build cache.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (@($RetroRewind, $ItemFever, $FormulaKart, $SplitScreen8 | Where-Object { $_ }).Count -gt 1) { throw 'Pass only one of -RetroRewind, -ItemFever, -FormulaKart, -SplitScreen8.' }
if ($ItemFeverItems -and -not $RetroRewind) { throw '-ItemFeverItems goes with -RetroRewind.' }
# The mod product's pack, its recomp.yml profile, where that profile reads Code.pul, and its output.
$ModPack = $null
if ($RetroRewind) {
    $ModPack = $RetroRewind; $ModProfile = 'retro-rewind'; $ModName = 'Retro Rewind'
    $ModStagedPul = 'PulsarPacks\completed\RetroRewind\RetroRewind6\Binaries'; $ModOut = 'build\mods\retro_rewind_full_cpp'
} elseif ($ItemFever) {
    $ModPack = $ItemFever; $ModProfile = 'item-fever'; $ModName = 'Item Fever'
    $ModStagedPul = 'PulsarPacks\completed\ItemFever\ItemFever\Binaries'; $ModOut = 'build\mods\item_fever_full_cpp'
} elseif ($FormulaKart) {
    $ModPack = Join-Path $FormulaKart 'fkw'; $ModProfile = 'formula-kart'; $ModName = 'Formula Kart Wii'
    $ModStagedPul = 'PulsarPacks\completed\FormulaKart\fkw\Binaries'; $ModOut = 'build\mods\formula_kart_full_cpp'
} elseif ($SplitScreen8) {
    # The pack is made inside the checkout (Code.pul plus menu archives generated from the disc).
    $ModPack = 'PulsarPacks\completed\SplitScreen8\SplitScreen8'; $ModProfile = 'splitscreen8'; $ModName = 'MKWii 8P Splitscreen'
    $ModStagedPul = 'PulsarPacks\completed\SplitScreen8\SplitScreen8\Binaries'; $ModOut = 'build\mods\splitscreen8_full_cpp'
}
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = $Here
$Logs = Join-Path $Here 'build-logs'
$Toolkit = Join-Path $InstallRoot 'Toolkit'
$OriginalWorkspace = Join-Path $InstallRoot 'BuildWorkspace'
$NativeBuild = Join-Path $Repo 'native-build-win'
$BuildMarker = Join-Path $Logs 'build_success.txt'
$BuildLog = Join-Path $Logs 'build_steps.txt'
$RunStartedAt = Get-Date
New-Item -ItemType Directory -Force -Path $Logs | Out-Null
function Record([string]$Text) {
    $line = '[mkw100] ' + $Text
    Write-Host $line
    Add-Content -LiteralPath $BuildLog -Value $line -Encoding UTF8
}
function Require([string]$File, [string]$Description) {
    if (-not (Test-Path -LiteralPath $File)) { throw ('Missing ' + $Description + ': ' + $File) }
}
function InvokeTool([string]$Executable, [string[]]$Arguments, [string]$Name) {
    Record ('Starting ' + $Name)
    $stdout = Join-Path $Logs ($Name + '.stdout.txt')
    $stderr = Join-Path $Logs ($Name + '.stderr.txt')
    # Windows PowerShell 5.1 compatible: Start-Process returns the process
    # object; WaitForExit waits only for THIS process, not its entire process
    # tree. Unlike native direct invocation, the process ExitCode is explicit.
    # Keep original Windows argument quoting (verified in the prior build).
    $quoted = @($Arguments | ForEach-Object {
        if ($_.Contains('"')) { throw ('Unsupported quote in native argument: ' + $_) }
        '"' + ($_ -replace '(\\+)$', '$1$1') + '"'
    })
    $child = Start-Process -FilePath $Executable -ArgumentList $quoted `
        -NoNewWindow -PassThru -WorkingDirectory $Repo `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -ErrorAction Stop
    try {
        $handle = $child.Handle
        $child.WaitForExit()
        $code = [int]$child.ExitCode
    } finally {
        $child.Dispose()
    }
    $tail = @(Get-Content -LiteralPath $stdout -Tail 12 -ErrorAction SilentlyContinue)
    if ($code -ne 0) { $tail += @(Get-Content -LiteralPath $stderr -Tail 35 -ErrorAction SilentlyContinue) }
    foreach ($line in $tail) { Write-Host $line }
    if ($code -ne 0) { throw ($Name + ' failed with exit ' + $code + ' (see ' + $Logs + ')') }
    Record ('Completed ' + $Name)
}
function MakeJunction([string]$Target, [string]$Link) {
    Require $Target 'shared dependency directory'
    if (Test-Path -LiteralPath $Link) {
        $item = Get-Item -LiteralPath $Link -Force
        if ($item.LinkType -eq 'Junction') {
            $got = @($item.Target)[0]
            if ([IO.Path]::GetFullPath($got).TrimEnd('\') -ine [IO.Path]::GetFullPath($Target).TrimEnd('\')) {
                throw ('Junction points to unexpected directory: ' + $Link + ' -> ' + $got)
            }
            return
        }
        if (($item -is [IO.DirectoryInfo]) -and @([IO.Directory]::EnumerateFileSystemEntries($Link)).Count -eq 0) {
            Remove-Item -LiteralPath $Link -Force
        } else { throw ('Refusing to replace nonempty directory: ' + $Link) }
    }
    New-Item -ItemType Junction -Path $Link -Target $Target -ErrorAction Stop | Out-Null
}
function SyncPatchedAurora([string]$Shared, [string]$Private, [object[]]$Patches) {
    # mkw100 needs aurora changes (larger per-frame GPU buffers for 128 racers), but aurora-main is
    # a junction to a shared workspace other builds use. Mirror it into a private gitignored copy,
    # apply patches\aurora\*.patch there, and redo that copy only when either side changes.
    $stamp = Join-Path $Private '.mkw100-patch-stamp'
    $want = (Get-MkwAuroraSourceFingerprint $Shared) + ';' + (($Patches | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }) -join ',')
    if ((Test-Path -LiteralPath $stamp) -and ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq $want)) {
        Record ('Patched aurora up to date: ' + $Private)
        return
    }
    & robocopy.exe $Shared $Private /MIR /XD .git /XF .git /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw ('robocopy of shared aurora failed with exit ' + $LASTEXITCODE) }
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if (-not $git) { throw 'git.exe is needed to apply patches\aurora\*.patch' }
    $rel = [IO.Path]::GetFileName($Private)
    foreach ($patch in $Patches) {
        & $git.Source -C $Repo apply --whitespace=nowarn ('--directory=' + $rel) $patch.FullName
        if ($LASTEXITCODE -ne 0) { throw ('Aurora patch did not apply: ' + $patch.Name) }
        Record ('Applied aurora patch ' + $patch.Name)
    }
    $want | Set-Content -LiteralPath $stamp -Encoding ASCII
}
function PackageBuildEvidence { Write-Host ("Build logs: " + $Logs) }
try {
    ('MKW100 BUILD ATTEMPT: ' + (Get-Date -Format o)) | Set-Content -LiteralPath $BuildLog -Encoding UTF8
    if (Test-Path $BuildMarker) { Remove-Item $BuildMarker -Force }
    if (Test-Path (Join-Path $Logs 'build_failure.txt')) { Remove-Item (Join-Path $Logs 'build_failure.txt') -Force }
    # Aurora: the repo's aurora-main submodule (git clone --recurse-submodules) when it is checked
    # out, else a junction to the WiiCompiled install's BuildWorkspace\aurora-main (older checkouts).
    $repoAurora = Join-Path $Repo 'aurora-main'
    $repoAuroraItem = Get-Item -LiteralPath $repoAurora -Force -ErrorAction SilentlyContinue
    $useSubmodule = ($null -ne $repoAuroraItem) -and ($repoAuroraItem.LinkType -ne 'Junction') -and
        (Test-Path -LiteralPath (Join-Path $repoAurora 'CMakeLists.txt'))
    if (-not $useSubmodule) {
        Require (Join-Path $OriginalWorkspace 'aurora-main\CMakeLists.txt') 'Aurora source (run "git submodule update --init" in the repo, or keep the install''s BuildWorkspace)'
    }
    # The checkout keeps its own copies in Assets\ (copied below on the first build, hash-checked);
    # only a checkout without them needs the install's.
    if (-not (Test-Path (Join-Path $Repo 'Assets\main.dol'))) { Require (Join-Path $OriginalWorkspace 'Assets\main.dol') 'user-owned main.dol' }
    if (-not (Test-Path (Join-Path $Repo 'Assets\StaticR.rel'))) { Require (Join-Path $OriginalWorkspace 'Assets\StaticR.rel') 'user-owned StaticR.rel' }
    Require (Join-Path $OriginalWorkspace 'Dependencies') 'original cached dependencies'
    foreach ($f in @('CMake\bin\cmake.exe','Ninja\ninja.exe','llvm-mingw\bin\x86_64-w64-mingw32-clang.exe','llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe','llvm-mingw\bin\x86_64-w64-mingw32-windres.exe')) {
        Require (Join-Path $Toolkit $f) 'bundled native toolchain'
    }
    $dotnet = Get-Command dotnet.exe -ErrorAction SilentlyContinue
    if (-not $dotnet) { throw 'dotnet.exe not on PATH: fork needs a .NET SDK to compile its modified translator (the old bundled Translator.Cli.exe is incompatible).' }
    $sdkList = & $dotnet.Source --list-sdks
    if (-not $sdkList) { throw 'dotnet is present but no SDK installed. Install a .NET 8+ SDK, then rerun.' }
    Record ('dotnet SDKs: ' + (($sdkList | Select-Object -Last 3) -join '; '))
    Require (Join-Path $Repo 'projects\mkwii\mkw100\player-slot-sites.txt') 'mkw100 translation rewrite table'
    Require (Join-Path $Repo 'runtime\src\hle\boot_race.cpp') 'mkw100 race harness'
    $assets = Join-Path $Repo 'Assets'
    New-Item -ItemType Directory -Path $assets -Force | Out-Null
    $dol = Join-Path $assets 'main.dol'
    $rel = Join-Path $assets 'StaticR.rel'
    if (-not (Test-Path $dol)) { Copy-Item -LiteralPath (Join-Path $OriginalWorkspace 'Assets\main.dol') -Destination $dol }
    if (-not (Test-Path $rel)) { Copy-Item -LiteralPath (Join-Path $OriginalWorkspace 'Assets\StaticR.rel') -Destination $rel }
    $expectedDol = '80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05'
    $expectedRel = '16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d'
    if ((Get-FileHash $dol -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedDol -or
        (Get-FileHash $rel -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedRel) {
        throw 'Clean PAL RMCP01 DOL/REL checksum mismatch; refusing translation.'
    }
    if ($useSubmodule) {
        Record 'Aurora: the repo''s aurora-main submodule'
    } else {
        MakeJunction (Join-Path $OriginalWorkspace 'aurora-main') $repoAurora
        Record 'Aurora: junction to the install''s BuildWorkspace aurora-main'
    }
    MakeJunction (Join-Path $OriginalWorkspace 'Dependencies') (Join-Path $Repo 'Dependencies')
    Record 'Clean PAL inputs validated; shared Dependencies are a junction (not copied).'
    if ($ModPack) {
        if ($SplitScreen8) { $ModPack = Join-Path $Repo $ModPack }
        $ModPack = [IO.Path]::GetFullPath($ModPack)
        # The project's profile reads the pul from this workspace path, and the base
        # leg blocks leaf inlining at every address it patches, so it must be staged first.
        $stagedPulDir = Join-Path $Repo $ModStagedPul
        New-Item -ItemType Directory -Path $stagedPulDir -Force | Out-Null
        $stagedPul = Join-Path $stagedPulDir 'Code.pul'
        if ($FormulaKart) {
            # FKW ships no Code.pul: run its one-shot patcher in an interpreter and stage the result.
            $fkwPayload = Join-Path $ModPack 'fkw\FormulaKartWiiP.bin'
            $fkwXml = Join-Path ([IO.Path]::GetFullPath($FormulaKart)) 'riivolution\fkw.xml'
            Require $fkwPayload 'Formula Kart Wii PAL payload'
            Require $fkwXml 'Formula Kart Wii Riivolution XML'
            $python = Get-Command python.exe -ErrorAction SilentlyContinue
            if (-not $python) { throw 'python.exe is needed on PATH to convert the Formula Kart Wii payload.' }
            & $python.Source -I (Join-Path $Repo 'scripts\mkw100\fkw_to_kamek.py') --payload $fkwPayload --xml $fkwXml `
                --dol $dol --rel $rel --out $stagedPul --report (Join-Path $Logs 'fkw_patches.json') |
                ForEach-Object { Record $_ }
            if ($LASTEXITCODE -ne 0) { throw 'fkw_to_kamek.py failed' }
            $retroPul = [IO.Path]::GetFullPath($stagedPul)
        } elseif ($SplitScreen8) {
            $discUi = Join-Path $InstallRoot 'GameAssets\DATA\files\Scene\UI'
            Require (Join-Path $discUi 'MenuMulti.szs') 'disc menu archive (the WiiCompiled install''s GameAssets)'
            $python = Get-Command python.exe -ErrorAction SilentlyContinue
            if (-not $python) { throw 'python.exe is needed on PATH to stage MKWii 8P Splitscreen (scripts/mkw100/ss8_stage_pack.py).' }
            & $python.Source -I (Join-Path $Repo 'scripts\mkw100\ss8_stage_pack.py') --release ([IO.Path]::GetFullPath($SplitScreen8)) `
                --disc-ui $discUi --out $ModPack | ForEach-Object { Record $_ }
            if ($LASTEXITCODE -ne 0) { throw 'ss8_stage_pack.py failed' }
            $retroPul = [IO.Path]::GetFullPath($stagedPul)
        } else {
            $retroPul = Join-Path $ModPack 'Binaries\Code.pul'
            Require $retroPul ($ModName + ' Code.pul')
            if ([IO.Path]::GetFullPath($stagedPul) -ne $retroPul) { Copy-Item -LiteralPath $retroPul -Destination $stagedPul -Force }
        }
        if ($ItemFeverItems) {
            # Both packs' own Code.pul are staged too, so the base translation knows every address
            # either of them can patch; the combined module only ever patches a subset of those.
            $ItemFeverItems = [IO.Path]::GetFullPath($ItemFeverItems)
            $ifPul = Join-Path $ItemFeverItems 'Binaries\Code.pul'
            Require $ifPul 'Item Fever Code.pul'
            $ifStaged = Join-Path $Repo 'PulsarPacks\completed\ItemFever\ItemFever\Binaries'
            New-Item -ItemType Directory -Path $ifStaged -Force | Out-Null
            if ([IO.Path]::GetFullPath((Join-Path $ifStaged 'Code.pul')) -ne $ifPul) { Copy-Item -LiteralPath $ifPul -Destination (Join-Path $ifStaged 'Code.pul') -Force }
            $combinedDir = Join-Path $Repo 'PulsarPacks\combined\rr-item-fever'
            New-Item -ItemType Directory -Path $combinedDir -Force | Out-Null
            $python = Get-Command python.exe -ErrorAction SilentlyContinue
            if (-not $python) { throw 'python.exe is needed for -ItemFeverItems (scripts/mkw100/combine_pul.py).' }
            # keep-func/own-func entries need the game's function ranges, from an earlier build's manifest.
            $combineArgs = @('-I', (Join-Path $Repo 'scripts\mkw100\combine_pul.py'), '--rr', $stagedPul, '--if', $ifPul,
                '--plan', (Join-Path $Repo 'projects\mkwii\mkw100\rr-item-fever.txt'), '--out', (Join-Path $combinedDir 'Code.pul'))
            $prevManifest = Join-Path $Repo 'build\base\mkwii_base_manifest.json'
            if (Test-Path -LiteralPath $prevManifest) { $combineArgs += @('--manifest', $prevManifest) }
            $combineOut = & $python.Source @combineArgs
            if ($LASTEXITCODE -ne 0) { throw 'combine_pul.py failed' }
            foreach ($line in $combineOut) { Record ('combine_pul: ' + $line) }
            $ModProfile = 'rr-item-fever'; $ModName = 'Retro Rewind + Item Fever items'; $ModOut = 'build\mods\rr_item_fever_full_cpp'
            $retroPul = Join-Path $combinedDir 'Code.pul'
        }
        $pulSha = (Get-FileHash -LiteralPath $retroPul -Algorithm SHA256).Hash.ToLowerInvariant()
        Record ($ModName + ' Code.pul staged, SHA256 ' + $pulSha)
        # The 12-racer fixes for a pack are measured on one Code.pul; another build still runs, without them.
        $knownPul = @{ 'retro-rewind' = 'd68778e9e126ebd80d45fd4ad57882e269a4b99d121b066910ef04e0a9a4360d' }
        if ($knownPul.ContainsKey($ModProfile) -and $knownPul[$ModProfile] -ne $pulSha) {
            Write-Warning ($ModName + ' Code.pul is not the build MKW100 was measured on (' + $knownPul[$ModProfile].Substring(0, 8) +
                '...). Its racer fixes past 12 are switched off at run time, and translate-mod may stop on its array sites.')
        }
    }
    Record ('Incremental fork cache: ' + $NativeBuild)
    if ($PreflightOnly) {
        Record 'PREFLIGHT PASSED — no translator, CMake or native compilation run.'
        PackageBuildEvidence
        exit 0
    }
    $bin = Join-Path $Toolkit 'llvm-mingw\bin'
    . (Join-Path $Repo 'Launcher\NativeBuildFlags.ps1')
    $auroraDir = Join-Path $Repo 'aurora-main'
    $auroraPatches = @(Get-ChildItem -LiteralPath (Join-Path $Repo 'patches\aurora') -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name)
    if ($auroraPatches.Count -gt 0) {
        $auroraDir = Join-Path $Repo 'aurora-mkw100'
        SyncPatchedAurora $repoAurora $auroraDir $auroraPatches
    }
    $prebuilt = Resolve-MkwNativePrebuiltDirectory (Join-Path $Repo 'Dependencies') $bin `
        $auroraDir (Join-Path $Repo 'runtime\third_party')
    # The shipped package is harvested from unpatched aurora; patching makes a source build expected.
    if (-not $prebuilt -and $auroraPatches.Count -gt 0) { $AllowSourceDependencies = $true }
    if (-not $prebuilt -and -not $AllowSourceDependencies) {
        throw 'Existing native_prebuilt does not match the fork source/toolchain. Refusing surprise multi-GB source dependency build; rerun with -AllowSourceDependencies only if disk space permits.'
    }
    Record ('Native prebuilt: ' + $(if($prebuilt){$prebuilt}else{'unavailable; opted into source compilation'}))
    $cmdPath = Join-Path $env:SystemRoot 'System32\cmd.exe'
    Require $cmdPath 'Windows cmd.exe for process-exitcode smoke test'
    $translatorOut = Join-Path $Repo 'build-translator-win'
    InvokeTool $dotnet.Source @('build',(Join-Path $Repo 'translator\src\Translator.Cli\Translator.Cli.csproj'),'-c','Release','-o',$translatorOut,'-v','minimal','-nologo') '01_translator'
    $translatorDll = Join-Path $translatorOut 'Translator.Cli.dll'
    Require $translatorDll 'new fork translator CLI'
    Record 'Translator targets .NET 8; Major roll-forward allows the installed .NET 10 runtime.'
    InvokeTool $dotnet.Source @('--roll-forward','Major',$translatorDll,'--help') '01b_translator_hostcheck'
    $project = Join-Path $Repo 'projects\mkwii\recomp.yml'
    $metadata = Join-Path $Repo 'generated\base_translation_output.json'
    $functions = Join-Path $Repo 'generated\functions'
    $shards = Join-Path $Repo 'generated\build_shards'
    $baseMan = Join-Path $Repo 'build\base'
    $generated = Join-Path $Repo 'generated'
    New-Item -ItemType Directory -Path $generated,$baseMan -Force | Out-Null
    $threads = [Math]::Max(1,[Math]::Min(16,$Jobs))
    InvokeTool $dotnet.Source @('--roll-forward','Major',$translatorDll,'translate-recursive','0x800060A4','--project',$project,
        '--outdir',$functions,'--output-metadata',$metadata,'--production-source-bundle',
        (Join-Path $generated 'base_translation_sources.bin'),'--no-function-files','--prune-stale',
        '--threads',"$threads") '02_translate'
    InvokeTool $dotnet.Source @('--roll-forward','Major',$translatorDll,'emit-base-manifest','--project',$project,'--out',$baseMan,
        '--functions-dir',$functions,'--translation-output-metadata',$metadata,'--region','P') '03_manifest'
    if ($ModPack) {
        $retroOut = Join-Path $Repo $ModOut
        New-Item -ItemType Directory -Path $retroOut -Force | Out-Null
        $modArgs = @('--roll-forward','Major',$translatorDll,'translate-mod','--project',$project,'--profile',$ModProfile,
            '--base-manifest',(Join-Path $baseMan 'mkwii_base_manifest.json'),'--base-translation-output-metadata',$metadata,
            '--code-pul',$retroPul,'--mod-root',$ModPack,'--mod-name',$ModName,
            '--region','P','--out',$retroOut,'--prefer-cached-inputs','--emit-cpp','--threads',"$threads")
        if ($RetroRewind) { $modArgs += '--skip-retro-wfc' }
        InvokeTool $dotnet.Source $modArgs '03b_translate_mod'
    }
    InvokeTool $dotnet.Source @('--roll-forward','Major',$translatorDll,'generate-data-init','--project',$project) '04_data_init'
    $shardArgs = @('--roll-forward','Major',$translatorDll,'emit-build-shards','--project',$project,'--base-metadata',$metadata,
        '--base-functions-dir',$functions,'--native-source-dir',(Join-Path $Repo 'runtime\src'),'--out',$shards)
    if ($ModPack) {
        $shardArgs += @('--resolved-profile',(Join-Path $retroOut 'resolved_dispatch_profile.json'),'--retro-cpp-dir',(Join-Path $retroOut 'cpp'))
    }
    InvokeTool $dotnet.Source $shardArgs '05_shards'
    $cmake = Join-Path $Toolkit 'CMake\bin\cmake.exe'
    $configure = Get-MkwNativeConfigureArguments `
        -SourceDirectory (Join-Path $Repo 'runtime') -BuildDirectory $NativeBuild `
        -Ninja (Join-Path $Toolkit 'Ninja\ninja.exe') `
        -CCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang.exe') `
        -CxxCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang++.exe') `
        -ResourceCompiler (Join-Path $bin 'x86_64-w64-mingw32-windres.exe') `
        -DependenciesDirectory (Join-Path $Repo 'Dependencies') -NativePrebuiltDirectory $prebuilt `
        -AdditionalArguments @('-DMKW_TRANSLATED_COMPILE_JOBS=4','-DMKW_X86_CPU_PROFILE=v3',('-DMKW_AURORA_DIR=' + $auroraDir.Replace('\','/')))
    InvokeTool $cmake $configure '06_configure'
    $targets = @('WiiCompiled') + $(if ($ModPack) { @('RetroRewind') } else { @() })
    InvokeTool $cmake (@('--build',$NativeBuild,'--target') + $targets + @('--parallel',"$threads")) '07_compile'
    $builtExe = Join-Path $NativeBuild 'WiiCompiled.exe'
    Require $builtExe 'new mkw100 Windows executable'
    $sha = (Get-FileHash -LiteralPath $builtExe -Algorithm SHA256).Hash.ToLowerInvariant()
    @('BUILD_SUCCESS',('Executable: '+$builtExe),('SHA256: '+$sha),'Build cache is retained for all future changes') |
        Set-Content -LiteralPath $BuildMarker -Encoding UTF8
    Record ('BUILD SUCCESS. New executable SHA256 ' + $sha)
    PackageBuildEvidence
} catch {
    $errorText = $_.Exception.Message
    Write-Host ('[mkw100] BUILD/PREFLIGHT FAILED: ' + $errorText) -ForegroundColor Red
    ('BUILD_FAILED: '+$errorText) | Set-Content -LiteralPath (Join-Path $Logs 'build_failure.txt') -Encoding UTF8
    PackageBuildEvidence
    exit 1
}
