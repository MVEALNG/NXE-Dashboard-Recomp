#!/usr/bin/env python3
"""Fill assets/covers and assets/icons from your Xbox LIVE play history.

The README asks you to supply cover art and icons yourself, because an extracted
package carries no artwork and a game's icon normally lives in a GPD written the
first time that title is played -- which has never happened here. titlehub knows
what you have played, though, and its title ids are the same ones the dashboard
uses: 1297287142 is 0x4D5307E6 is Halo 3.

    python tools/fetch_covers.py                 # only what is missing
    python tools/fetch_covers.py --force         # re-fetch everything
    python tools/fetch_covers.py --dry-run       # list, download nothing

Two kinds of picture come back, and they do not match
-----------------------------------------------------
The dashboard wants portrait art -- the hand-made Halo 3 cover here is 267x400
with a 64x96 thumbnail -- and what a title gives you depends on which host it
still lives on:

  store-images.s-microsoft.com   1080x1080 SQUARE. The store replaced box art
                                 with square tiles years ago and titlehub's
                                 "BoxArt" entry is square too. 30 of 47 titles.
  images-eds.xboxlive.com        85x120 portrait, the genuine 360-era art, but
                                 small and it refuses to be scaled up. 17 of 47.

So a shelf built from this is a mix of correct-but-small and sharp-but-square.
Neither host will letterbox: every mode=Padding/Letterbox/Crop is a 400 or 404.
Squaring the circle means padding them locally, which needs an image library
(Pillow) -- a dependency worth adding deliberately rather than by surprise, so
it is not done here.

Anything already in assets/ is left alone; --force is the only way to overwrite,
because hand-made art is almost certainly better than either of these.
"""
import argparse
import sys
from pathlib import Path

import xbl_auth

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

TITLEHUB = ("https://titlehub.xboxlive.com/users/xuid(%s)/titles/titlehistory"
            "/decoration/scid,image,achievement")

# The thumbnail goes in the content metadata's thumbnail field, which caps at
# 15,616 bytes; 64x64 lands around 12K. The full cover has no such limit but
# there is no point pulling 1080x1080 for a tile a few hundred pixels wide.
COVER_QS = "w=267&h=267"
THUMB_QS = "w=64&h=64"
THUMB_LIMIT = 15616


def fetch(url: str, *sizes: str) -> bytes:
    """First of `sizes` the image host will actually serve, else the raw image.

    Two hosts appear in a play history and they disagree about resizing:

      store-images.s-microsoft.com   modern store art, 1080x1080 square,
                                     resizes to anything asked for
      images-eds.xboxlive.com        the old EDS service, 85x120 portrait --
                                     real 360-era box art -- which will scale
                                     down but answers 400 to any request larger
                                     than the source

    So ask for the size wanted, and take the original when the host refuses.
    The EDS pictures are small, but their shape is right where the store's
    square tiles are not.
    """
    import requests
    sep = "&" if "?" in url else "?"
    for qs in list(sizes) + [""]:
        try:
            r = requests.get(url + (sep + qs if qs else ""), timeout=40)
            if r.status_code == 200 and r.content:
                return r.content
        except Exception:
            pass
    return b""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--assets", default=str(REPO / "out/build/win-amd64-debug/assets"),
                    help="assets folder holding covers/ and icons/")
    ap.add_argument("--force", action="store_true", help="overwrite art that is already there")
    ap.add_argument("--all-devices", action="store_true",
                    help="include titles never played on an Xbox 360")
    ap.add_argument("--dry-run", action="store_true", help="list what would be written")
    args = ap.parse_args()

    auth, xuid = xbl_auth.sign_in()
    if not xuid:
        raise SystemExit("signed in but no xuid came back")

    r = xbl_auth.get(TITLEHUB % xuid, auth, contract="2")
    if r.status_code != 200:
        raise SystemExit("titlehub refused (%d): %s" % (r.status_code, r.text[:200]))
    titles = r.json().get("titles", [])

    if not args.all_devices:
        titles = [t for t in titles if "Xbox360" in (t.get("devices") or [])]
    print("  %d title(s) to consider" % len(titles))

    covers = Path(args.assets) / "covers"
    icons = Path(args.assets) / "icons"
    if not args.dry_run:
        covers.mkdir(parents=True, exist_ok=True)
        icons.mkdir(parents=True, exist_ok=True)

    wrote = skipped = failed = 0
    for t in titles:
        try:
            tid = int(t["titleId"])
        except (KeyError, TypeError, ValueError):
            continue
        hexid = "%08X" % tid
        name = t.get("name") or hexid

        # Prefer a BoxArt entry when there is one, else whatever the tile is.
        url = ""
        for im in (t.get("images") or []):
            if (im.get("type") or "").lower() == "boxart" and im.get("url"):
                url = im["url"]
                break
        url = url or (t.get("displayImage") or "")
        if not url:
            print("    %s  %-34s no artwork" % (hexid, name[:34]))
            failed += 1
            continue

        cover_p = covers / (hexid + ".png")
        thumb_p = covers / (hexid + ".thumb.png")
        icon_p = icons / (hexid + ".png")

        # Per file, not all-or-nothing. Art that is already there was put there
        # on purpose and is very likely better than what this fetches -- the
        # 267x400 Halo 3 cover in assets/covers is real box art, and replacing it
        # with a square store tile because its icon happened to be missing would
        # be a straight downgrade. --force is the only way to overwrite.
        want = [(p, kind) for p, kind in ((cover_p, "cover"), (thumb_p, "thumb"), (icon_p, "icon"))
                if args.force or not p.exists()]
        if not want:
            skipped += 1
            continue
        if args.dry_run:
            print("    %s  %s" % (hexid, name[:44]))
            wrote += 1
            continue

        kinds = {k for _, k in want}
        cover = fetch(url, COVER_QS) if "cover" in kinds else b""
        # thumb and icon are the same small picture, fetched once.
        small = fetch(url, THUMB_QS) if kinds & {"thumb", "icon"} else b""

        if ("cover" in kinds and not cover) or (kinds & {"thumb", "icon"} and not small):
            print("    %s  %-34s download failed" % (hexid, name[:34]))
            failed += 1
            continue
        if small and len(small) > THUMB_LIMIT:
            # Refused rather than written: the dashboard rejects an oversized
            # thumbnail, and half a picture is worse than none.
            print("    %s  %-34s thumb %d bytes over the %d limit; skipped"
                  % (hexid, name[:34], len(small), THUMB_LIMIT))
            failed += 1
            continue

        for p, kind in want:
            p.write_bytes(cover if kind == "cover" else small)
        wrote += 1
        print("    %s  %-34s %s" % (hexid, name[:34], "+".join(sorted(kinds))))

    print()
    print("  wrote %d, skipped %d already present, %d without usable art"
          % (wrote, skipped, failed))
    if not args.dry_run and wrote:
        print("  covers in %s" % covers)
        print("  icons  in %s" % icons)
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(HERE))
    raise SystemExit(main())
