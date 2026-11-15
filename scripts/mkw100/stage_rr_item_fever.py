#!/usr/bin/env python3
"""Put the Item Fever files the Retro Rewind + Item Fever items build needs into a stage folder.

    python -I scripts/mkw100/stage_rr_item_fever.py --stage play-rrif \
        --rr-pack <RetroRewind6 folder> --if-pack <ItemFever pack folder (Binaries, Assets, Disc)>

Writes <stage>/ItemFeverItems/ (docs/mkw100/rr-item-fever.md), a Riivolution root for the stage's
UserData/Config.toml:  [paths] overlay_roots = ["<stage>/ItemFeverItems"]

  riivolution/ItemFeverItems.xml  maps the files below over the disc
  files/IFCnfg.pul                Item Fever's Config.pul (combine_pul.py renames the name it reads)
  files/Common.szs                Item Fever's Race/Common.szs + Assets/CommonAssets2.szs (item models)
  files/RaceAssets.szs            Retro Rewind's RaceAssets.szs + Item Fever's + its mkwci icons
  files/revo_kart.brsar           Item Fever's sound archive (item sounds)

Everything comes from the user's own copies of both packs; nothing here is shipped.
"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from merge_szs import u8_files, u8_build, yaz0_decode, yaz0_encode  # noqa: E402

XML = """<?xml version="1.0" encoding="UTF-8"?>
<!-- MKW100: Item Fever files for the Retro Rewind + Item Fever items build (docs/mkw100/rr-item-fever.md).
     Made by scripts/mkw100/stage_rr_item_fever.py from the user's own copies of both packs. -->
<wiidisc version="1">
  <id game="RMC">
    <region type="P"/>
  </id>
  <options>
    <section name="Item Fever items">
      <option name="Items" id="IFItems" default="1">
        <choice name="On">
          <patch id="IFItems"/>
        </choice>
      </option>
    </section>
  </options>
  <patch id="IFItems">
    <file disc="/Binaries/IFCnfg.pul" external="/files/IFCnfg.pul" create="true"/>
    <file disc="/Race/Common.szs" external="/files/Common.szs"/>
    <file disc="/RaceAssets.szs" external="/files/RaceAssets.szs" create="true"/>
    <file disc="/sound/revo_kart.brsar" external="/files/revo_kart.brsar"/>
  </patch>
</wiidisc>
"""


def merge(out, parts):
    merged = {}
    for path, keep in parts:
        files = u8_files(yaz0_decode(open(path, 'rb').read()))
        merged.update({k: v for k, v in files.items() if keep(k)})
    with open(out, 'wb') as f:
        f.write(yaz0_encode(u8_build(merged)))
    print(f'{out}: {len(merged)} files')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stage', required=True)
    ap.add_argument('--rr-pack', required=True)
    ap.add_argument('--if-pack', required=True)
    a = ap.parse_args()
    root = os.path.join(a.stage, 'ItemFeverItems')
    files = os.path.join(root, 'files')
    os.makedirs(files, exist_ok=True)
    os.makedirs(os.path.join(root, 'riivolution'), exist_ok=True)
    ifp = a.if_pack
    shutil.copyfile(os.path.join(ifp, 'Binaries', 'Config.pul'), os.path.join(files, 'IFCnfg.pul'))
    shutil.copyfile(os.path.join(ifp, 'Disc', 'sound', 'revo_kart.brsar'), os.path.join(files, 'revo_kart.brsar'))
    everything = lambda _k: True  # noqa: E731
    merge(os.path.join(files, 'Common.szs'), [
        (os.path.join(ifp, 'Disc', 'Race', 'Common.szs'), everything),
        (os.path.join(ifp, 'Assets', 'CommonAssets2.szs'), everything)])
    merge(os.path.join(files, 'RaceAssets.szs'), [
        (os.path.join(a.rr_pack, 'Assets', 'RaceAssets.szs'), everything),
        (os.path.join(ifp, 'Assets', 'RaceAssets.szs'), everything),
        (os.path.join(ifp, 'Disc', 'Scene', 'UI', 'Race.szs'), lambda k: k.startswith('game_image/timg/mkwci'))])
    with open(os.path.join(root, 'riivolution', 'ItemFeverItems.xml'), 'w', encoding='utf-8') as f:
        f.write(XML)
    print(f'{root}: ready; add it to [paths] overlay_roots in the stage\'s UserData/Config.toml')


if __name__ == '__main__':
    main()
