# MKW100

A fork of the WiiCompiled Mario Kart Wii (PAL) recompiler for big offline races: up to 128 racers. It builds from source on your own PC, from your own game files.

## What it does

- **VS race**: 1 to 128 racers. Set the count in the new **Racers** row on the VS rules page, or in F10 → Local VS.
- **Team VS**: 2 to 12 teams, chosen by team count, by team size, or with a custom size per colour. Races of more than 12 racers use MKW100's team colours, HUD tinting and team results.
- **Balloon battle**: past 12 players, with red/blue team sizes and your team picked on the battle team screen.
- **Grand Prix**: past 12 racers through the award ceremony, with points scaled to the field and a GP rank.
- **Split screen**: 2 to 4 local players, with CPUs filling the rest of the field.
- **200cc** in place of Mirror. Choosing Mirror runs the race unmirrored at 200cc, with brake drifting.
- **Impossible CPUs**: a fourth CPU difficulty after Hard on the rules page.
- Position icons 13th to 128th, a speedometer, instant item boxes, scoring and item-distribution options for big fields, all characters/vehicles/cups unlocked, and optional 30 FPS rendering.
- **Debug item cycler**: a port of _Ro's code. In a race, hold Y and press D-pad left/right to pick an item. It is off by default.
- **F10** opens the settings overlay. The Local VS tab holds the options above. They are saved to `UserData/Config.toml` in the play folder.

Online play is not supported: the installer turns networking off.

### Retro Rewind

The same checkout can also build a Retro Rewind version from your Retro Rewind 6 pack (offline, no Retro WFC). It is capped at **100 racers**, because each racer loads its own kart archive with Retro Rewind's per-racer skins, and 128 of them don't fit in memory. CPUs get varied skins.

## Installing on Windows

You need:

1. **WiiCompiled**, installed and run once with the full build kit: the folder with `Base`, `Toolkit`, `BuildWorkspace` (including `Assets` with the clean PAL `main.dol`/`StaticR.rel`, and `Dependencies`) and `GameAssets\DATA`. A copy that only has the executable is not enough.
2. **A .NET SDK**, 8 or newer (`dotnet --list-sdks` must list one).
3. **Git**.
4. For Retro Rewind: a **Retro Rewind 6** folder, meaning the `RetroRewind6` folder that holds `Binaries\Code.pul`. If you let the WiiCompiled installer set up Retro Rewind, it already recorded that folder.

Then clone the repository with its submodule into any folder (spaces in the path are fine):

```
git clone --recurse-submodules https://github.com/vinichenzo279-wq/mkw100
```

Double-click **Install-MKW100.cmd** in that folder. It asks for your WiiCompiled folder and whether to build Retro Rewind too. Or run it from PowerShell:

```powershell
./Install-MKW100.ps1 -InstallRoot 'D:\Games\WiiCompiled'                        # base game only
./Install-MKW100.ps1 -InstallRoot 'D:\Games\WiiCompiled' -RetroRewind auto      # plus the recorded Retro Rewind folder
./Install-MKW100.ps1 -InstallRoot 'D:\Games\WiiCompiled' -RetroRewind 'D:\RR\RetroRewind6'
```

Play with **Play-MKW100.cmd**, or **Play-RetroRewind.cmd** for Retro Rewind. Create a new licence in the game's menus on the first launch.

The first build takes a while: it translates the game, builds aurora (the graphics layer) with MKW100's patches, then compiles everything. Logs go to `build-logs\`. Later builds are incremental.

### What goes where

| Folder | Contents |
|---|---|
| `play\` | `WiiCompiled.exe`, the base game, with its own `UserData` (settings, NAND saves, logs) |
| `play-rr\` | `RetroRewind.exe` with its own `UserData`, plus `RetroRewind6\`, a folder of links to your pack. Retro Rewind's own save lives in `play-rr\riivolution\save`, so your Dolphin or WheelWizard save is left alone. |
| `install-options.json` | Your WiiCompiled folder and Retro Rewind choice, reused on the next run |

Your WiiCompiled installation is only read from: the game data and the clean DOL/REL stay there, so keep it where it is. Nothing in it is changed, and your original WiiCompiled saves are not copied.

### Updating

Run `git pull` and `git submodule update --init`, then run **Install-MKW100.cmd** again. It remembers your choices, rebuilds what changed and keeps your settings and saves. Once you add Retro Rewind, keep building it in that checkout (the installer does this for you). Switching it off and on again forces a full rebuild. `-NoRetroRewind` turns it off.

### Item Fever (coming)

An Item Fever version is in progress. It builds the same mod executable as Retro Rewind, so one checkout holds the base game plus **one** mod. For Item Fever, use a second clone in another folder, with its own `play-if\`.

## Development

Run the installer again after changing source. Registering a new native wrapper needs translations and dispatch to be regenerated, which the build does. Don't commit `Assets`, `generated`, build output, `play*`, `Dependencies`, game files or saves.

Most MKW100 code is in `runtime/src/hle` (one file per feature, each with a header comment), `runtime/include/guest_abi`, `projects/mkwii/mkw100` and the translator. Aurora changes are patches in `patches/aurora`, applied to a private copy (`aurora-mkw100\`) at build time. Design notes, access tables and the CPU-race oracle are in `docs/mkw100`. macOS builds use `scripts/mkw100/build.sh` and `build_rr.sh` (see `docs/building-macos.md`).

Before sending a change, test menu entry, a full race and the next race at 12 racers and at a big count. Say which racer counts, modes and controllers you tested. Keep 12-racer behaviour unchanged unless the change is about that.

## Attribution

A fork of WiiCompiled; the upstream documentation is in `README-UPSTREAM.md`. GPLv3, like upstream: keep `LICENSE`, upstream attribution and dependency licences (`THIRD-PARTY-NOTICES.md`). Instant item boxes follow Anarion's inactive-store approach, also documented by Unnamed at https://mariokartwii.com/showthread.php?tid=2206. 200cc follows Pulsar (MelgMKW, MIT). The item cycler is _Ro's (https://mariokartwii.com/showthread.php?tid=2674). Position icon credits are in `runtime/assets/rank_icons/README.md`. Never publish game files or built game executables with this source.
