#!/usr/bin/env python3
"""Write gamedir/gamerpics.txt: the gamer pictures you can choose from.

Change Gamer Picture is a Guide screen on a console -- XAM's own, which this port
has no UI layer to draw, and no .xur for it ships in any dashboard package. So
the chooser is built the same way the other pages here are, out of the pictures
themselves.

The dumped dashboard's shared resources carry the default set, named by the key
the tile API uses:

    64_fffe07d1 0002000000010000 .png
    ^^ size     ^^^^^^^^ title   ^^^^^^^^^^^^^^^^ image id

    python tools/import_gamerpics.py --from "A:/Xbox 360 Dumped Dash/Metro/Shrdes"
    python tools/import_gamerpics.py --from ... --dry-run

Selecting one writes it into the signed-in profile's package as tile_64.png and
tile_32.png, which is where the gamercard reads its picture from -- so the choice
takes effect the same way it would on hardware.

Everything written is a plain text file plus PNGs. The dashboard reads them off
disk and never touches the network.
"""
import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

from fetch_marketplace import ascii_safe, fit_to_card  # noqa: E402

# 64_fffe07d10002000000010000.png -> ('64', 'fffe07d1', '0002000000010000')
NAME = re.compile(r"^(\d+)_([0-9a-f]{8})([0-9a-f]{16})\.png$", re.I)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src", required=True,
                    help="folder of the dumped shared resources")
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--force", action="store_true", help="rebuild tiles already present")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    src = Path(args.src)
    if not src.is_dir():
        raise SystemExit("no such folder: %s" % src)

    # Prefer the 64px copy of each picture; fall back to the 32px one.
    found = {}
    for path in sorted(src.iterdir()):
        m = NAME.match(path.name)
        if not m:
            continue
        size, title, image_id = int(m.group(1)), m.group(2).lower(), m.group(3).lower()
        key = (title, image_id)
        if key not in found or size > found[key][0]:
            found[key] = (size, path)

    if not found:
        raise SystemExit("no gamer pictures in %s (expected names like "
                         "64_fffe07d10002000000010000.png)" % src)
    print("  %d gamer picture(s)" % len(found))

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "gamerpics"
    if not args.dry_run:
        imgdir.mkdir(parents=True, exist_ok=True)

    rows = []
    for index, ((title, image_id), (size, path)) in enumerate(sorted(found.items()), start=1):
        name = "%s%s.png" % (title, image_id)
        dest = imgdir / name
        if not args.dry_run and (args.force or not dest.exists()):
            tile, _ = fit_to_card(path.read_bytes())
            dest.write_bytes(tile)
        # `extra` is the source file, which the dashboard copies into the profile
        # package when the tile is chosen.
        rows.append(("Gamer Picture %d" % index, "Select to use this picture",
                     "images/gamerpics/" + name, path.name, "gamerpic"))
        print("    %-18s %dpx  %s" % ("Gamer Picture %d" % index, size,
                                      ascii_safe(path.name)))

    card = "images/gamerpics/" + sorted(found.items())[0][1][1].stem
    card = rows[0][2]  # the first picture stands for the set

    out = [
        "# Written by tools/import_gamerpics.py -- edit freely, it is only a text file.",
        "# The gamer pictures Change Gamer Picture offers.",
        "# name | subtitle | image | source file | kind ('category' marks the heading,",
        "#   'gamerpic' marks a tile that sets the picture when chosen)",
        "",
        "Gamer Picture|%d to choose from|%s||category" % (len(rows), card),
    ]
    out += ["|".join(str(x).replace("|", "/") for x in r) for r in rows]
    text = "\n".join(out) + "\n"

    if args.dry_run:
        print()
        print(ascii_safe(text))
        return 0

    (gamedir / "gamerpics.txt").write_text(text, encoding="utf-8")
    # The originals, kept at their real size for writing into the profile.
    raw = gamedir / "gamerpics_src"
    raw.mkdir(parents=True, exist_ok=True)
    for path in sorted(src.iterdir()):
        if NAME.match(path.name):
            target = raw / path.name
            if args.force or not target.exists():
                target.write_bytes(path.read_bytes())

    print()
    print("  wrote %s" % (gamedir / "gamerpics.txt"))
    print("  originals in %s" % raw)
    return 0


if __name__ == "__main__":
    sys.exit(main())
