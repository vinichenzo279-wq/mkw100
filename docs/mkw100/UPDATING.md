# Updating MKW100

Two kinds of updates come from outside: a new **WiiCompiled** release, and a new build of a **pack** (Retro Rewind, Item Fever). This page says what each one touches and how to take it.

## A new WiiCompiled release

MKW100 is a fork of [patchzyy/Wiicompiled](https://github.com/patchzyy/Wiicompiled). Its git history records upstream 0.9.3 (`f02d000`) as its base and 0.9.4 (`ffbde83`) as merged, so a new release is an ordinary merge:

```bash
git remote add upstream https://github.com/patchzyy/Wiicompiled.git   # once
git fetch upstream
git checkout -b wiicompiled-<version> origin/main
git merge upstream/main
```

Resolve the conflicts, then follow the checklist below. Places where conflicts are likely:

| Area | Why |
|---|---|
| `README.md` | Ours is MKW100's. Keep ours and copy upstream's into `README-UPSTREAM.md`. |
| `.gitignore` | Keep both sides' lines. |
| `runtime/src/hle/gx/*`, `runtime/src/hle/input/pad.cpp`, `runtime/src/main.cpp` | Both sides change them (GX speed-ups, `--race-auto`, headless). Keep both changes; an MKW100 line usually has a comment saying why. |
| `translator/src/Translator.Core/CodeGen/*` | Upstream's code generator; MKW100's translator changes are in `Lifting/` (site rewrites) and `Analysis/GuestHelperEffectCatalog.cs`. A conflict here needs a careful read. |
| `aurora-main` (submodule) | Take upstream's commit. MKW100's aurora changes are patches in `patches/aurora/`, applied by the build to a private copy; check they still apply: `git -C aurora-main apply --check ../patches/aurora/*.patch`. |

Checklist after the merge (headless, `build-logs/pr-integration/run-case.ps1`; results only compare within one play folder, since `UserData/Config.toml` affects races past 12 racers):

1. Base build passes.
2. 12 racers still gives `fa906b50b69477e8`. A different result here means a change in the game's behaviour (upstream fixed something that changes races, or a merge mistake); find out which before going on.
3. 128 and 255 racers, a 40-racer battle and a 40-racer Team VS finish, with the same results as the build before the merge where nothing in upstream's changes should affect them.
4. Retro Rewind and Item Fever builds pass, and 12 and 100 racers finish (`run-rr.ps1`, `run-if.ps1`).
5. A short play test from the menus (character select, a race, results, the next race).

The 0.9.4 merge (2026-11-14) changed none of these results.

## A new build of a pack (Retro Rewind, Item Fever)

MKW100's pack fixes hold addresses inside one build of each pack's `Code.pul`: the `profile=` sites in `projects/mkwii/mkw100/array-sites.txt` and `player-slot-sites.txt`, and the hooks in `runtime/src/hle/rr_skins.cpp` and `item_fever.cpp`. A new pack build moves its code, so those addresses point at other instructions.

What happens on its own:

- **The build does not fail.** `translate-mod` prints the Kamek code SHA-1 of the pack. If the profile is listed in `projects/mkwii/mkw100/mod-builds.txt` and the digest is not, it leaves that profile's sites out and prints a warning.
- **The game does not crash.** The runtime (`runtime/src/hle/mod_build.cpp`) recognises the same digests. With an unknown one it turns the pack hooks off and caps that pack at **12 racers** (`LocalVs::MaxRacers`), so races run like the unmodified pack. The log says `mod-build: WARNING: this Code.pul is not a build ...`.

To bring big races back for the new build:

1. Note the new digest (the build log line `[mkw100] retro-rewind Kamek code SHA-1 ...`).
2. For each `profile=` site and each address in `rr_skins.cpp` / `item_fever.cpp`, find the same instruction in the new `Code.pul`. Most functions only move; matching the surrounding instructions (with branch offsets and `lis`/`addi` address halves masked) finds them. Re-check by hand any site whose surroundings changed.
3. Update the addresses, then put the new digest in **both** `mod-builds.txt` and `mod_build.cpp` (keep the old line only if both builds are still supported, which needs both address sets, so normally replace it).
4. Run the pack's 12- and 100-racer checks and a menu play test.

### Retro Rewind with Item Fever's items

The experimental combined build (`-ItemFeverItems`, docs/mkw100/rr-item-fever.md) depends on both packs' builds at once:

- `scripts/mkw100/combine_pul.py` stops when either `Code.pul` is not a build listed in `mod-builds.txt`, so a pack update first goes through the steps above for that pack.
- Its plan, `projects/mkwii/mkw100/rr-item-fever.txt`, holds addresses inside both packs' modules (kept patches, Retro Rewind's code check, `chain` sites). Those need the same remeasuring.
- Its site lists come from the two packs' `profile=` sites through `projects/mkwii/mkw100/profile-merges.txt`, so they follow automatically.
- The runtime recognises it by the marker `combine_pul.py` writes (both source digests), checked against the digests in `mod_build.cpp`.
