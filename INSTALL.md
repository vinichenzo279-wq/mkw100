# Installing MKW100 on a new PC

These steps are for a Windows PC that already has **WiiCompiled** (the Mario Kart Wii PC port) but nothing from MKW100 yet. MKW100 is built on your own PC from your own game files. Nothing here is downloaded as a finished game.

## What you need

- About **5 GB** of free disk space.
- An internet connection for the first build.
- Time: the first build takes a long while. Later updates are much faster.

## Step 1: Check your WiiCompiled folder

Open the folder where WiiCompiled is installed, for example `D:\Games\WiiCompiled`. It must contain all of these:

| Inside the WiiCompiled folder | What it is |
|---|---|
| `Base` | the WiiCompiled program files |
| `Toolkit` | the compiler tools |
| `BuildWorkspace\Assets\main.dol` and `BuildWorkspace\Assets\StaticR.rel` | the code from your own PAL (European) Mario Kart Wii disc |
| `BuildWorkspace\Dependencies` | libraries the build needs |
| `GameAssets\DATA` | the game data from your disc |

If any of them is missing, run the WiiCompiled installer again with its full build kit and let it finish. A copy that only has the game's `.exe` is not enough. Your disc must be the **PAL** version (game ID RMCP01).

Write down the folder's path; you'll need it in step 5.

## Step 2: Install the .NET SDK (version 8 or newer)

Open **PowerShell** (Start menu, type `PowerShell`) and run:

```powershell
winget install Microsoft.DotNet.SDK.8
```

Close PowerShell, open a new one, and check:

```powershell
dotnet --list-sdks
```

It must list at least one version that starts with 8 or higher. (Without winget, download the SDK, not the "runtime", from https://dotnet.microsoft.com/download.)

## Step 3: Install Git

```powershell
winget install Git.Git
```

Close PowerShell and open a new one so `git` is found. (Without winget: https://git-scm.com/download/win, default options are fine.)

## Step 4: Download MKW100

In PowerShell, go to the folder that should hold MKW100 (spaces in the path are fine) and download it:

```powershell
cd D:\Games
git clone --recurse-submodules https://github.com/vinichenzo279-wq/mkw100
```

This makes a folder `D:\Games\mkw100`. Don't forget `--recurse-submodules`: it also downloads the graphics library the build needs.

## Step 5: Build and install

Open the `mkw100` folder in Explorer and double-click **Install-MKW100.cmd**. A window opens and asks:

1. **WiiCompiled installation folder**: paste the path from step 1 and press Enter.
2. **Retro Rewind**: press Enter to skip it (or see the README if you have Retro Rewind 6).

Then it builds. It translates the game, builds the graphics layer, then compiles everything. Leave the window open until it says:

```
Installed. Run Play-MKW100.cmd. Settings and saves are kept on updates.
```

Your WiiCompiled folder is only read from; nothing in it is changed, and your normal WiiCompiled saves are left alone.

## Step 6: Play

Double-click **Play-MKW100.cmd** in the `mkw100` folder.

- On the first launch, create a new licence in the game's menus.
- Set the number of racers (up to 255) in the **Racers** row of the VS rules page.
- Press **F10** in the game for MKW100's settings (Local VS tab: racers, teams, instant item boxes, 30 FPS rendering and more). They are saved in `play\UserData\Config.toml`.
- Online play is turned off.
- Big races are heavy: around 100–128 racers is a good balance. 255 works but runs slowly.

## Updating later

Open PowerShell in the `mkw100` folder and run:

```powershell
git pull
git submodule update --init
```

Then double-click **Install-MKW100.cmd** again. It remembers your WiiCompiled folder and keeps your settings and saves. Close the game first.

## If something goes wrong

The window shows an error and stops. The common ones:

| Error says | What to do |
|---|---|
| `Missing game data: ...GameAssets\DATA` | The path you typed is not the WiiCompiled folder. Check step 1 and run again. |
| `Missing user-owned main.dol` / `StaticR.rel` / `original cached dependencies` | Your WiiCompiled install has no build kit. Rerun the WiiCompiled installer with it (step 1). |
| `Clean PAL RMCP01 DOL/REL checksum mismatch` | The game files are not from a clean PAL disc. MKW100 only works with the European version. |
| `dotnet.exe not on PATH` or `no SDK installed` | Do step 2 again, then open a **new** window and rerun. |
| `git.exe is needed` | Do step 3 again, then open a new window and rerun. |
| `Aurora source (run "git submodule update --init" ...)` | You cloned without `--recurse-submodules`. Run `git submodule update --init` in the `mkw100` folder, then rerun. |
| `Close WiiCompiled.exe, then rerun the installer` | The game is still open. Close it and run the installer again. |
| `Build failed; see build-logs` | Something went wrong while compiling. Send the newest files in `mkw100\build-logs\` to whoever helps you. |

If you want to start over, delete the `install-options.json` file in the `mkw100` folder. The installer then asks for your WiiCompiled folder again.

Retro Rewind and Item Fever versions are described in `README.md`.
