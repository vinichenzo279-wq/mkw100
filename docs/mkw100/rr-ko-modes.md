# Retro Rewind KO, Lap KO and KO Royale past 12 racers

Retro Rewind's three knockout modes (rr-pulsar `PulsarEngine/Gamemodes/KO`, `LapKO`,
`BattleRoyale`, GPL-3) keep their racers in 12-entry tables. Offline past 12 racers MKW100 runs
their per-racer parts natively for every racer (`runtime/src/hle/rr_ko_modes.cpp`); up to 12
racers, and online, Retro Rewind's own code runs unchanged. Known Retro Rewind build only
(`hle/mod_build.h`).

## What broke past 12

| Mode | Retro Rewind code | Past 12 |
|---|---|---|
| Lap KO | `LapKO::Mgr`: `active[12]`, `crossed[12]`, `crossOrder[12]`, `lastLapValue[12]`; a round ends once `activeCount - knockouts` racers crossed the line | lap crossings of racers 12+ never counted: nobody was knocked out (24 racers, 7 minutes) |
| KO | `KO::Mgr::CalcWouldBeKnockedOut` (every race frame) ranks into a `PlayerPosition[12]` on the stack; `status[12][2]` by player id offline; `ProcessKOs` runs inside `GPVSLeaderboardTotal::FillRows` | stack and object overrun every frame (the reported crash); `FillRows` is `LocalVs_TotalRows` past 12, so nobody was ever knocked out |
| KO Royale | `BattleRoyale.cpp` on `LapKO::Mgr`: `GetBalloonCount(id >= 12) = 0`, hits on 12+ remove nothing, knockouts and "last racer in" walk racers 0-11 | only racers 0-11 could go out (24 racers: only racer 0) |
| every mode | `EjectItemsFromItemDamage` writes `sLastEjectFrame[id]` (u16[12] at 0x81879DC4) on every item hit, before its KO Royale check | racers 12+ wrote over the module's next statics in every Retro Rewind race |

## What runs instead (offline, more than 12 racers)

Each hook keeps the managers' own fields current (`activeCount`, `roundIndex`, `totalRounds`,
`raceFinished`, `eliminationPlan`, the 12 table entries, `winnerPlayerId`, `offlineRaceNumber`), so
the rest of Retro Rewind (HUD, item rules, the elimination banner, and the 12-racer code once a KO
series is down to 12) reads what it expects.

| Address | Retro Rewind function | Past 12 |
|---|---|---|
| 0x8180FABC | `LapKO::Mgr::InitForRace` | after it: every racer active, the knockout plan |
| 0x81810610 | `LapKO::Mgr::UpdateFrame` | race init, lap crossings, rounds, offline finish for every racer |
| 0x8180FF90 | `LapKO::Mgr::ProcessElimination` | native (also used by KO Royale) |
| 0x8180D710 | `KO::Mgr::Update` | danger marks without the stack table |
| 0x8180C794 | `KO::Mgr::PrepareOfflineVSNextRace` | drops the knocked-out racers from every menu slot and from `local_vs.cpp`'s series roster (`LocalVs::KoKeep`) |
| (LocalVs_TotalRows) | `KO::Mgr::ProcessKOs` | `RrKoModes::OnSeriesStandings`, from the series standings |
| 0x8180846C | `BattleRoyale AddBalloons` | nothing: `balloon_battle.cpp` hands out the starting balloons (Royale's count) |
| 0x81809B8C | `ProcessBalloonEliminations` | every racer |
| 0x81809D44 | `FinishSoleActiveUnfinishedPlayer` | every racer |
| 0x8180936C, 0x81809490, 0x81809688 | `OnRemoveHit`, `OnMoveHit`, `FinishPoweredHitAction` | native when a racer 12+ is involved |
| 0x8180AD60, 0x8180AD70 | `sLastEjectFrame` read/write | array-sites.txt shadow at 0x82D80000 for racers 12+ |

## Scaling

- **Lap KO**: the race length stays the one Retro Rewind picks from its 12-racer plan, and so does
  the number of racers on the final lap. The other knockouts are spread over the laps before it:
  24 racers, 4 KO per race, 3-lap course: 3 laps, 10 + 10 out, 4 on the last lap; 100 racers: 48 +
  48 out, 4 on the last lap; 1 KO per race: 11 laps, 2 on the last lap.
- **KO**: each KO race knocks out the same share of the field as at 12: KOs per race x racers / 12,
  rounded up (100 racers, 4 per race: 34, 22, 15, 10, 7, then Retro Rewind's own count at 12).
- **KO Royale**: no scaling; every racer starts with the balloon setting's count.

## Not carried over past 12

- Item odds are not re-weighted for the racers left: Retro Rewind does it by running
  `Item::SlotData::SetupVSProbabilities` again, which allocates new tables from the race heap; past
  12 that heap is full and the game panicked (`eggExpHeap.cpp:174`). The odds stay those of the
  starting field.
- The elimination banner names racers 0-11 only (Retro Rewind's display keeps 12 names).
- KO Royale: hits between two of racers 0-11 keep Retro Rewind's own star/bullet/mega and
  mushroom-steal tables; team checks are Retro Rewind's for racers 0-11 only (Royale offline is a
  free-for-all).

## Tests

`scripts/mkw100/run-rr-ko.ps1 -Name <new> -Players N ...` (stage `<repo>/play-rr-ko` or `-Stage`;
evidence in `build-logs/ko/`), headless:

- `-RRSettings '45=2'` Lap KO, `'45=1'` KO, `'52=1'` KO Royale; `48=n` KOs per race (n+1),
  `49=n` races per KO, `53=n` Royale balloons.
- `-SpareLocal` (`MKW100_TEST_KO_SPARE_LOCAL`): the local racer (racer 0, CPU driven) is never
  knocked out, so a race or series runs on to a winner instead of ending when racer 0 is out.
- `-Series N -Auto` (`MKW100_TEST_SERIES`, `--race-auto`): the race oracle reports each race and
  stays for the results pages; the game's "next race" section (0x4B, the menus) is replaced by the
  boot race's section, so the next race runs on the same course with the series' scenario. The
  run ends when the KO series is decided, when the game leaves for any other section (series
  over), or after N races.
