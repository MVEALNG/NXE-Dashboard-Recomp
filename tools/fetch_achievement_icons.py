#!/usr/bin/env python3
"""Fill in the achievement icons the profile's GPDs are missing.

A GPD stores each achievement's icon in namespace 2, keyed by the image id the
achievement record names. Whether they are there depends on whether the console
ever fetched them: of the 44 title GPDs in the profile staged here, 18 carry
some icons and 26 carry none at all, leaving ~700 achievements with nothing to
draw.

The icons come from Xbox Live's image server, which is the same place the
console got them and is still serving:

    http://image.xboxlive.com/global/t.<titleid>/ach/0/<imageid>

That was checked against a title whose GPD does have icons -- PvZ Garden
Warfare, 454109C9 -- where achievement 1 comes back as a 13,112 byte PNG
against the 13,110 bytes stored in the GPD. Same image, so both the endpoint
and the id mapping are right.

    python tools/fetch_achievement_icons.py [--limit N] [--dry-run]

Downloads land beside the storage device rather than being written back into
the GPDs, which stay untouched:

    A:/Xbox360Storage/Cache/achievement_icons/<TITLEID>/<imageid>.png

src/gpd_images.cpp looks there when a GPD has no image under an id. Existing
files are skipped, so re-running only fetches what is still missing.

Deliberately not scraped from a site: xboxachievements.com answers automated
requests with 403, and working around that is not something to do. This uses
the platform's own endpoint, one request per missing icon, paced.
"""
import argparse
import os
import struct
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from profile_summary import xdbf_entries  # noqa: E402

PROFILE = (r"A:\Xbox360Storage\Content\ECF094C2048FC0CD\FFFE07D1\00010000"
           r"\ECF094C2048FC0CD")
CACHE = r"A:\Xbox360Storage\Cache\achievement_icons"

URL = "http://image.xboxlive.com/global/t.{title:08x}/ach/0/{image}"

NAMESPACE_ACHIEVEMENT = 1
NAMESPACE_IMAGE = 2


def missing_icons(profile_dir):
    """[(title_id, image_id)] the GPDs reference but do not contain."""
    wanted = []
    for name in sorted(os.listdir(profile_dir)):
        if not name.lower().endswith(".gpd") or name.upper().startswith("FFF"):
            continue
        try:
            title_id = int(name[:-4], 16)
        except ValueError:
            continue

        data = open(os.path.join(profile_dir, name), "rb").read()
        entries = xdbf_entries(data)
        have = {ident for ns, ident, _o, _l in entries if ns == NAMESPACE_IMAGE}

        for ns, _ident, off, _length in entries:
            if ns != NAMESPACE_ACHIEVEMENT or off + 0x18 > len(data):
                continue
            image_id = struct.unpack_from(">I", data, off + 0x08)[0]
            if image_id and image_id not in have:
                wanted.append((title_id, image_id))
    # Stable order, no duplicates.
    return sorted(set(wanted))


def fetch(title_id, image_id, timeout=20):
    url = URL.format(title=title_id, image=image_id)
    request = urllib.request.Request(url, headers={"User-Agent": "Xbox360/2.0"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        data = response.read()
    # Anything that is not a PNG is an error page, not an icon.
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError(f"not a PNG ({len(data)} bytes)")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--limit", type=int, default=0,
                        help="stop after this many downloads")
    parser.add_argument("--dry-run", action="store_true",
                        help="list what is missing and exit")
    parser.add_argument("--delay", type=float, default=0.15,
                        help="seconds between requests (default 0.15)")
    parser.add_argument("--profile", default=PROFILE)
    parser.add_argument("--cache", default=CACHE)
    args = parser.parse_args()

    wanted = missing_icons(args.profile)
    titles = len({t for t, _ in wanted})
    print(f"{len(wanted)} icon(s) missing across {titles} title(s)")
    if args.dry_run:
        return 0

    got = skipped = failed = 0
    for title_id, image_id in wanted:
        directory = os.path.join(args.cache, f"{title_id:08X}")
        target = os.path.join(directory, f"{image_id}.png")
        if os.path.exists(target) and os.path.getsize(target):
            skipped += 1
            continue
        if args.limit and got >= args.limit:
            break

        try:
            data = fetch(title_id, image_id)
        except (urllib.error.URLError, urllib.error.HTTPError, ValueError, OSError) as e:
            failed += 1
            if failed <= 10:
                print(f"  ! {title_id:08X}/{image_id}: {e}")
            time.sleep(args.delay)
            continue

        os.makedirs(directory, exist_ok=True)
        with open(target, "wb") as handle:
            handle.write(data)
        got += 1
        if got % 50 == 0:
            print(f"  {got} downloaded...")
        time.sleep(args.delay)

    print(f"downloaded {got}, skipped {skipped} already present, {failed} failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
