# Insane Kart Wii in MKW100

Insane Kart Wii (IKW) builds as MKW100's mod product like Item Fever: `Build-MKW100.ps1 -InsaneKart <IKWv3>`,
`Install-MKW100.ps1 -InsaneKart <download>` (stage `play-ikw\`), `Play-InsaneKart.cmd`. Measured on
**IKW 3.4.1** (the 3.3.0 download with the 3.4.0 and 3.4.1 update zips extracted over it; 3.4.1
only replaces Code.pul and Config.pul).

## What IKW is

- A **Pulsar pack** ("the everything mod"): `IKWv3\Binaries\Code.pul` (Kamek module, link base
  0x803992E0, loaded at 0x81800000 like Retro Rewind and Item Fever), `Config.pul` (Pulsar config:
  cups, ~5,600 tracks in `Tracks\`), `Loader.pul`. Its code is a fork of Pulsar with IKW's own modes
  (Item Rain, Mission Mode, battle modes, KO, item expansion with new items, Random Combinations,
  200cc, transmissions, ...), plus about 8,600 character files (`CharacterLayers\`, `MiiOutfitC\`).
  12.8 GB extracted.
- No `riivolution\` folder: its launcher (based on Retro Rewind's) reads `Binaries\boot.thfs`, which
  is a plain Riivolution XML (RR-style: the Pack patch, My Stuff, a separate-save option). The
  profile `insane-kart` (recomp.yml) points the runtime at that file and picks option `Pack`.
  "Seperate Savegame" stays off. Without its launcher IKW keeps the game save in the stage's NAND,
  renamed `rksys.ikw` (its SD copy `IKWv3\save\IKWSave<n>\savefile.ikw` is only used under the
  launcher; the installer copies that folder anyway, so nothing writes into the download). A new
  stage starts with licence creation, as IKW does on a Wii. IKW's own settings files
  (`IKWSettingsv34.pul`, `Trophies.pul`, `MissionRun.pul`, ghosts, crash reports) are written to
  `play-ikw\IKWv3\`.
- `runtime-ext-*.dol` is the launcher's SD file layer (loaded at 0x81744260 on a Wii). IKW 3.x does
  **all** of its own file I/O through a function table in it (0x81782E00; IKW's module holds that
  pointer in its data, whatever the platform), so IKW cannot run without its launcher.
  `runtime/src/hle/insane_kart_sd.cpp` provides the same twelve calls natively (open, close, read,
  write, rename, stat, mkdir, opendir/readdir/closedir, lseek, errno) over the stage folder and
  points IKW at them. It never writes through a junction into the download.
- Code.pul has **five** region chunks (P, E, J, K and a demo-disc chunk); the translator now reads
  five-chunk files (`KamekPulFile`).

## Licence

`github.com/insanekartwii/Insane-Kart-Wii` (a fork of Pulsar, last pushed 2026-08-07) carries
`LICENSE.txt`: "Copyright (c) 2026 Insane Kart Wii Team - All rights reserved", no permission to
copy, modify or distribute any part. Pulsar itself is MIT, but IKW's changes are not licensed.
So nothing of IKW is in this repository: the build translates the user's own download, as for
Item Fever and Formula Kart Wii, and a built copy must not be shared. Ask the IKW team before
distributing anything.

## What the port took

| Problem | Fix |
|---|---|
| 5-chunk Code.pul | `KamekPulFile` accepts 4 or 5 sizes; `pul_hooks.py` too |
| `0x80005EFC` (online-only .init word), `0x81500010` (branch from outside the game) | `drop_commands` in the profile (new key, each address must match one command) |
| Four `Section` hooks (`0x8062D338`...) jump through a veneer IKW writes into OS low memory (`b module` at 0x800009C8, `blr` after it); module code "returns" with `b 0x800009CC` | `LowMemoryVeneerFolder` (translate-mod): the hooks branch to the target directly, the return becomes `blr` |
| strlen hook through a null-pointer guard IKW writes at 0x800018A8 | hook dropped; `insane_kart.cpp` wraps strlen with the same guard (IKW build only) |
| IKW writes `cmplwi 255` at the four lap-count sites MKW100 raises to 98 | cmpimm sites accept 255 and keep a bound already past theirs |
| 12.8 GB of files: runtime FST synthetic extents passed the 32-bit word offset range (16 GiB) | `dvd.cpp` places added files in the disc's free ranges when they don't fit after its end |
| An IKW site shares its module address with an Item Fever site | `ArrayOverflowSites` keys by address only after the profile filter |
| 14 `bl over; .string "..."; over: mflr rX` sequences (inline strings in IKW's HUD hooks): the recomp made the bl a call, which returned into the string (null call in the race HUD) | translate-mod lists these bl sites (`PpcLifter.FindBlOverDataSites`: a bl ≤ 0x100 bytes forward onto `mflr rX`, X ≠ 0) and the lifter turns each into "LR = next address; tail-call the target", the target becoming a module function; base translation is untouched (the list is empty there) |
| IKW patches game code while running (its menus, settings and modes): KamekRuntimeWrite helpers (module 0x818001CC word, 0x81800210 halfword, 0x8180025C byte, 0x8180035C branch) rewrite game instructions between the game's word and IKW's (branches into its module, calls, nops, immediates, branch flips). A recompiled game can't take code written at run time | `Mods.RuntimePatchSites`: translate-mod finds the helpers by shape and every call with a constant address (Kamek's auto-mapped game address) and value: 272 calls, 109 game instructions with 228 variant words in 3.4.1. Each such instruction becomes its own block that reads the guest word there (`MKW100_RuntimeWord`) and jumps to the variant compiled for it; 14 more sites only change an immediate (li/addi/cmpwi/ori with the same high half), and their block reads the immediate at run time (`MKW100_RuntimeImmS/U`); a variant of that form does the same, since IKW computes some of the values it writes (`li r0, 0x78` or `0xB4` at 0x80532BCC), and the runtime sends any word of the variant's form to it. IKW hooks that return to LR+K (`mflr; addi; mtlr; blr`) after a variant call land on a dispatch (`MKW100_LocalReturn`), its function retranslated as an overlay (a no-op conditional write marks it), and the base build doesn't inline it. `insane_kart.cpp`'s helper natives write the word when the site knows it (registered by the mod's data patches, plus the word the site held first) and skip and log anything else |
| IKW's section hook re-enters `Section::CreateSectionPages` past its prologue through `lis/ori; mtctr; mr r3, r31; li r0, 0x8E; bctr` (crash: missing jump target 0x8062C3C4) | `ContinuationPlanner` finds module tail jumps whose mtctr is up to four instructions before the bctr |

## Past 12 racers

- Racedata slot reads: all 31 `mulli …, 0xF0` in the module are player-slot sites (as for Retro
  Rewind's 58).
- Per-racer `[12]` tables: `scripts/mkw100/pack_racer_tables.py --profile insane-kart` (generated block
  in array-sites.txt, table list `runtime/src/hle/insane_kart_racer_tables.inc`): 12 tables shadowed
  at 0x82E00000, 75 access sites, 14 `racer < 12` checks raised to 255, 75 kept at 12, 6 tables left
  out (initialised data, mixed element sizes, or not 12 long). IKW has far fewer than Item Fever
  (89 compares with 12 vs 558): most of its per-racer code goes through the game's own arrays.
  `insane_kart.cpp` resets the shadows when a race starts.
- Code behind a kept check simply skips racers 12+ (an effect that doesn't reach them), it doesn't
  corrupt memory.
- A table with no `racer < 12` check is invisible to the script. One is listed by hand (array-sites.txt,
  plus the table in `insane_kart.cpp`): u8[12] at 0x81872384, cleared for every racer each frame by
  IKW's hook in Kart::Manager::Update. Unshadowed, racers 20..23 zeroed `Settings::Mgr::sInstance`
  (0x81872398) right behind it, so from the first race on every settings read came from low memory:
  wrong minimap icon names (race-start crash in `SetPicturePane`), wrong character models, empty
  texts. Headless direct-boot races never showed it (the corruption only shows on the next screens).
  For a new version, check the address with a menu-path race (`MKW100_TEST_MENU_PRESS`).

## A new IKW version

Everything in IKW's module is measured on 3.4.1's Code.pul (SHA-1 of the P code 698faae1...). For
another version (3.4.0 needed all of these too):

1. `projects/mkwii/mkw100/mod-builds.txt` and `runtime/src/hle/mod_build.cpp`: the new digest
   (translate-mod's "code sha1"; until then the runtime treats the build as unknown and IKW stops at
   its first file access).
2. `pack_racer_tables.py <Code.pul> --profile insane-kart --write-sites --write-tables
   runtime/src/hle/insane_kart_racer_tables.inc`; the player-slot sites: every `mulli rD, rA, 0xF0`.
3. `insane_kart_sd.cpp` `kIkwApiPointer`: the module data word holding 0x81782E00.
4. `insane_kart.cpp`: the run-time patch helpers (store word / half / byte + cache flush) at module
   0x1CC / 0x210 / 0x25C in 3.4.1.
5. recomp.yml `drop_commands` (game addresses; translate-mod stops if one no longer matches).

## Testing

`MKW100-OpenSource\build-logs\pr-integration\run-ikw.ps1` (stage `MKW100-IKW\play-ikw`) copies the
oracle NAND's licensed `rksys.dat` to `rksys.ikw`, or the race boot ends in licence creation.
Diagnostics: `MKW100_TRACE_SECTIONS=1` (boot section and scene change requests),
`MKW100_TRACE_IKW_SD=1` (files IKW opens through its SD API).

## Results (2026-11-24, IKW 3.4.1, course 0x08, run-time patches active)

| case | racers | how | result |
|---|---|---|---|
| ikw341-12-u | 12 | full race | finishes, a4a2104753c2c87d |
| ikw341-24-i | 24 | full race | finishes, 853843866c33159f |
| ikw341-100-i | 100 | diverse roster, finish at frame 3600 | finishes, d5c81e7d4133e5e9 |
| ikw341-160-a | 160 | diverse roster, finish at frame 3600 | finishes, b15ba4a5600ea536 |
| ikw341-255-j | 255 | diverse roster, finish at frame 3600 | finishes, 0244a10d016d51e3 |
| ikw341-255-k | 255 | diverse roster, finish at frame 3600 | finishes, 491088b6d21768e3 |
| ikw341-12-v | 12 | full race, computed-immediate fix | finishes, bde81b88edcf8bdd |
| ikw341-255-l | 255 | diverse roster, finish at frame 3600, computed-immediate fix | finishes, e6e8b4e385b6de23 |

No crash and no unmapped guest touches in any of them. The first six still skipped four writes of a
computed immediate (`li r0, 0xB4`, the scan had only seen 0x78); with that fixed, the last two skip none. IKW races are not
repeatable run to run (host-clock RNG, like Retro Rewind and Formula Kart Wii). Base game 12 racers
with this branch's WiiCompiled.exe: fa906b50b69477e8 (unchanged). `Install-MKW100.ps1 -InsaneKart`
builds and stages `play-ikw\`.

255 is the cap but not fully solid: on the build just before the immediate-site change, two of four
255 runs failed: once an unmapped read (0x524D4354) in `Item::ObjBomb::OnKill`+0x174 drawing an
explosion effect (nw4r::ef draw order; no run-time patch touches that code, likely the effect pool
running out with that many bombs), and once a stall after all 255 racers finished (timed out). Neither
reproduced in the two 255 runs above. If they show up in play, lower `kInsaneKartMaxRacers` in
`local_vs.cpp` (160 has not failed).

Not tested: menus and HUD by hand (no menu harness), IKW's own modes (Item Rain, Mission Mode,
battle modes) and its items past racer 12 in play.
