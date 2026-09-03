#!/usr/bin/env python3
"""Import the Xbox 360's own genre cards from a kiosk disc.

The Experience Discs carry mta-ge-*.png: 352x198 category cards, a game
screenshot with the genre's glyph in the corner. They are the only real Xbox 360
category artwork that survives anywhere I could find -- the marketplace archive
has none, and Microsoft's live catalogue serves none -- so the Game Marketplace
row is built from the genres these cover rather than from the catalogue's own
longer list, and every category on it wears Microsoft's art.

    python tools/import_genre_cards.py
    python tools/import_genre_cards.py --disc "A:/Disc/Experience Disc 1.6/..."
    python tools/import_genre_cards.py --list

Writes gamedir/images/marketplace/genre_<Genre>.png. fetch_marketplace.py picks
those up in preference to the cover montage it would otherwise build, and never
overwrites them -- --force rebuilds montages, not Microsoft's artwork.

Cropped, not letterboxed
------------------------
The cards are 16:9 and a slot card is 4:3, so something has to give. These fill
the tile and lose a little from each side, because letterboxing them left grey
bands across the row. The cost is that a glyph sitting hard against the right
edge -- Racing has one -- can be clipped.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

from fetch_marketplace import CARD_H, CARD_W, safe_name  # noqa: E402

# Genre on the row -> the disc's file for it.
#
# mta-ge-RPG and mta-ge-SportsAndRacing are second cards for genres already
# covered here, so they are not used. mta-ge-Strategy is deliberately absent:
# it is not artwork, it is an 11KB placeholder reading "Strategy Meta Image" in
# red on white, so Strategy falls back to a montage of its own covers.
CARDS = {
    "Action": "mta-ge-Action.png",
    "Action & Adventure": "mta-ge-ActionAdventure.png",
    "Adventure": "mta-ge-Adventure.png",
    "Racing": "mta-ge-Racing.png",
    "Role Playing": "mta-ge-RolePlaying.png",
    "Sports": "mta-ge-Sports.png",
    "Original Xbox Games": "mta-ge-OriginalXboxGames.png",
}

# Anything this small is the "<Genre> Meta Image" placeholder rather than a
# screenshot. The real cards are 118KB and up.
PLACEHOLDER_BYTES = 40000

DISC_GUESSES = [
    "A:/Disc/Experience Disc 1.6/Experience Disc Version 1.6 (Europe) (De,En,Es,Fr,It)/images",
    "A:/Disc/Experience Disc 3.2/Xbox 360 Experience v3.2/images",
    "A:/Disc/Experience Disc 4.1/Experience Disc Version 4.1 (USA)/images",
]


def find_disc(explicit):
    if explicit:
        p = Path(explicit)
        if not p.is_dir():
            raise SystemExit("not a directory: %s" % p)
        return p
    for guess in DISC_GUESSES:
        p = Path(guess)
        if p.is_dir() and any(p.glob("mta-ge-*.png")):
            return p
    raise SystemExit(
        "no kiosk disc found. Point --disc at an Experience Disc's images folder\n"
        "  (the one holding mta-ge-Action.png and friends).")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", default="", help="an Experience Disc's images folder")
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--list", action="store_true", help="show what the disc has and stop")
    args = ap.parse_args()

    disc = find_disc(args.disc)
    print("  disc: %s" % disc)

    if args.list:
        for f in sorted(disc.glob("mta-ge-*.png")):
            size = f.stat().st_size
            print("   %-34s %8d%s" % (f.name, size,
                                      "  (placeholder)" if size < PLACEHOLDER_BYTES else ""))
        return 0

    if not shutil.which("ffmpeg"):
        raise SystemExit("this needs ffmpeg to fit the cards to the tile")

    out = Path(args.gamedir) / "images" / "marketplace"
    out.mkdir(parents=True, exist_ok=True)

    wrote = 0
    for genre, filename in CARDS.items():
        src = disc / filename
        if not src.exists():
            print("   %-22s %s not on this disc" % (genre, filename))
            continue
        if src.stat().st_size < PLACEHOLDER_BYTES:
            print("   %-22s %s is a placeholder; skipped" % (genre, filename))
            continue

        dst = out / ("genre_%s.png" % safe_name(genre))
        vf = ("scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d,format=rgba"
              % (CARD_W, CARD_H, CARD_W, CARD_H))
        r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(src),
                            "-vf", vf, str(dst)], capture_output=True)
        if r.returncode == 0 and dst.exists():
            wrote += 1
            print("   %-22s <- %s" % (genre, filename))
        else:
            print("   %-22s failed: %s" % (genre, r.stderr.decode("utf-8", "replace")[:80]))

    print()
    print("  wrote %d genre card(s) to %s" % (wrote, out))
    print("  re-run fetch_marketplace.py to pick them up")
    return 0


if __name__ == "__main__":
    sys.exit(main())
