#!/usr/bin/env python3
"""Write gamedir/recent.txt: the people you have recently played with.

The Xbox 360 kept a Recent Players list beside Friends, and the service behind it
is still up: peoplehub's recentplayers view returns the same thing it always did,
including what you played together and when.

    python tools/fetch_recent_players.py
    python tools/fetch_recent_players.py --limit 30
    python tools/fetch_recent_players.py --dry-run

Written as one category with the players under it, the same shape the marketplace
rows use, because there are far more recent players than a channel can hold --
0x922D6018 refuses any slot past the 64th. So the Friends row gets a single
"Recent Players" tile and the list itself lives on the page behind it.

Everything written is a plain text file plus PNGs. The dashboard reads them off
disk and never touches the network.
"""
import argparse
import sys
from pathlib import Path

try:
    import requests
except ImportError:  # pragma: no cover
    raise SystemExit("this needs 'requests':  python -m pip install requests")

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

import xbl_auth  # noqa: E402
from fetch_friends import safe_name  # noqa: E402
from fetch_marketplace import (ascii_safe, fit_to_card,  # noqa: E402
                               make_category_card)

RECENT = "https://peoplehub.xboxlive.com/users/me/people/recentplayers"

MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")


def when(stamp):
    """'2026-08-24T22:41:07Z' -> '24 Aug 2026'. Empty when it will not parse."""
    text = str(stamp or "")
    if len(text) < 10:
        return ""
    try:
        year, month, day = int(text[0:4]), int(text[5:7]), int(text[8:10])
        return "%d %s %d" % (day, MONTHS[month - 1], year)
    except (ValueError, IndexError):
        return ""


def subtitle_for(person):
    """What the second line says: the game you played together, and when.

    This is what the 360's own Recent Players list showed, and peoplehub still
    carries it under recentPlayer.titles. Presence is not used -- presenceText
    comes back null for almost everyone here, because these are people you played
    with once rather than people you follow.
    """
    titles = ((person.get("recentPlayer") or {}).get("titles") or [])
    if not titles:
        return ""
    first = titles[0]
    name = " ".join(str(first.get("titleName") or "").split())
    date = when(first.get("lastPlayedWithDateTime"))
    if name and date:
        return "%s · %s" % (name, date)
    return name or date


def gamerpic(url, is_360):
    """The gamerpic, asking the way the image service wants to be asked.

    A legacy 360 gamerpic answers w=424 alone, and the bare URL, with an error;
    it wants width and height together. Asking for both first and falling back
    covers either kind -- the same ladder fetch_friends.py needed.
    """
    if not url:
        return b""
    sep = "&" if "?" in url else "?"
    ladder = [url + sep + "w=424&h=424", url + sep + "w=424", url]
    if is_360:
        ladder = [url + sep + "w=424&h=424"]
    for candidate in ladder:
        try:
            r = requests.get(candidate, timeout=30)
            if r.status_code == 200 and r.content:
                return r.content
        except Exception:
            pass
    return b""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--limit", type=int, default=60,
                    help="how many players to write; a channel holds 64 slots (default 60)")
    ap.add_argument("--force", action="store_true", help="re-download gamerpics already present")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    auth, _ = xbl_auth.sign_in()
    r = xbl_auth.get(RECENT, auth, contract="5")
    if r.status_code != 200:
        raise SystemExit("peoplehub refused (%d): %s" % (r.status_code, r.text[:200]))
    people = r.json().get("people", [])
    print("  %d recent player(s); taking %d" % (len(people), min(len(people), args.limit)))

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "recent"
    if not args.dry_run:
        imgdir.mkdir(parents=True, exist_ok=True)

    try:
        from PIL import Image
        composer = ("pillow", Image)
    except ImportError:
        import shutil
        composer = ("ffmpeg", None) if shutil.which("ffmpeg") else ("none", None)

    rows, made = [], []
    for p in people:
        if len(rows) >= args.limit:
            break
        tag = (p.get("gamertag") or p.get("modernGamertag") or "").strip()
        if not tag:
            continue

        name = safe_name(tag) + ".png"
        dest = imgdir / name
        rel = "images/recent/" + name

        if not args.dry_run and (args.force or not dest.exists()):
            art = gamerpic(p.get("displayPicRaw"), bool(p.get("isXbox360Gamerpic")))
            if not art:
                print("    %-22s no gamerpic; skipped" % ascii_safe(tag)[:22])
                continue
            tile, _ = fit_to_card(art)
            dest.write_bytes(tile)
        elif not dest.exists():
            continue

        made.append(dest)
        sub = subtitle_for(p)
        rows.append((tag, sub, rel, str(p.get("xuid") or ""), ""))
        print("    %-22s %s" % (ascii_safe(tag)[:22], ascii_safe(sub)[:52]))

    if not rows:
        raise SystemExit("no recent players had a usable gamerpic")

    # The tile that opens the list, wearing a grid of the faces behind it.
    card_rel, card = "", imgdir / "cat_Recent_Players.png"
    if not args.dry_run and made:
        if args.force or not card.exists():
            art = make_category_card(made, composer)
            if art:
                card.write_bytes(art)
        if card.exists():
            card_rel = "images/recent/" + card.name

    out = [
        "# Written by tools/fetch_recent_players.py -- edit freely, it is only a text file.",
        "# The people you have recently played with, from peoplehub's recentplayers view.",
        "# name | subtitle | image | xuid | kind ('category' marks the heading)",
        "",
        "Recent Players|%d player%s|%s||category"
        % (len(people), "" if len(people) == 1 else "s", card_rel),
    ]
    out += ["|".join(str(x).replace("|", "/") for x in r) for r in rows]
    text = "\n".join(out) + "\n"

    if args.dry_run:
        print()
        print(ascii_safe(text))
        return 0

    (gamedir / "recent.txt").write_text(text, encoding="utf-8")
    print()
    print("  wrote %d player(s) to %s" % (len(rows), gamedir / "recent.txt"))
    print("  gamerpics in %s" % imgdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
