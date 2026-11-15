# Position icons 13th–255th

`rank013.tpl` … `rank255.tpl` are the race HUD's position counter past 12th
(`runtime/src/hle/rank_icons.cpp`). The build copies this folder next to the executable as
`rank_icons/`; without it the HUD falls back to the overlay's vector digits.

They have the size and format of the game's own 1st–12th (`Race_E.szs`,
`tt_position_no_st_64x64_NN.tpl`): 180×128, RGB5A3, one image. The digits fill the height as the
stock ones do; three-digit places are narrowed rather than shortened. 13th–24th are green,
25th–255th blue.

`generate.py` renders them (Python 3 with Pillow; font: DejaVu Sans Condensed Bold Oblique, not
included, placed next to the script):

```
python generate.py --out Out180     # then copy Out180/TPL/*.tpl here
```

History: until 2026-11-15 these were 64×64 RGBA8 (13th–24th by hammerbro101, "Wii - Mario Kart
Wii - Positions (24 Players)", 25th–255th from the MKW100 Rank128 pack). The HUD stretched them
into the stock 180×128 pane, which made them look short and soft next to 1st–12th.

**1st–12th** are the game's own textures and are not included.
