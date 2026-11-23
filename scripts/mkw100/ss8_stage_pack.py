#!/usr/bin/env python3
"""Stage MKWii 8P Splitscreen (SplitScreen8) as an MKW100 mod pack.

MKWii 8P Splitscreen 1.2.0 (github.com/therenow9/mkwii-8p-splitscreen, GPLv3) ships two builds of its
game module. MKW100 uses the "Mario Kart Wii" one: payload/mkw/Code.pul, rr-pulsar's SplitScreen8 module built
with SS8_VANILLA (no Retro Rewind), a plain Kamek module that MKW100's translate-mod reads like any pack's
Code.pul. Its Loader.pul is not needed: MKW100's mod loader places the module itself.

The character pages' per-player OK markers and the Same as Last Time hint need controls the game does not have.
They are derived from the player's own disc files at build time, as the release's setup does
(scripts/mkw100/ss8/gen_charselect_multi.py, gen_same_button.py), so nothing derived from game data is in the repo.

Output (the pack folder recomp.yml's splitscreen8 profile reads):
  <out>/Binaries/Code.pul
  <out>/UI/MenuMulti.szs, <out>/UI/MenuSingle.szs
  <out>/xml/SplitScreen8.xml

Usage:
  ss8_stage_pack.py --release <unzipped mkwii-8p-splitscreen-1.2.0> --disc-ui <disc files/Scene/UI> --out <pack>
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "ss8"))

import gen_charselect_multi  # noqa: E402
import gen_same_button  # noqa: E402

# The release MKW100 was measured on (payload/release.json's hash of mkw/Code.pul).
KNOWN_CODE_PUL = {
    "68c01d037e1d14f006f6a15878503589f91f900b069bc7ded67d7ee1bec75d7f": "MKWii 8P Splitscreen 1.2.0 (mkw/Code.pul)",
}

# The vanilla pack's Riivolution file, after the release's own (tools/install_8p.py VANILLA_XML) minus the Loader
# hooks: only the file mappings matter to MKW100.
XML = """<wiidisc version="1">
	<id game="RMC"/>
	<options>
		<section name="SplitScreen8">
			<option name="Pack">
				<choice name="Enabled">
					<patch id="SplitScreen8"/>
				</choice>
			</option>
		</section>
	</options>
	<patch id="SplitScreen8">
		<savegame clone="true" external="/riivolution/save/SplitScreen8/{$__gameid}{$__region}"/>
		<folder external="/SplitScreen8/Binaries" disc="/Binaries" create="true"/>
		<file external="/SplitScreen8/UI/MenuMulti.szs" disc="/Scene/UI/MenuMulti.szs"/>
		<file external="/SplitScreen8/UI/MenuSingle.szs" disc="/Scene/UI/MenuSingle.szs"/>
	</patch>
</wiidisc>
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_code_pul(release: Path) -> Path:
    for candidate in (release / "payload" / "mkw" / "Code.pul", release / "mkw" / "Code.pul", release / "Code.pul"):
        if candidate.is_file():
            return candidate
    raise SystemExit(f"no payload/mkw/Code.pul under {release} (the unzipped mkwii-8p-splitscreen-<version> folder)")


def write_if_changed(path: Path, data: bytes) -> None:
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--release", type=Path, required=True)
    ap.add_argument("--disc-ui", type=Path, required=True, help="the disc's files/Scene/UI folder")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)

    code = find_code_pul(args.release)
    digest = sha256(code)
    print(f"Code.pul {code} SHA256 {digest}")
    if digest not in KNOWN_CODE_PUL:
        print("WARNING: not the MKWii 8P Splitscreen build MKW100 was measured on (1.2.0); "
              "MKW100's fixes for it stay off and races keep 12 racers")
    else:
        print(f"  = {KNOWN_CODE_PUL[digest]}")
    write_if_changed(args.out / "Binaries" / "Code.pul", code.read_bytes())

    sources = {name: args.disc_ui / name for name in ("MenuMulti.szs", "MenuSingle.szs")}
    for name, path in sources.items():
        if not path.is_file():
            raise SystemExit(f"missing {path}")
    # Regenerate only when the inputs or the generators change.
    stamp_parts = [sha256(p) for p in sources.values()]
    stamp_parts += [sha256(HERE / "ss8" / n) for n in ("gen_charselect_multi.py", "gen_same_button.py")]
    stamp = ";".join(stamp_parts)
    stamp_file = args.out / "UI" / ".mkw100-stamp"
    outputs = [args.out / "UI" / n for n in sources]
    if stamp_file.is_file() and stamp_file.read_text().strip() == stamp and all(p.is_file() for p in outputs):
        print("menu archives up to date")
    else:
        (args.out / "UI").mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            # MenuMulti.szs: the per-player OK markers, then the hint; MenuSingle.szs: the hint (tools/install_8p.py SS8_CHAINS).
            if gen_charselect_multi.main([str(sources["MenuMulti.szs"]), str(t / "multi1.szs")]) != 0:
                raise SystemExit("gen_charselect_multi failed")
            if gen_same_button.main([str(t / "multi1.szs"), str(t / "MenuMulti.szs")]) != 0:
                raise SystemExit("gen_same_button failed on MenuMulti.szs")
            if gen_same_button.main([str(sources["MenuSingle.szs"]), str(t / "MenuSingle.szs")]) != 0:
                raise SystemExit("gen_same_button failed on MenuSingle.szs")
            for name in sources:
                shutil.copyfile(t / name, args.out / "UI" / name)
        stamp_file.write_text(stamp + "\n")
    write_if_changed(args.out / "xml" / "SplitScreen8.xml", XML.encode("utf-8"))
    print(f"pack staged in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
