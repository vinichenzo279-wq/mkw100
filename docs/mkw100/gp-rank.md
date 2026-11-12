# How Mario Kart Wii works out your Grand Prix rank

This is the GP rank (★★★ down to E) shown on the results after a cup. It is decoded from the PAL
game's own code (RMCP01). The function names come from the community symbol map, and the field
names from the Pulsar `GameSource` headers.

The rank is made of two scores, a **driving score** and your **cup points**. Each one is turned
into a number from 0 to 5, the two numbers are added, and the sum picks the rank.

## 1. The driving score: what each race adds

After every GP race, `RaceinfoPlayer::ComputeGPRank` (`0x805368F8`) works out one number per
racer. `RacedataScenario::UpdatePoints` calls it at the end of each race, for human racers only.
The number is clamped to **−50 … +250**. It is then added to a running total: the menu scenario
player's `+0xDE`, a signed 16-bit value, which is zeroed when a cup starts. Four races of at most
250 give at most 1000.

One race's score is the sum of these terms:

| Term | Points | Source |
|---|---|---|
| Finishing time against the course's reference time | `+ trunc(1000 × (ref − raceFrames) / ref)` | `RaceinfoPlayer +0x2C` (frames raced) |
| Time spent in 1st place | `+ trunc(150 × framesIn1st / ref)` | `RaceinfoPlayer +0x30` |
| Successful start boost (rocket start) | `+25` | `GpStats +0x0` (bool) |
| Each mini-turbo | `+2` each | `GpStats +0x4` (`mtCount`) |
| Each item hit on another racer | `+5` each | `Tallies +0x8` (`itemsHitCount`) |
| Off-road driving | `− offroadFrames / 6` (−10 a second at 60 fps) | `GpStats +0x8` |
| Unknown counter (likely being hit) | `−20` each | `GpStats +0xC` |
| Each collision with an object | `−30` each | `GpStats +0x10` (`objectCollisionCount`) |
| Each fall out of bounds (Lakitu) | `−70` each | `GpStats +0x14` (`oobCount`) |
| Unknown bonus counter | `+` its value | `GpStats +0x18` (u16) |
| Playing with the Wii Wheel | `+10` | the controller's type is 0 (Wheel) |
| A controller setting (flag at `+0x51` of the controller object) | `+25` | likely manual drift; not confirmed |

How the terms are read:

- **`ref`** is a reference time in frames for the course and class. It comes from a game data table
  at offset `+12 + course × 8 + class × 2` (16-bit entries). Finishing faster than `ref` gives
  points; finishing slower takes them away. Finishing 10% faster than the reference is about
  +100.
- **The kart counters** (`GpStats`, `Tallies`) belong to each kart: `Kart::Values +0x34` and
  `+0x38`. They count up during the race.
- **The controller terms** only apply to human racers. CPUs have no controller.
- **Without the reference table**, the result is rank 7 (E). The table is missing outside GP, or
  when the game's mode flags are set.

## 2. Turning the two scores into a rank

`RacedataPlayer::ComputeGPRank` (`0x8052DAF0`) does this:

- **Driving score** (the 4-race total): 1000 or more → 0, 700+ → 1, 400+ → 2, 200+ → 3,
  0+ → 4, below 0 → 5. Thresholds: table at `0x808900C0`.
- **Cup points** (`+0xDA`, made for 12 racers, so 4 wins = 4 × 15 = 60): 60 or more → 0,
  52+ → 1, 40+ → 2, 20+ → 3, below 20 → 4. Thresholds: table at `0x808900D8`.

**Rank = driving + points**, capped at 7.

| Sum | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7+ |
|---|---|---|---|---|---|---|---|---|
| Rank | ★★★ | ★★ | ★ | A | B | C | D | E |

So **★★★ needs all 60 points and at least 1000 driving score**, which is an average of 250, the
per-race maximum, in every race. Winning every race is not enough on its own: you also have to
drive clean and fast.

## In MKW100

- **Cup points:** with more than 12 racers, our scoring gives much bigger totals. For the rank,
  your points are scaled into the game's range: your total ÷ (1st place's points × 4 races) × 60.
  The thresholds then mean the same as with 12 racers.
- **Driving score:** our points code replaces the game's `UpdatePoints` past 12 racers. It also
  calls `RaceinfoPlayer::ComputeGPRank`, the same as the game, once per race for human racers.
- **"Finish race now":** the debug button skips almost all driving, so expect a low driving score
  (rank B with all wins).
