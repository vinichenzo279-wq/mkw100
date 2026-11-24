# MKW100

A fork of the WiiCompiled Mario Kart Wii (PAL) recompiler for big offline races: up to 255 racers. It builds from source on your own PC, from your own game files.

## What it does

- **VS race**: 1 to 255 racers. Set the count in the new **Racers** row on the VS rules page, or in F10 → Local VS.
- **Team VS**: 2 to 12 teams, chosen by team count, by team size, or with a custom size per colour. Races of more than 12 racers use MKW100's team colours, HUD tinting and team results.
- **Balloon battle**: past 12 players, with red/blue team sizes and your team picked on the battle team screen.
- **Grand Prix**: past 12 racers through the award ceremony, with points scaled to the field and a GP rank.
- **Split screen**: 2 to 4 local players, with CPUs filling the rest of the field (5 to 8 with the [8 Player Splitscreen](#8-player-splitscreen) build).
- **Laps**: 1 to 99 laps per race (F10 → Local VS → Laps). The lap counter shows up to 9.
- **200cc** in place of Mirror (the class button reads 200cc), with Pulsar's brake drifting, fast falling and cannon cap. A 200cc Grand Prix has Very Hard CPUs.
- **Harder CPUs**: the CPU row on the rules page offers Hard, Very Hard and Impossible. F10 → Local VS has a CPU difficulty slider and the speed boost of Very Hard and Impossible (+40 and +80 by default).
- Position icons 13th to 255th, a speedometer in the race HUD (Pulsar's, as Retro Rewind shows it), instant item boxes, scoring and item-distribution options for big fields, all characters/vehicles/cups unlocked, and optional 30 FPS rendering.
- **Debug item cycler**: a port of _Ro's code. In a race, hold Y and press D-pad left/right to pick an item. It is off by default.
- **F10** opens the settings overlay. The Local VS tab holds the options above. They are saved to `UserData/Config.toml` in the play folder.

Online play is not supported: the installer turns networking off.

### Retro Rewind

The same checkout can also build a Retro Rewind version from your Retro Rewind 6 pack (offline, no Retro WFC). It runs up to **160 racers**. CPUs get varied skins up to racer 100; past that they keep the stock look, because every skin loads a large kart archive and 128 of them don't fit in memory. Retro Rewind's Knockout, Lap Knockout and Royale modes work past 12 racers; Lap Knockout takes a fixed number of knockouts per lap (1 to 25, F10 → Local VS) over up to 50 laps.

Character skin packs that install into the Retro Rewind folder, such as **Character Expansion** (YouthfulAngelF), need no installer option: install them into your `RetroRewind6` folder as their readme says, then run the installer with that folder. The game log lists how many skins it found per character. The racer fixes are measured on the WheelWizard RetroRewind6 `Code.pul`; with another build they switch themselves off (and the build warns).

## Installing on Windows

Step by step for a new PC: [INSTALL.md](INSTALL.md).

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
| `play-if\` | `RetroRewind.exe` built from Item Fever, with its own `UserData`, `ItemFever\` (links to your pack) and `riivolution\ItemFever.xml` |
| `play-rr-if\` | Experimental: `RetroRewind.exe` built from Retro Rewind with Item Fever's items, with its own `UserData`, `RetroRewind6\` (links) and `ItemFeverItems\` (Item Fever's item files, merged from your download) |
| `play-fkw\` | `RetroRewind.exe` built from Formula Kart Wii, with its own `UserData`, `fkw\` (links to your download) and `riivolution\fkw.xml` |
| `play-ss8\` | `RetroRewind.exe` built from MKWii 8P Splitscreen, with its own `UserData` and `SplitScreen8\` (links to the pack the build made in the checkout) |
| `play-ikw\` | `RetroRewind.exe` built from Insane Kart Wii, with its own `UserData` and `IKWv3\` (links to your download, its `save\` copied) |
| `install-options.json` | Your WiiCompiled folder and mod choice, reused on the next run |

Your WiiCompiled installation is only read from: the game data and the clean DOL/REL stay there, so keep it where it is. Nothing in it is changed, and your original WiiCompiled saves are not copied.

### Updating

Run `git pull` and `git submodule update --init`, then run **Install-MKW100.cmd** again. It remembers your choices, rebuilds what changed and keeps your settings and saves. Once you add Retro Rewind, keep building it in that checkout (the installer does this for you). Switching it off and on again forces a full rebuild. `-NoRetroRewind` turns it off.

### Item Fever

Instead of Retro Rewind, a checkout can build an **Item Fever 1.1.2** version (offline, PAL): its 21 new items, Item Rain and Loot Party, up to 255 racers. Unzip the Item Fever download anywhere and pass the folder that holds `ItemFever\` and `riivolution\`:

```powershell
./Install-MKW100.ps1 -InstallRoot 'D:\Games\WiiCompiled' -ItemFever 'D:\Mods\item-fever-112'
```

Play with **Play-ItemFever.cmd**. It lives in `play-if\` with its own `UserData`; Item Fever's own save (`ifever.dat`) is made from that NAND's save on the first launch. Both mods build the same mod executable, so one checkout holds the base game plus **one** mod: for both, use a second clone in another folder. Known gaps: Item Fever's own per-racer data is made for 12 racers, and not all of it has been extended yet, so a crash in a big race is possible. The Boo works for every racer, but racers past the 12th can't be robbed by one yet.

### Retro Rewind with Item Fever's items (experimental)

With `-RetroRewind`, `-ItemFeverItems` adds Item Fever's items and their logic to Retro Rewind: its item table, the 21 new items and the four reworked ones, without Item Fever's menus, settings pages or save. Retro Rewind's own item rules (custom item toggles, item modes) are off in this version, Item Fever's settings stay at their defaults, Item Rain and Loot Party are off, and it is offline only (Retro Rewind's check for a modified game is switched off, so it must never go online). Headless races of up to 128 racers and every Item Fever item at 24 racers pass, and the menus lead into a race. See `docs/mkw100/rr-item-fever.md`.

You need both mods already installed, in folders of your own: Retro Rewind (`-RetroRewind auto` finds WheelWizard's copy) and **Item Fever 1.1.2** (the folder that holds `Binaries`, `Assets` and `Disc`). MKW100 ships nothing from either mod. The installer reads them from those folders and builds the combined version on your PC, with WiiCompiled, like the other builds (about half an hour).

```powershell
./Install-MKW100.ps1 -InstallRoot 'D:\Games\WiiCompiled' -RetroRewind auto -ItemFeverItems 'D:\Mods\ItemFever-1.1.2\ItemFever'
```

Play with **Play-RetroRewindItemFever.cmd**. It lives in `play-rr-if\`, so `play-rr\` keeps the plain Retro Rewind of an earlier install; this checkout's Retro Rewind executable is then the combined one. `-NoItemFeverItems` goes back to plain Retro Rewind.

Item Fever's licence allows personal, non-commercial play and forbids redistributing it, modified or not, without the author's written permission. What the installer makes (`play-rr-if\`, its executable and its `ItemFeverItems\` folder) contains both mods: keep it for yourself and don't share it. Anyone else needs their own downloads of both mods and runs the installer themselves.

### Formula Kart Wii

A checkout can also build **Formula Kart Wii** (offline, PAL; [source](https://github.com/CLF78/FormulaKartWii)): 50-lap races, its item system and two-digit lap counter, up to 255 racers. FKW is a Riivolution patch, not a Pulsar pack: the build runs its one-time patcher over your download and translates the result (`scripts/mkw100/fkw_to_kamek.py`, so **python** must be on PATH). Unzip the FKW download anywhere and pass the folder that holds `fkw\` and `riivolution\`:

```powershell
./Install-MKW100.ps1 -InstallRoot 'D:\Games\WiiCompiled' -FormulaKart 'D:\Mods\FKW'
```

Play with **Play-FormulaKart.cmd**. It lives in `play-fkw\` with its own `UserData`. Like the other mods it uses the mod executable, so it takes the checkout's one mod slot; `-NoFormulaKart` turns it off. The options FKW reads during races are in F10 → **Formula Kart** and under `[formula_kart]` in `play-fkw\UserData\Config.toml`: `green_shell` (`"blue"`, or `"green"`, `"red"`, `"banana"`, `"fib"`, `"bomb"`, `"feather"`, `"pow"`), `better_items`, `bullet_thunderclouds`, `feather_cut_indicators`, `drift_bikes` (`"default"`, `"outside"`, `"inside"`), `drift_karts` (`"default"`, `"inside"`) and `race_music` (`"normal"`, `"accelerating"`). Its other options stay at its Riivolution defaults, and its online code stays unused. FKW has no licence: build it from your own download, and don't share a built copy.

### 8 Player Splitscreen

A checkout can also build **MKWii 8P Splitscreen** by therenow9 ([source](https://github.com/therenow9/mkwii-8p-splitscreen), GPLv3): 5 to 8 local players on one screen (6 or 8 tiles) in VS and Battle, with MKW100's CPUs filling the field past 12: up to 128 racers with 5 to 8 players (eight views of a bigger field overflow the renderer's per-frame buffers), the usual 255 with 1 to 4. It uses the release's "Mario Kart Wii" build (the game plus the 8-player module, no Retro Rewind). Download the release zip (`mkwii-8p-splitscreen-1.2.0.zip`), unzip it anywhere and pass that folder; you don't need to run its setup. The build makes the character pages' extra controls from your own disc files (`scripts/mkw100/ss8_stage_pack.py`, so **python** must be on PATH):

```powershell
./Install-MKW100.ps1 -InstallRoot 'D:\Games\WiiCompiled' -SplitScreen8 'D:\Mods\mkwii-8p-splitscreen-1.2.0'
```

Play with **Play-SplitScreen8.cmd**. It lives in `play-ss8\` with its own `UserData`. Players 1-4 are the GameCube rows in F10's controller page; players 5-8 are its "Wii Remote 1-4" rows (shown only in this build), where any gamepad plays as a Wii Remote with a Classic Controller, or a real Wii Remote on that channel. The main menu gets 5 to 8 Players buttons next to the game's own. Like the other mods it takes the checkout's one mod slot; `-NoSplitScreen8` turns it off.

### Insane Kart Wii

A checkout can also build **Insane Kart Wii 3.4.1** (offline, PAL): its tracks, characters, items and modes. Extract the IKW download (and its update zips, over it) anywhere and pass the folder that holds `IKWv3\`:

```powershell
./Install-MKW100.ps1 -InstallRoot 'D:\Games\WiiCompiled' -InsaneKart 'D:\Mods\Insane Kart Wii'
```

Play with **Play-InsaneKart.cmd**. It lives in `play-ikw\` with its own `UserData`; `IKWv3\` there links to your download, except `IKWv3\save\` (IKW's own save), which is a copy. It takes the checkout's one mod slot; `-NoInsaneKart` turns it off. The first start asks you to create a licence, as IKW does on a Wii (its save is `UserData\NAND\...\rksys.ikw`). MKW100 is measured on 3.4.1's code; another IKW version needs its addresses measured again (`docs/mkw100/insane-kart.md`) and won't start until then. IKW switches game code on and off while running (its menus, settings and modes); the build compiles every switch it can find in IKW's code with both states, and logs any it couldn't (`insane-kart` lines in `UserData\Logs`). Insane Kart Wii is all rights reserved: build it from your own download, and don't share a built copy. Details: `docs/mkw100/insane-kart.md`.

## Development

Run the installer again after changing source. Registering a new native wrapper needs translations and dispatch to be regenerated, which the build does. Don't commit `Assets`, `generated`, build output, `play*`, `Dependencies`, game files or saves.

Most MKW100 code is in `runtime/src/hle` (one file per feature, each with a header comment), `runtime/include/guest_abi`, `projects/mkwii/mkw100` and the translator. Aurora changes are patches in `patches/aurora`, applied to a private copy (`aurora-mkw100\`) at build time. Design notes, access tables and the CPU-race oracle are in `docs/mkw100`. macOS builds use `scripts/mkw100/build.sh` and `build_rr.sh` (see `docs/building-macos.md`).

Before sending a change, test menu entry, a full race and the next race at 12 racers and at a big count. Say which racer counts, modes and controllers you tested. Keep 12-racer behaviour unchanged unless the change is about that.

## Attribution

A fork of WiiCompiled; the upstream documentation is in `README-UPSTREAM.md`. GPLv3, like upstream: keep `LICENSE`, upstream attribution and dependency licences (`THIRD-PARTY-NOTICES.md`). Instant item boxes follow Anarion's inactive-store approach, also documented by Unnamed at https://mariokartwii.com/showthread.php?tid=2206. 200cc follows Pulsar (MelgMKW, MIT), and so does the HUD speedometer: its layout and digit pictures are Pulsar's (`runtime/assets/mkw100/README.md`). The item cycler is _Ro's (https://mariokartwii.com/showthread.php?tid=2674). Position icon credits are in `runtime/assets/rank_icons/README.md`. Never publish game files or built game executables with this source.
