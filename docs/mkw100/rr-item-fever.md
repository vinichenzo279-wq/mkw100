# Item Fever's items in Retro Rewind: feasibility

Status (2026-11-15, branch `rr-item-fever`): the go/no-go test below **passed**, and the build now runs headless races of up to 128 racers with Item Fever's item system in Retro Rewind. Not pushed, not play-tested from the menus. Measured on the packs MKW100 supports today:

- Retro Rewind: WheelWizard RetroRewind6, Kamek code SHA-1 `5061374c...`.
- Item Fever 1.1.0: SHA-1 `2c6c40ca...`. Since 2026-11-17 (rebased onto wiicompiled-0.9.4) the build takes Item Fever 1.1.1 (`47816337...`), the version MKW100 recognises; the plan's entries are game and Retro Rewind addresses and hold for both, and the three game patches 1.1.1 adds (its new settings, off by default) are left out like the rest of its settings. Since 2026-11-23 it takes Item Fever 1.1.2 (`cc8a2cd8...`) instead: its code and data moved, so the Item Fever addresses in the site lists and runtime were remapped (1.1.2 also reworked the Snowball's code; its per-racer entries were regenerated). Of the five game patches 1.1.2 adds, two are kept (the Custom Items fix in the roulette, a sound hook) and three left out (the real-Wii Frame Rate setting and an effects speed-up); see the plan's header. The figures below were measured on 1.1.0.

The numbers come from `scripts/mkw100/pul_hooks.py` (see the end of this page).

## Short answer

Bringing Item Fever's items into the Retro Rewind build is possible, but only with a lot of work, and nobody can promise it works until a test build shows it.

What makes it possible:

- Item Fever's module fits in the 2 MiB Kamek overlay next to Retro Rewind's.
- Kamek modules can be relocated to another base address.
- MKW100 already translates both packs and already makes both run past 12 racers.

What makes it hard: these are not two add-ons but **two complete copies of the Pulsar engine**.

- Of Item Fever's 1,154 patches to the game, 548 sit at addresses Retro Rewind patches too. At 340 of those, both packs branch into their own code.
- Among those 340 are Pulsar's own hook dispatchers (boot, race load, race frame, section load). With only one pack's branch at a dispatcher, the other pack's whole hook list never runs.
- Item Fever's item code is not a separate part of its module. It reads Item Fever's own settings, its own Pulsar `System`, its own asset banks and its own item table. All of these are set up by the menus and boot code we would leave out.
- There is no source code and there are no symbols, only the binary.

**Recommendation:** option A below, staged.

1. Start with a 1-week **go/no-go test**: one new Item Fever item usable in a headless Retro Rewind race, with Item Fever loaded as a second module.
2. Only if that works, the new items without Item Rain and Loot Party. Retro Rewind has its own Item Rain.
3. Build it as a separate, offline-only executable, not as a switch inside the normal Retro Rewind build.

At the same time, the user can ask Item Fever's author for permission and for the item source code. With the source, this becomes a much cleaner source-level merge with Retro Rewind's public `rr-pulsar` (option D).

The licence (see below) rules out a native port of Item Fever's code (option B).

## Go/no-go test: result

**Passed.** It also needed much less than the estimate in option A, because a simpler route turned up: one **combined** Kamek module instead of two translated modules. `translate-mod` and the rest of the build needed no change.

### How it works

- **`scripts/mkw100/combine_pul.py`** builds one raw Kamek chunk from both packs' `Code.pul`:
  - Retro Rewind at 0x81800000, Item Fever at +0xC0000 (0x818C0000), with Item Fever's bss as the chunk's bss (ends at 0x819978C8, below the player slots at 0x81A00000).
  - One constructor table holding both packs' 102 constructors.
  - Its two un-relocated references to its own linked image are moved too.
  - All of Retro Rewind's patches are kept. Of Item Fever's patches to the game, only those the plan names are kept: **`projects/mkwii/mkw100/rr-item-fever.txt`**.
  - Since both packs' patches end up in one module, a game function patched by both gets one patched copy containing both.
- **Pulsar's hook dispatchers** (boot, race load, race frame), which both packs branch to from the same instruction, get a small PowerPC trampoline in the combined module. It calls Retro Rewind's dispatcher, then Item Fever's, with the caller's r3-r5.
- **The plan** (388 of Item Fever's 1,154 patches are kept):
  - Item Fever owns every item-system function it patches (`own-func`: the item table expansion, item objects, CPU item use, inventory, item HUD). Retro Rewind's own item patches in those functions (item toggles, its item slot tables) are left out.
  - Item Fever's patches in item, kart, object and effect functions that Retro Rewind doesn't patch at all are kept.
  - Item Fever's data writes its items need are kept: vtable slots for its item and object methods, the item property table, sound ids, resource names.
  - A few patches are taken from Item Fever instead of Retro Rewind: Pulsar's model-file lookup.
  - Retro Rewind's boot-time check that game code is unmodified ("Error 148") is stubbed: this build patches the game on purpose, and stays offline.
  - Item Fever's menus, settings pages, save handling (`ifever.dat`), online lock-out and its own heap-size changes are left out.
- **Item Fever's files**: `scripts/mkw100/stage_rr_item_fever.py` makes `<stage>/ItemFeverItems`, a Riivolution root added to the stage's `UserData/Config.toml` (`[paths] overlay_roots`). It holds:
  - Item Fever's `Config.pul`, under the name the combined module reads.
  - `Race/Common.szs` built from Item Fever's `Common.szs` and `CommonAssets2.szs`.
  - `RaceAssets.szs` built from Retro Rewind's, Item Fever's and its `mkwci` icon textures.
  - Item Fever's `revo_kart.brsar`.

  All of it comes from the user's own copies of both packs (`scripts/mkw100/merge_szs.py` merges archives).
- **Build**: `Build-MKW100.ps1 ... -RetroRewind <RetroRewind6> -ItemFeverItems <ItemFever pack>` builds `RetroRewind.exe` from the combined module (profile `rr-item-fever` in `recomp.yml`).
- **Test hook**: `MKW100_TEST_GIVE_ITEM=<id>` hands every CPU holding nothing that item every 300 frames (`runtime/src/hle/rr_item_fever.cpp`).

### What the headless runs showed (12 racers, Luigi Circuit)

- **Both packs boot.** Retro Rewind starts, then Item Fever logs `*** Item Fever: Pulsar started ***`, loads its config and installs its items ("Ice Flower ready (slot 0x1B ...)", "FEATHER item installed on slot 0x1D", "Golden Mushroom rework active", and others).
- **Plain race**: it finishes with a full classification. The CPUs pull items from Item Fever's item table. The log shows its Purple Shell, Black Shell, Banzai Bill, Chain Chomp, Rainbow Mushroom, grapple star, throwable banana and Thunder Cloud rework code at work.
- **Ice Flower** (`MKW100_TEST_GIVE_ITEM=0x1B`): every 5 s about 8 CPUs got one, and by the next round almost all had used it. 13 rounds, and the race finished.
- The final check ran on a stage made by `stage_rr_item_fever.py`.

### Blockers met on the way, in order

Each fix is in the plan or the stage script:

1. Retro Rewind's code check "Error 148" stopped the boot.
2. Item Fever needs its `Config.pul` ("Invalid Pulsar Config").
3. A merged `Common.szs` stored without compression was too large for Pulsar's archive loader.
4. Fake Item Box crash: Item Fever's data patches (vtable slots, resource names) were missing.
5. The race scene exited before racing, caused by Item Fever's heap-size patches (now left out).
6. Retro Rewind's item HUD read past its 19-item tables, until Item Fever took over the item window, item balloon and `GetItemIconPaneName`.

### Open points

- **Determinism: fixed.** Two runs of the same case used to agree up to race frame 4800, then diverge.
  - **Plain Retro Rewind** (the 0.9.4 build's `RetroRewind.exe`) had the same problem, even earlier: two identical 12-racer runs differed by frame 1200. So it was not caused by the combination.
  - **The cause**: the game's `Random()` constructor and `Random::Reset()` seed from `OSGetTick`, which is the host clock in the recomp. The game seeds its own race randomness from race data, so the base game repeats exactly. But Retro Rewind (15 call sites) and Item Fever (14) create clock-seeded `Random` objects for their own features.
  - **The fix**: `runtime/src/hle/scripted_random.cpp` gives those objects seeds from a fixed sequence in scripted test races (`--race-cpu`, `--race-auto`). Normal play keeps clock seeds.
  - **Result**: two runs of the plain combined case give the same result `a3be7efeb9b046a7` at every trajectory checkpoint, and two runs of the Ice Flower case give `a36ef50f0c8aeca3` (results of the build before the 12-racer cap was lifted; current ones below).
  - **Not covered**: Item Fever's 5 direct `OSGetTick` reads are in its UI controls and don't affect the race.
  - **Applies to the normal Retro Rewind build too.** Its headless results become repeatable once this file is in that build.
- **Past 12 racers: done, up to 128.** Headless races of 12, 24, 48, 64, 100 and 128 racers finish with a full classification, including 128 racers with an Ice Flower handed to every CPU every 5 s and 128 racers with MKW100's item mechanics test (lightning and POW from racers past 12). What it took:
  - **Recognising the build at run time.** The combined module's own digest changes whenever the plan changes, so `combine_pul.py` writes a marker at 0x818AFF00: `MKW100 RR+IF v1` and both source packs' Kamek code SHA-1. `mod_build.cpp` gives `Kind::RetroRewindItemFever` when both digests are builds it knows. That lifts the cap to 128 and switches on Retro Rewind's racer fixes (`rr_skins.cpp`). `combine_pul.py` refuses packs whose digests aren't in `mod-builds.txt`, since the plan holds addresses of those builds.
  - **The site lists.** `projects/mkwii/mkw100/profile-merges.txt` makes profile `rr-item-fever` take the `retro-rewind` sites as they are and the `item-fever` sites moved by +0xC0000 (every value in the Kamek overlay moves, shadow tables don't): 187 array sites and the player-slot sites. In the translator this is `Translator.Core/Lifting/ProfileMerges.cs`, with unit tests.
  - **Item Fever's run-time hooks** (`item_fever.cpp`: resetting its racer tables' shadows) are registered at the moved addresses too, and use the moved tables; `item_limits.cpp` and `item_cycler.cpp` treat the combined build as Item Fever. `guest_versions.inc` now covers the whole 2 MiB overlay (0x81800000-0x819FFFFF).
  - **More of Retro Rewind's item code left out.** The first 24-racer run stopped on a Thunder Cloud without a model. Retro Rewind's Mega Thunder Cloud handler (0x80580630) had handled the strike instead of Item Fever's cloud rework. Item Fever now also gets that site, banana damage and Pulsar's three Feather physics sites, and owns `Item::InitAllObjProperties` / `Item::LoadObjProperties`, which removes Retro Rewind's own item rules (custom item toggles, item modes, Battle Royale, its Item Rain's "all items can land").
  - **Results** (repeatable): 12 racers `59e215a1e8678039`, 24 `a56bc6fd42641cf9` (twice), 100 `4629df62208e7535`, 128 `f3f5c1ee1974502f`.
- **Silent countdown: fixed** (found in the user's first play test, 2026-11-16: no countdown or start sounds until the music began).
  - **The cause**: Item Fever's `revo_kart.brsar` has a sound group 14 that does not fit the game's 6 MiB boot sound heap (`Audio::Manager::InitSelf`, `lis r3, 0x60` at 0x80717280). Item Fever sets it to 10 MiB; the combined build had left that patch out. `MKW100_TRACE_SOUND=1` (`runtime/src/hle/sound_trace.cpp`) showed group 14 failing to load and 128 sounds per 12-racer race failing with StartResult 4.
  - **10 MiB alone stops the boot**: with both packs loaded, the heap the game passes has only 0x600060 bytes free, so `EGG::SoundHeapMgr::CreateSoundHeap` gets nothing, creates no sound heap, and `SoundArchivePlayer::SetupMram` fails. The system debug heap is no help either: the root scene's heaps have taken it by then. Dropping Retro Rewind's race sound-heap change instead (`MiscSound.cpp`) ran a 100-racer race out of memory.
  - **The fix**: the plan sets 10 MiB (`write32 0x80717280`), and `rr_item_fever.cpp` hooks `Audio::Manager::InitSelf`. In the combined build, when the passed heap cannot hold 10 MiB + 1 MiB, the audio heaps come from the root scene's debug heap (about 59 MiB free, alive as long as the game).
  - **Result**: group 14 loads. The only sounds that don't start are StartResult 1 (lower priority than the voices already playing) and, at 100+ racers, StartResult 9 (not enough sound instances), which is expected with that many karts. Race results are unchanged: 12, 100 and 128 racers give the same results as before.
- **Item audit: Item Fever patches restored** (user's second play test, 2026-11-16: the Banzai Bill ran at Bullet Bill speed, never ended and stayed in the item slot; the penguin suit made the player invisible).
  - **The cause**: the plan kept whole functions only when Retro Rewind does not patch them. That dropped Item Fever's patches at sites Retro Rewind leaves alone when the function itself has a Retro Rewind patch somewhere else, for example `Kart::Movement::Update`, `Kart::Player::ApplyPhysics`, `Kart::Collision::CheckItemCollision`, `RaceCamera`, `ObjectsMgr`, and the item objects' own functions. 579 of Item Fever's 1154 game patches were left out, and 162 of those sit at a site no Retro Rewind patch uses.
  - **The fix**: 85 of them, in race code, are now kept (`keep` block at the end of `rr-item-fever.txt`; 487 Item Fever patches kept in all). Still left out: menus, online, heap sizes, file loading, course lighting and fog, battle and team scoring, start-grid physics. The clashing sites are mostly the same Pulsar feature in both packs at different module offsets, and Retro Rewind's copy stays.
  - **Result**: with the Banzai Bill (0x1E) handed to every CPU, its whole cycle now runs as in plain Item Fever: "goes Banzai Bill", the leader cutoff, "Banzai Bill over" and the inventory flush. Before, it never ended. The penguin suit (0x27) is picked up and used as in plain Item Fever, but whether it can be seen needs a play test. 12, 24, 100 and 128 racers (128 with the item test) finish. Race results changed with the item code (12 racers `533f2d123454882b`).
- **Item models at 100 racers: fixed** (user's third play test, 2026-11-16, 100 racers: Pull Star, Shell Bomb, Purple Shell and the giant shell came out as green shells; the Chain Chomp, Snowball and wand did nothing; a ? block crashed in `Item::ObjKumo::UpdateModelPosition` on an object without a model).
  - **The cause**: Item Fever checks `EGG::ExpHeap::getTotalFreeSize` on the race scene's heap before it loads models, about a dozen times: its projectile model sets, its dropped items, the penguin suit. If the heap reports too little, it skips the model: "sets loaded 0" and "SUBSTITUTION: family N spawned green", or "scene space exhausted". Past 12 racers that heap is nearly full. But MKW100's `race_heap.cpp` moves an allocation that doesn't fit to the scene's other struct heaps (the debug one has tens of MB), so the allocation would have worked. **Plain Item Fever behaves the same** at 100 and 128 racers (40 substitutions in a headless race).
  - **The fix** (`item_fever.cpp`, one wrapper on getTotalFreeSize for calls from the mod's code, with `race_heap.cpp`'s `RaceHeap_FallbackFree`): with Item Fever's code loaded (alone or combined) and the overflow active, a scene struct heap's free size includes the free size of the heaps an allocation would move on to. In the combined build the overflow is also active at 12 racers or fewer: with both packs the scene heap has 850 KB left at 12, under the 900 KB Item Fever wants for dropped items. Base game and plain Retro Rewind are unchanged.
  - **Result**: 12, 24, 100 and 128 racers (128 with the item test): 0 substitutions (40 before at 100), dropped-item models loaded, nothing out of memory. The penguin suit was handed to every CPU at 100 racers and used. 12 racers now give `2e6186f8ca8550ae`, because dropped items exist. Each item still needs a check by eye.
- **Item audit, part 2** (user's RR+IF 1.1.2 play test, 2026-11-24: the Chain Chomp, Snowball, Ice Flower and Rock did nothing; no item timer bar or Purple Shell distance meter).
  - **The cause**: more Item Fever patches the plan left out. Its `ArchiveMgr::GetFile` / `GetKartArchiveFile` (a kart-archive lookup of "honeBall", the Snowball, or "ice.brres" that fails is retried in Common) and `CtrlRaceCount::OnUpdate` (its timer and distance texts) clash with nothing. Its objects (Chain Chomps, Goombas, ...) come from its wrapper of `ObjectsMgr::CreateInstance`'s two calls of `CreateAllObjects`, and its Rock code from its wrapper of `Kart::Movement::Update`'s call of `Kart::Boost::Update`; Retro Rewind wraps the same calls, and its wrappers had won.
  - **The fix**: the three are kept, and the two wrapper pairs are nested (`combine_pul.py` `nest-ctr` / `nest-bl`): the game calls Item Fever's wrapper, whose call of the game function goes to Retro Rewind's wrapper of that call site.
  - **Result**: "pass 0/1: ... 2 Chomp(s) injected", Chain Chomp throws and Snowballs (SNOW-HAND, SNOW-LIFE) now show in a 48-racer item race as in plain Item Fever. 12 racers give `f73985f4818149ca` (the restored item code changes the race). The timer bar and the distance meter need a check by eye.
- **Boo past 12 racers** (both builds): a racer 12+ who used a Boo Mushroom stayed a ghost for the rest of the race. Item Fever's per-frame Boo pass runs min(racers, 12) times, a bound that stays at 12 (it also bounds the column of its [12][12] pass-through table); `item_fever.cpp` now runs the ghost and spin countdowns of racers 12+ after it, on their shadows. The pass-through (stealing) of every pair with a racer 12+ runs there too (`BooPassPast12`, Item Fever's checks, steal and effects, its own 60-frame counters per pair); the steal of a held item reaches racers 0-63 (its tables hold 64). A steal needs the two karts within 140 units: at 80 racers with a Boo for every fourth racer, 9 pass-throughs and 1 steal (p36 from p79) in one race.
- **Pull Star grab past 12: fixed** (both builds). Its grab (+0x2DEB4) needed owner and victim under 12. `pack_racer_tables.py` now shadows its state table in initialised data (+0xA2DF8, reset to 0xFF for all racers; `INITIALISED_TABLES`), counts the owner's `Item::Player` and Racedata's team entries (+0x35CDC) as game arrays sized for every racer, and raises the grabbed racer's gate after a check by hand (`PASSED_ON_SAFE`). The owner reaches racers 0-55 (its tables hold 56). At 80 racers with items: grabs of racers 12+ (24 -> 66, 40 -> 75, 68 -> 73).
- **Freeze past 12: up to 24 racers** (both builds). Item Fever's freeze (Ice Flower, Penguin Suit; +0x3E0C8) skipped a victim 12+, and its per-frame pass (+0x3F8E4: thaw countdown, ice block) ran racers 0-11 only. `pack_racer_tables.py` now raises that pass's racer loop (`LOOP_COUNTER_OK`: the function's other loops step pointers, this one does not), shadows the ice-block slot table (+0xA2E64) and the u32[12] at +0xCF9D0 (its `rlwinm 2, 22, 29` accesses below 12 by hand, `NOT_RACER_SITES`), marks the slot-taking functions (`NOT_RACER_PARAMS`) and raises the freeze's victim gate. Two tables of the pass hold 24 racers (+0xB5664, +0xB7244), so the pass and the freeze stop at 24 (`RAISE_LIMIT`): a racer 24+ is not frozen, never frozen for good. `item_fever.cpp` completes the pass's last two loops for racers 12-23 (their ice block shown, a thawed racer's slot cleared), starts their slots at 0xFF, and gives a racer 12+ its own ice block when Item Fever's choice (which sees racers 0-11 only) is taken. `MKW100_TEST_FREEZE=<racers>` freezes them at frame 900 through Item Fever's own freeze: at 40 racers, 14, 20 and 23 frozen and thawed, 30 left alone. The Penguin Suit taken off by a freeze (+0x34780) still covers racers 0-11 only.
- **Not checked from the menus**: character select, the item icons on screen, results, the next race. Headless runs don't show the HUD.
- **Item Fever's settings stay at their defaults** (Settings.pul in its own NAND folder), with no way to change them in this build. Item Rain and Loot Party are off.
- **Assets.** The merged `RaceAssets.szs` keeps all of Retro Rewind's files except the item icon bank `PULitem.brlyt` (only 4 files differ between the two packs; the other three are Retro Rewind's knockout end screen, course info and timer controls). "Boo: animation 3 MISSING" is logged by plain Item Fever in MKW100 as well, so it isn't caused by the merge.
- **Retro Rewind's own Item Rain** (switched on in Retro Rewind's menus) is still in this build and untested with Item Fever's items; it does nothing unless turned on.
- **Playing it**: `Install-MKW100.ps1 -RetroRewind ... -ItemFeverItems <Item Fever download>` builds it and stages it into `play-rr-if\` (with `ItemFeverItems\` and the `overlay_roots` line), launched by `Play-RetroRewindItemFever.cmd`; `play-rr\` is left alone.
- **The other builds still pass with this branch's shared changes** (runtime, translator), tested headless on 2026-11-15:
  - base game: 12 racers `fa906b50b69477e8` (3 runs, two build configurations), 24 `924b7ee927786e20`, 128 `5576bba5f6e7ecec`, 128 with the item test `82a35cdb20a4e222`: all four are MKW100's existing baselines;
  - plain Retro Rewind: 12 racers `e6b5e28c2590f6dd` twice (repeatable now), 24, 100, 128 and 128 with the item test finish, pack fixes on;
  - plain Item Fever: 12 racers `19f921d8b3158f48` twice, 24, 100, 128 and 128 with the item test finish, pack fixes on.
- **Online must stay off** for this build. Retro Rewind's code check is stubbed, and the items differ from everyone else's.
- **The licence question** (below) is unchanged: this runs the user's own copy, but the author's permission is still the decent thing to ask for before sharing it.

## How the two packs run in MKW100 today

- `translate-mod` takes the PAL Kamek chunk of a pack's `Code.pul` and relocates it statically to `module_guest_base` 0x81800000 (`projects/mkwii/recomp.yml`). It turns every module function into C++.
  - Each Kamek command with an absolute address becomes one of these:
    - A patched copy of the base function it lands in (an "overlay function": a branch into the module, or a written instruction).
    - A data patch.
  - One mod product (`RetroRewind.exe`) holds exactly **one** module. Both packs build that same target, which is why a checkout builds only one of them.
- The overlay region `MEM1_KAMEK_OVERLAY` is 0x81800000-0x81A00000 (`runtime/src/memory.cpp`). MKW100's overflow player slots start right above it at 0x81A00000 (`guest_abi/player_slots.h`).
- MKW100's pack fixes hold absolute module addresses:
  - Item Fever: 183 `profile=item-fever` sites in `array-sites.txt`, 43 in `player-slot-sites.txt`, plus `runtime/src/hle/item_fever.cpp`.
  - Retro Rewind: 4 + 58 sites plus `rr_skins.cpp`.
  - `mod_build.cpp` turns them off for an unknown pack build.

## Module sizes and layout

| | code | bss | module (code + bss) | static constructors |
|---|---|---|---|---|
| Retro Rewind | 0x77944 (478 KiB) | 0x38158 (224 KiB) | 0xAFA9C (702 KiB) | 46 |
| Item Fever 1.1.0 | 0xA0DB0 (643 KiB) | 0x36B18 (218 KiB) | 0xD78C8 (862 KiB) | 56 |

Together that is 1.53 MiB of the 2 MiB overlay.

- Retro Rewind also places its Retro WFC helper module right after its bss (size not measured here, it comes from the server payload).
- With Retro Rewind at 0x81800000 and Item Fever at, say, 0x818C0000, Item Fever ends at about 0x81998000. That leaves roughly 400 KiB below the player slots.
- Branches from game code into either module stay well within the ±32 MiB a `b`/`bl` can reach.

## What Item Fever's item code touches

Item Fever's module (its log strings call it "MKWCI", Mario Kart Wii Custom Items) contains far more than items:

- Its menus, the Mod Settings pages and the Item Rain / Loot Party buttons.
- Its save file `ifever.dat`, made from a copy of `rksys.dat`.
- 200cc and Very Hard unlocks, the dark theme, Time Trial ghosts and random tracks.
- An online lock-out and the Mario Kart World music support.

### Item Fever's patches to the game, by area

How many of them land where Retro Rewind patches too:

| area | IF patches | same address as RR | both branch to their own code | same function only |
|---|---|---|---|---|
| items, karts, objects, effects, AI, map icons | 390 | 114 | 76 | 77 |
| menus, UI, HUD | 292 | 210 | 137 | 25 |
| unnamed StaticR functions (mostly UI pages) | 220 | 51 | 8 | 5 |
| race / scene / archive / core | 119 | 59 | 44 | 21 |
| online (RKNet, DWC) | 85 | 80 | 54 | 1 |
| sound | 44 | 32 | 21 | 0 |
| save (RKSYS) | 4 | 2 | 0 | 2 |
| **total** | **1,154** | **548** | **340** | **131** |

Of the 548 shared addresses:

- 175 are identical writes, the same Pulsar patch in both packs, so they don't conflict.
- 340 are branches in both packs.
- The rest are different writes at the same address, for example:
  - Lap count limits: IF 9, RR 99 (`Lakitu::Player::OnStartLap`).
  - Different `ObjectsMgr` capacities: IF 0x640, RR 0x500.
  - Different item object properties at 0x808B5C24.

### Item side

The item side alone is about 390 patches. They fall into these groups:

- **Item table expansion**, based on mkw-item-expansion (MIT):
  - `Item::Manager::Init`, `Item::ObjHolder::Spawn/Update`, `Item::SlotData::Init/DecideItem/DecideRouletteItem` and the `Setup*Probabilities` functions.
  - Their item-count compares are patched to 0x2B, where the game has 19 items.
  - `Item::PlayerInventory::SetItem/LoseItemFromDmg`, `Item::PlayerRoulette::DecideItem`, `Item::PlayerObj::UseItem`.
  - The `Item::Obj*::InitSelf` of several shells and the banana.
  - The incoming-item and item-window HUD (`CtrlRaceItemBalloon`, `CtrlRaceItemWindow`), plus the map icons.
- **Kart reactions**:
  - About 20 `Kart::Damage::Apply*` call sites.
  - `Kart::Collision::CheckItemCollision/HandleShellCollision/...`, `Kart::Movement::Update`, `Kart::Player::ApplyPhysics`, `Kart::Sub::Update` (Item Fever's per-kart routine).
  - Effects (`Effects::Player::*`), `RaceCamera` (Giga/Banzai), `ObjectsMgr` (Chain Chomp, dropped items, item boxes).
- **Per-racer state** in [12] tables inside Item Fever's bss. These are the 226 `profile=item-fever` sites MKW100 already rewrites for 13 to 128 racers.
- **Item Rain and Loot Party**: their own tables, spawning through the same `ObjHolder` paths, timing from Pulsar's race-frame hook list, and settings from Item Fever's Mod Settings.
- **Assets the items need**:
  - Models (`mkwciPenguin.brres`, `mkwciBanzaiBill.brres`, `black-shell.brres`, `gigaMushroom.brres`, ...) in Item Fever's replacement `Race/Common.szs`.
  - Effects referenced by name (`rk_noirKoura`, `mkwciFlamme`, ...).
  - Item icons `MKWCI_00`-`MKWCI_27` in a `PULitem` icon bank from its UI archives.
  - Sounds in its replacement `sound/revo_kart.brsar`.
  - CPU tables (`mkwciHardAI.bin`, ...).
- **Settings the item code reads**:
  - Item Fever / Original for the four reworked items.
  - The Mega Thunder Cloud row, Draggable Items and the Blooper toggle.
  - All Item Rain options.
  - These live in Item Fever's Pulsar settings, which its own boot code loads.

### Retro Rewind's own item features

Retro Rewind has its own item features on the same functions (from its public source, `rr-pulsar`):

- `Race/CustomItems.cpp`: item toggles, and `ItemSlotRR.bin`, `ItemSlotRandom.bin` and `ItemSlotBlast.bin` tables, patching `Item::SlotData::DecideItem`, `PlayerInventory::SetItem` and `LoseItemFromDmg`.
- `Race/ItemBehaviour.cpp`: object properties.
- Pulsar's Feather and Mega Thunder Cloud.
- Its own Item Rain (`Gamemodes/ItemRain`).

Item Fever's Feather and Mega Thunder Cloud started from the same Pulsar code.

## Which hooks clash

**Pulsar's hook dispatchers.** These Pulsar sites are patched by both packs, each to its own dispatcher:

| site | dispatcher |
|---|---|
| 0x80543BB4 | `BootHook` |
| 0x80554728 (and 0x8083822C) | `RaceLoadHook` |
| 0x80601C60 | `FrameLoadHook` |
| 0x8053369C | `RaceFrameHook` (`Raceinfo::Update`) |
| 0x8063507C | `SectionLoadHook` |
| 0x8051AC40 | `System::OnSceneEnter` |

Whichever branch the overlay keeps, the other pack's whole list of boot, race and frame callbacks is lost. The fix is a native wrapper in MKW100 that calls both dispatchers, one after the other. This is the central piece that has to work.

**Item logic on the same instructions.** Of the 390 item-area patches, 76 are branches at addresses Retro Rewind branches too, and 77 more land in functions Retro Rewind also patches elsewhere. Examples:

- `Item::SlotData::DecideItem` 0x807BB7D8: both packs replace the limit check.
- `Item::SlotData::Init/Setup*Probabilities`: both packs' Pulsar `ItemSlot.bin` loaders.
- `Item::PlayerInventory::LoseItemFromDmg` 0x807BC6C4.
- `Item::ObjHolder::Update`, `Item::Manager::Init`.
- `Kart::Movement::Update` (three sites), `Kart::Player::ApplyPhysics`, `Kart::Sub::Update`.
- `Effects::Player::UpdateKartEffects` (12 sites), `ObjectsMgr::__ct/CreateAllObjects/CreateInstance`, `Objects::Itembox::OnCollision`.
- The item-window HUD.

Each needs a decision: Item Fever only, Retro Rewind only, or both chained. Some can be decided from what the site does. Others need Item Fever's handler disassembled, which is the time-consuming part.

**The rest** (menus, online, save, sound, most of the core): in an "Item Fever items" build Retro Rewind keeps these sites and Item Fever's patches are left out.

- That includes Item Fever's online lock-out: in this build the online buttons have to be shut by MKW100 itself.
- The open question is which of Item Fever's left-out callbacks its item code silently depends on, for example the boot code that creates its `System`, loads its settings and builds its icon bank.

## Can the two modules be loaded side by side?

| question | answer |
|---|---|
| Address ranges | Yes. 1.53 MiB of 2 MiB, Item Fever at a second base such as 0x818C0000. |
| Relocations | Yes. Kamek modules are relocatable (12,023 of Item Fever's 13,177 Kamek commands are module-relative), and `translate-mod` already relocates to any `module_guest_base`. |
| Static data, bss | Separate. Each module's statics and bss (each pack's `Pulsar::System::sInstance`, its settings, its item tables) sit in its own range. They don't overlap in memory, but they are two engines working on one game. |
| Static constructors | Both modules' constructor lists have to run (46 + 56). Pulsar registers its hook lists from them. |
| Translator | Not yet. `translate-mod` handles one module per product: one relocated image, one constructor range, one digest, one `module_link_base` remap in the code generator (`LinkedAddressRemap`), and one patch plan whose overlay functions get one module's patches. Two modules that patch the same base function must end up in **one** overlay function. This is the largest translator change. |
| MKW100's Item Fever fixes | Its 226 sites and `item_fever.cpp` addresses all move by the base difference (0xC0000 for 0x818C0000). That's mechanical. The shadow windows at 0x82D00000+ stay. |
| Pack updates | Both digests have to match, and the clash list has to be redone for a new build of either pack. |
| Retro WFC | Retro Rewind's chunk stays unmodified, so its server hash still matches. A Retro Rewind with different items online would be unfair, though, and would fall out of sync, so this build must be offline only. |

## Assets

- **Item models**: Item Fever replaces all of `Race/Common.szs` (vanilla content plus its models, `ItemSlot.bin` and CPU tables). Retro Rewind does not replace it and only adds `CommonAssets.szs` on top through Pulsar.
  - So the combined build can take Item Fever's `Common.szs` as the base file, with Retro Rewind's layer still on top.
  - The two packs' `ItemSlot.bin` formats (19 vs 43 items) would both exist. In this mode Item Fever's tables would decide the items, and Retro Rewind's item toggles and item modes would be off.
- **Sounds**: Item Fever replaces `revo_kart.brsar`, Retro Rewind does not, so Item Fever's can be used. Still to check: Retro Rewind's character voices against Item Fever's brsar.
- **Item icons**: Item Fever's code finds `MKWCI_xx` panes in the `PULitem` bank of its own UI archives. These are loaded through `ArchivesHolder::LoadArchives` 0x8052AA2C, which both packs patch. The combined build has to load Item Fever's `RaceAssets.szs` / `CommonAssets2.szs` as an extra archive next to Retro Rewind's (Pulsar's holders take more than one archive, not yet tried). Retro Rewind's own `Race.szs` item window has to show them.
- **BMG text**: item names in Item Fever's message files are only used by its menus, which we leave out. The HUD shows icons only.

All assets stay in the user's own Item Fever download. The build reads them from there, as the Item Fever build does today, and the repo ships none of them.

## Licence

Item Fever's `CREDITS.txt` keeps all rights to its own code, models, icons and sounds. It allows free personal use, but not redistribution or "reuse its contents in other projects" without written permission.

- **Options A and C** work like MKW100's current Item Fever support: the user's own copy is translated on their own machine, and the repo holds only addresses and glue code. Mixing it into Retro Rewind is still a further step from "playing Item Fever", so asking Kiwinner first is the decent thing to do, and it may also bring the source code (option D).
- **Option B** (porting Item Fever's code into MKW100's GPL-3 source) is not allowed without that permission.
- Retro Rewind's code (rr-pulsar) is GPL-3 and mkw-item-expansion is MIT. Both may be used.

## Options

### A. Item Fever as a second module, only its item hooks (recommended, staged)

Translate Item Fever's `Code.pul` as a second module at 0x818C0000 in the Retro Rewind product. Apply an allow-list of Item Fever's item patches, keep Retro Rewind's at clashing sites unless the list says otherwise, and chain the Pulsar dispatchers natively.

The work:

1. **Translator** (1-2 weeks):
   - A second Kamek chunk in `translate-mod`, with its own base, link-base remap, function discovery, constructor range, digest and image.
   - Overlay functions that merge both modules' patches.
   - A hook allow-list file like `array-sites.txt`.
2. **Sorting the hooks** (1-2 weeks):
   - A disassembler/call-graph pass from each Item Fever handler to sort its 1,154 patches.
   - Decide the ~150 clashing item sites, partly by reading Item Fever's handlers.
3. **Runtime** (about 1 week):
   - Chain the Pulsar dispatchers.
   - Create Item Fever's `System` and settings with defaults and no pages.
   - Mount Item Fever's files (`Config.pul`, `Common.szs`, brsar, UI archives) next to Retro Rewind's.
   - Shut online in this mode.
4. **MKW100 sites** (a few days): move Item Fever's 226 sites and `item_fever.cpp` to the new base, then run the 12/100/128-racer checks.

**Estimate:** 4-7 weeks. **Risk:** high.

- Hidden dependencies of the item code on left-out Item Fever code show up as crashes in code without symbols.
- Two engines may disagree on shared state (item object pools, `ObjectsMgr` size, lap rules).
- Every update of either pack means work again.

**How to choose it:** a separate product, for example `RetroRewind-ItemFever.exe` with its own launcher. "Optional" then means picking that launcher. A switch inside the normal Retro Rewind build would need a runtime gate at every Item Fever site.

### B. Port the item logic natively into MKW100

Rewrite the items as C++ in the runtime.

- Turning Item Fever's disassembly into C++ is a derivative of Item Fever's code, which the licence forbids without permission.
- A clean-room rewrite of 21 items from their behaviour alone (each is a new game object: model, physics, collision, HUD, sound, CPU use) plus the dropped-items system would take **months**.

Not recommended.

### C. A subset of items through the second module

Same setup as A, but the allow-list covers only some items, for example the new items without Item Rain, Loot Party or the four reworks.

- The reworks (Thunder Cloud, Golden Mushroom, Fake Item Box, Bob-omb) clash the most with Retro Rewind's own item code.
- **This is not a separate cheaper path.** Almost all the effort is the shared groundwork, which every new item needs: second module, dispatchers, item table expansion, icon bank, assets, settings. A subset saves perhaps 1-2 weeks of site decisions.

That is why A starts as a subset: one item for the go/no-go, then the new items, then possibly the reworks.

### D. Source-level merge (needs Item Fever's author)

With Item Fever's item source and permission, build one combined Pulsar `Code.pul` from rr-pulsar plus Item Fever's item files (the same engine API), using the Pulsar toolchain. MKW100 then treats it as a new Retro Rewind build: new digest, Retro Rewind's 62 sites measured again per `UPDATING.md`, Item Fever's per-racer tables measured in the new build.

- **The cleanest result**, and maintainable.
- It depends entirely on the author, plus a CodeWarrior/Kamek toolchain.
- **Estimate:** 1-3 weeks once the source is available, mostly merging the clashing hooks at source level, where they can be read.

### E. (alternative) Retro Rewind's tracks inside the Item Fever build

Both packs use Pulsar config version 3, and Item Fever's `Config.pul` holds no custom tracks. A merged pack folder could:

- Use Item Fever's `Code.pul` unchanged, which keeps its digest, so all of MKW100's Item Fever fixes keep working.
- Add Retro Rewind's track files and cups.

It is cheap to try (a 1-2 day test). It is **not what was asked**, though: Item Fever's menus, settings and save stay, and Retro Rewind's menus, characters and online are gone. Item Fever's code may also assume the 32 original tracks (trophies per cup, random track). Mentioned only as a fallback if A fails its test.

## Recommended next step

1. **User:** ask Kiwinner for permission to combine Item Fever's items with Retro Rewind in MKW100, and whether the item source can be shared. A yes with the source switches to option D.
2. **Go/no-go test for option A** (about 1 week, in this worktree):
   - Load Item Fever as a second module at 0x818C0000. A first version can link the second image by hand, without the full translator change.
   - Run both modules' constructors and chain the six Pulsar dispatchers.
   - Apply only Item Fever's item-table expansion plus one new item that mostly uses Item Fever-only code paths.
   - Success: a headless 12-racer Retro Rewind race in which CPUs receive and use that item without crashing, and Retro Rewind's 12-racer result otherwise unchanged when the item is off.
   - Failure (Item Fever's item code needs too much of its left-out boot/menu code): stop, and fall back to D or E.
3. Only after a pass: the full translator support, the new items, the 128-racer checks, then a menu play test by the user.

## Reproduce

```bash
python -I scripts/mkw100/pul_hooks.py --map projects/mkwii/MAP.txt \
  --manifest build/base/mkwii_base_manifest.json \
  IF=<ItemFever>/Binaries/Code.pul RR=<RetroRewind6>/Binaries/Code.pul --out <dir>
```

It prints the sizes and the table above. With `--out` it also writes every patch of each pack (`IF_hooks.txt`, `RR_hooks.txt`) and the functions both patch (`shared.txt`).
