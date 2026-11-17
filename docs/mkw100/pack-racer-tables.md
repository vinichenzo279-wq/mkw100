# Item Fever's effects past racer 12

Item Fever was written for 12 racers. Its item effects keep to racers 0..11 with checks like `cmplwi r3, 12; bgelr`, and their state lives in static tables of 12 elements inside its module. In a race of more than 12 racers, CPUs from slot 12 up were never frozen by the Ice Flower or turned into a Goomba by the wand, among other effects (user's play test, 80 racers, 2026-11-17). Item Fever 1.1.1's code has 558 compares with 12. MKW100 had widened 33 of them by hand (the special shells, the Item Rain racer flags) in `array-sites.txt`.

`scripts/mkw100/pack_racer_tables.py` does the rest systematically.

## What it does

1. **Tables.** Each function is walked in straight-line order. It tracks which registers hold a module address (a `lis/addi` pair with a relocation, plus `addi` offsets) and which hold a racer number. A racer number is a register the function compares with 12 against a less-than bound (equality tests are item IDs), a copy, mask or sign extension of one, or a parameter a caller passes a racer in. It also tracks scaled indexes (`rlwinm`/`mulli` of a racer number). A module address used as the base of an indexed load or store, or an `add`, with a scaled racer index is a table of 12 elements of that size. Some tables are left out (reported):
   - tables already shadowed by hand;
   - tables in the module's initialised data;
   - tables with several element sizes;
   - tables that don't end where the next variable starts. The boundaries are relocation targets, other tables' starts, and `base + constant` addresses (one source file's block of variables). A table followed by more memory may be longer than 12 elements, for example indexed by item ID, which has 43 values.
2. **Sites.** Every indexed access to a table, in any function, becomes an `index` site (table in rA), an `indexb` site (table in rB) or an `indexadd` site (an `add`). These are new translator site kinds. The runtime helpers only act when the base is exactly the table and the offset reaches element 12, so for racers 0..11 nothing changes. Shadows for elements 12..254 sit at 0x82E00000 (array shadow class 15).
3. **Checks.** A compare with 12 becomes a `cmpimm 255` site only when all of these hold:
   - its racer number directly indexes a shadowed table or goes to a game function that takes any racer (`Kart::Manager::GetKartPlayer`);
   - it reaches no unshadowed table;
   - it isn't added to a base the scanner cannot name;
   - it isn't used by an access form the sites don't rewrite (update forms such as `stwux`);
   - every Item Fever function it is passed to is safe by the same rules, or keeps its own check;
   - it isn't the counter of a loop that also steps a pointer through a table (that loop would walk past the table).
4. **Runtime** (`item_fever.cpp`, table list `item_fever_racer_tables.inc`). On a race's first frame each shadow gets the value that all 12 elements share (a reset value), or zeroes. `MKW100_TEST_RACER_TABLES=1` logs which tables racers 12+ wrote to. `MKW100_TEST_GIVE_ITEM=<id>` hands every CPU an item every 5 s.

Regenerate after a pack update:

    python -I scripts/mkw100/pack_racer_tables.py <ItemFever>/Binaries/Code.pul --write-sites \
        --write-tables runtime/src/hle/item_fever_racer_tables.inc --report build-logs/racer-tables/report.txt

`--guards N:M` keeps a slice of the raised checks, for bisecting a crash.

## Mistakes the rules came from

- **Item slot table, 12 position rows:** a "< 12" check alone is no proof of a racer number. Raising it stopped the boot. Now a check needs a direct table use or a racer function.
- **Loop compared a masked copy of its counter:** the scanner didn't link the copy to the counter, so it raised the check without seeing an unshadowed byte table in the loop. Racers 12+ then corrupted the variables after that table. Copies and masks now count as the same racer number.
- **`add` with r0 as the table register:** this was skipped like `rA = 0` of a load, which reads as zero. For `add`, r0 is a real register.

## Status (Item Fever 1.1.1, IF100)

236 tables shadowed, 917 access sites, 143 checks raised, 395 kept at 12. The kept ones by reason:

| Count | Reason |
|---|---|
| 158 | no racer use found (counts, other kinds of index) |
| 57 | index tables not shadowed (left-out tables) |
| 48 | only passed on to other functions |
| 32 | a callee isn't safe |
| 31 | racer offset on a base the scanner cannot name (mostly game arrays such as `Item::Manager` players, which MKW100 sizes for every racer) |
| 22 | pointer-stepping loops |

Racers 12+ now write Item Fever's tables, including the Ice Flower's (0x818CA06C) and the penguin's. Which effects still stop at 12 has to be seen in play.

Headless results (2026-11-17, `build-logs/iftest`):

- 12 racers: `19f921d8b3158f48`, Item Fever's baseline, unchanged.
- Every item from 0x16 to 0x29, handed to every CPU at 24 racers: each race finishes, and racers 12+ write 4 to 20 of the tables. 0x15 is never handed out.
- 0x2A to 0x2E crash in the game's own `Item::Player::Update` at 12 racers too. They are not Item Fever items to hand out.
- 100 racers with the Ice Flower or the penguin suit for every CPU, and 128 racers with MKW100's item test: each race finishes.
