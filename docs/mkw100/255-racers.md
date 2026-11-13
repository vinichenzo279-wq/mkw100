# 255 racers

Status 13.11.2026 (branch `racers-255`): the base game runs headless races with 255 racers: VS,
VS with the item exercise, Team VS, Grand Prix (two races and the results pages), balloon battle
(the full three minutes) and a diverse roster. Retro Rewind and Item Fever stay at 128 until
their own tables are checked.

255 is the end of the road. The racer count and racer ids are `u8` throughout the game (Racedata
+0x24, Raceinfo player ids and positions, item owners) and 0xFF means "none"; going further means
widening those everywhere.

## What assumed 128, and what it took

| Limit | Where | Change | State |
|---|---|---|---|
| Racer cap | `PlayerSlots::kMaxPlayers`, `ArrayOverflow::kMaxPlayers`, `kPositionsMaxRacers` | constant | 255 |
| Player slot windows | `player_slots.h`: 243 x 0x4000 = 0x3CC000 of the 0x600000 at 0x81A00000 | none | fits |
| Class shadow windows | `array_overflow.h`: 244 x window per 1 MiB class span; the largest window is 0x180 (input_manager), 0x16E00 in all | none (the static_assert holds) | fits |
| RaceBalloons shadow | `MKW100_ObjectArrayAdd`: 0x1000 per object, Vec3 x 243 = 0xB64 | none | fits |
| Effect group tables | `effect_groups.cpp`: 257 slots x 4 bytes did not fit the 0x400 before the activity table | constant (table at +0x800) | done |
| Place strings | `custom_messages.cpp`: 116 x 16 bytes sat between two other tables | moved to `kMenuText + 0x10000`; message index 0x7E00 + 255 stays below 0x7FFE | done |
| Signed racer bytes | game code: `extsb` on racer indices and places | translator site kind `sbyte`, 18 sites | done, see below |
| DriverMgr positions | game code: `u8 racePositions[]` at +0x161 indexed by racer in a 0x1F0-byte object: the ctor and Update write past it from 144 racers (200: the next heap block's header) | longer allocation (`driver_manager.cpp`) | done |
| Archives | the scene's MEM2 archive heap (46 MB) holds about 200 KB of kart archive per racer: 212 fit | overflow arena (`race_heap.cpp`) | done, see Memory |
| Battle ranking | our own `balloon_battle.cpp` read the position byte as `int8_t`: place 128 and up was skipped (128 racers already) | `int32_t` | done |
| Battle length | game code: `Raceinfo::__ct` reads the seconds from `minigame.kmg` by arena and player count, a row for 2..12 players | the 12-player length for more players (`balloon_battle.cpp`) | done, see Battle length |
| F10 slider, VS and battle rules row, `--race-players` | `LocalVs::MaxRacers()` | per product | base 255, mod product 128 |
| Position icons | `runtime/assets/rank_icons`: rank013..rank128.tpl | assets | 129..255 missing: the HUD uses the overlay digits |
| Item Fever | `item_fever.cpp` (`kFeverShadowRacers = 128 - 12`, shadows 0x100..0x200 apart), `cmpimm 128` guards in array-sites.txt | resize the shadows, guards to 255, retest | not done |
| Retro Rewind | `rr_racer_state` (class 10) fits; the per-racer skin archives (`rr_skins.cpp`) are budgeted for 128 and there is no arena | memory budget, retest | not done |
| Past 255 | `u8` count and ids everywhere | redesign | out of scope |

## Signed racer bytes

Up to 128 racers every racer index and place fits a signed byte. Past that, code that keeps one in
an `s8` and sign-extends it (`extsb`) gets a negative index. 134 racers crashed in
`Effects::Sub9d8::Update` (0x8067DE14), which keeps `s8 rank[racer]` and `s8 racer[rank]` tables.

The game has 2,064 `extsb` instructions, so the sites are found by running, not by reading:

1. Add a line `!trace-sbyte` to `projects/mkwii/mkw100/array-sites.txt` and build. Every `extsb`
   that is not an `sbyte` site then reports its result to `MKW100_TraceSignedByte`.
2. Run with `MKW100_TRACE_SBYTE=<file>` set. The file lists every site that produced -128..-2
   (address, lowest, highest, count) and is rewritten whenever a site is new, so a crash keeps it.
3. Compare a run of at most 128 racers with a run past 128. Sites only the second run lists are
   racer bytes. 12 and 128 racers list the same 24 sites (audio and library code).
4. Review each site, list it as `0xADDR 0 sbyte 0 0x0`, remove `!trace-sbyte`, rebuild.

An `sbyte` site calls `MKW100_RacerByte`: 0x80..0xFE stay positive, 0xFF is still -1 (the game's
"none", and `rank - 1` of rank 0).

Sites (traced at 200 and 255 racers: VS, items, Team VS, GP, battle, diverse roster, results pages):

- `Effects::Mgr`: 0x8067CEFC, 0x8067D620 (an `s8` racer counter used as a table index).
- `Effects::Sub9d8::Update` and its fix-up at 0x8067E0BC: all 15 `extsb` of the two functions
  (the trace saw three before the fix; the others read the same tables).
- 0x8071674C: `12 - position` from the Raceinfo position byte.

Listed by the trace past 128 racers but not racer bytes, left alone: 0x80040F4C and 0x8003F964
(main.dol library code next to sites every run lists), 0x801249F8 and 0x80124A68 (audio), and
with a scripted human 0x801AE5FC and 0x8052045C (stick values).

Limit of the method: a site is found only if a test runs it with a racer or place past 127. The
headless cases are CPU races, one scripted human, and the results pages. The menus, online,
replays, ghosts and split screen were not traced; a site missed there reads or writes before an
array (a wrong effect, or a crash).

## Memory

| Pool | Size | 255 racers |
|---|---|---|
| Player slot overflow (0x81A00000) | 6 MiB | 3.8 MiB |
| Class shadows (0x82000000, 15 spans) | 1 MiB each | 94 KiB in the largest |
| structsMem1 (MEM1) | 7 MB | about 85 KB per racer, overflows |
| Scene MEM2 heap: archives, then structsMem2 from what they leave | 46 MB | all archives; structsMem2 is left with almost nothing |
| Debug heap (MEM2 above 64 MB): arena + structsDebug | 62 MB = 40 MiB + 20 MB | takes both overflows |

How the game uses the 46 MB (from `GameScene::enter`, 0x8051AB58): it creates an archive heap
spanning the scene's whole MEM2 heap (scene +0xC84; +0xC80 for MEM1), loads the scene's archives,
shrinks the heap to fit and creates structsMem2 from the rest. So the archives and the scene's
MEM2 objects share those 46 MB, and at 255 racers the kart archives alone (about 53 MB) do not fit.

Two overflows therefore end in the debug heap:

- **Archives** that the archive heap cannot hold go to an arena of 40 MiB taken once from the end
  of the system's debug heap. It cannot be a scene's heap: `EGG::SceneManager::createScene` gives
  every scene new heaps and destroys them with it.
- **Scene objects**: structsMem1, then structsMem2, then structsDebug (20 MB with the arena),
  then the arena.

Measured peaks of the arena (the log prints `[race-heap] arena: N of 40 MiB in use`):
24 MiB in a 255-racer Grand Prix on course 0x08 with the default roster, 32 MiB with a diverse
roster on course 0x01 and in a 255-racer battle on Block Plaza, each on top of a full
structsDebug. That is about 52 to 54 MB of the 62,
so roughly 8 MB of headroom in the worst case measured; a larger course or pack content can
exceed it (`[race-heap] out of memory` in the log, usually followed by a crash).

What would buy real headroom: the default roster loads the same 13 kart archives up to 16 times
each. Sharing one copy per character and vehicle would free about 50 MB at 255 racers. Not done
(the game binds each racer's model to its own archive copy).

Things the arena has to get right, each found as a crash:

- Its blocks are zeroed. `GameScene` creates the struct heaps with the zero-clear flag and its
  objects rely on it.
- `MEMFindContainHeap` answers with the heap a block was asked from, since the game looks the
  result up among its own heaps (`EGG::Heap::free` without a heap).
- A new scene object is built at the old one's address and its heap fields hold the destroyed
  heaps until `GameScene::enter` stores the new ones; destroyed heaps are remembered.

## Battle length

`Raceinfo::__ct` (0x805327A0) reads a battle's length from `minigame.kmg`: `s16` seconds per
arena and player count, a row of 11 entries for 2..12 players (22 bytes per arena; balloon battle
at +0x10, coin runners at +0x1DC; the lookups are at 0x80532BCC and 0x80532C04). More players
index past the arena's row: the next arenas' rows first (40 racers on Block Plaza still got three
minutes), then other tables. There, 120 racers got 30 s, 128 got 20 s, 200 got 10 s, and 255
happened to get three minutes again. Main has this as well (seen at 120 and 128 racers).

More than 12 players now get the 12-player length: a hook after the constructor stores it in the
timer manager the way the constructor does (`Raceinfo_ct_805327a0` in `balloon_battle.cpp`).
`Raceinfo::GetBattleDuration` (0x805326EC) has the same lookup and is only called online.

Not fixed, the same row lookup: the object that `ObjectsMgr::CreateInstance` creates at 0x8087B3DC
reads four byte parameters per arena and player count in its init (0x8087B4D0; tables at +0x394,
+0x470, +0x54C and +0x628 of the file). Battles of more than 12 players get out-of-row values.

## Results

Headless, `build-logs/pr-integration/run-case.ps1` (30 minute timeout for battle: a 255-racer
battle takes about 12 minutes). No case logged `[race-heap] out of memory` or an unmapped guest
access. "Same before" compares with the executable that was in the play folder before this branch.

| Case | Result | Notes |
|---|---|---|
| `-Players 12` | `fa906b50b69477e8` | the baseline, unchanged |
| `-Players 24` | `924b7ee927786e20` | same before |
| `-Players 100` | `1f3f3b405973409f` | same before |
| `-Players 128` | `5576bba5f6e7ecec` | same before |
| `-Players 128 -Items` | `82a35cdb20a4e222` | same before |
| `-Players 128 -Diverse -Course 0x01` | `bc372e33fee410c5` | same before |
| `-Players 128 -Teams -FinishAt 1500 -Final -Results 60` | `ccd4a5746c111d7c` | same before; results pages survived |
| `-Players 200` | `8ea9cefaca6e9646` | arena 8 MiB |
| `-Players 255` | `66c77c1700c4cbee` | arena 24 MiB |
| `-Players 255 -Items` | `a971d5d0e5d1b82e` | arena 24 MiB |
| `-Players 255 -Teams` | `20a863a3280fa205` | arena 24 MiB |
| `-Players 255 -Gp` | `dd29fa54501fffba` | arena 24 MiB |
| `-Players 255 -Diverse -Course 0x01` | `80f79988a2054f1b` | arena 32 MiB |
| `-Players 255 -Auto -FinishAt 1500 -Results 60` | `3ea8b49d1e1aff6a` | results pages survived 60 s |
| `-Players 255 -Gp -Auto -FinishAt 1500 -Final -Results 90` | `aacd34b2ed9372f2` | results pages survived 90 s |
| `-Players 255 -Teams -FinishAt 1500 -Final -Results 60` | `dd7094538d4ddfa2` | results pages survived 60 s |

Battle (Block Plaza), with the battle length fix:

| Case | Result | Notes |
|---|---|---|
| `-Players 12 -Battle` | `b43215ff343ca601` | the game's own path |
| `-Players 40 -Battle` | `83c7644623a18375` | as in earlier runs on main |
| `-Players 40 -Battle -FinishAt 900 -Final -Results 60` | `68a9afa37ab3dab5` | as in earlier runs on main |
| `-Players 120 -Battle` | `728317b1d71abb09` | three minutes; before: over after 30 s (`f18e059d01e0735d`) |
| `-Players 128 -Battle` | `001c9682280d5d09` | three minutes; before: over after 20 s, ranking incomplete |
| `-Players 128 -Battle -FinishAt 1500 -Final -Results 60` | `2666bd09779d8f80` | results pages survived 60 s |
| `-Players 200 -Battle` | `a3bc071b83058535` | three minutes, arena 12 MiB |
| `-Players 255 -Battle` | `61dae2f9e336ba47` | three minutes, arena 32 MiB; same without the fix |
| `-Players 255 -Battle -FinishAt 1500 -Final -Results 60` | `380664ae768c22fe` | results pages survived 60 s (run before the fix) |

## Not done

- Position icons 129th..255th (`runtime/assets/rank_icons`); the overlay digits are used.
- Item Fever and Retro Rewind past 128 (see the table). Neither product was built or run from
  this branch.
- Anything a headless race does not reach: the menus with 255 (F10 slider, rules row, team
  screen, scrolling results), split screen, the award ceremony, online.
- Speed: 255 racers simulate well below real time (a 3-lap race on course 0x08 takes about six
  minutes headless).

Found on the way and not fixed here (none of them comes from this branch):

- Team VS and battle finals with the scripted human (`-Teams` or `-Battle` with
  `-Auto -Final -Results`) crash when the award results open (section 0x37 / 0x38):
  `Pages::AwardResults::PrepareResultItems` -> `AwardDemoResultItem::SetValues` ->
  `LayoutUIControl::SetTextBoxMessage` reads text as a pointer (0x756D6265). The executable from
  before this branch crashes the same way at 128 racers. Team VS crashes this way from 13
  racers (13, 40 and 128 tried; 12 racers pass). Battle: seen at 128 racers after the battle
  ended on its own; at 40 racers with a forced finish the game went on to section 0x4C and
  survived. CPU-driven finals pass at 40, 128 and 255 racers.
- Coin runners (`--race-battle=coin`) crashes at load with 13, 24 and 128 racers (12 works), on
  the executable from before this branch as well; with 24 racers in the code of the 0x8087B3DC
  object (lr 0x8087C488).
- The four per-player-count battle parameters (see Battle length).
