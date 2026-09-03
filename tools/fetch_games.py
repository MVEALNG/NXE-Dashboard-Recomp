#!/usr/bin/env python3
"""Write gamedir/games.txt and tile art for the Game Marketplace row.

Same shape as fetch_friends.py: this writes a plain file, the dashboard reads
it, and nothing in the recompiled dashboard ever talks to the network. The list
can be edited or hand-written, so the feature works without an account.

    python tools/fetch_games.py              # most recently played first
    python tools/fetch_games.py --limit 12
    python tools/fetch_games.py --dry-run

Tile art is square (424x424) rather than the covers fetch_covers.py writes.
A slot picture is drawn on a 420x320 card and the friend gamerpics -- also
square, also 424 -- sit on it correctly, so this asks for the same thing. The
85x120 EDS pictures some older titles still use are far too small to fill a
tile, so those are taken at whatever the host will give and will look soft.
"""
import argparse
import re
import sys
from pathlib import Path

import xbl_auth

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

TITLEHUB = ("https://titlehub.xboxlive.com/users/xuid(%s)/titles/titlehistory"
            "/decoration/scid,image,achievement")
TILE_QS = "w=424&h=424"


def fetch(url: str, *sizes: str) -> bytes:
    """First size the host serves, else the raw image. See fetch_covers.py."""
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


def safe(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_") or "game"


def achievement_line(t: dict) -> str:
    """"12 of 59 achievements, 245 of 1345 G", or empty when nothing is earned."""
    a = t.get("achievement") or {}
    cur, tot = a.get("currentAchievements"), a.get("totalAchievements")
    gs, gtot = a.get("currentGamerscore"), a.get("totalGamerscore")
    if not tot:
        return ""
    if cur:
        if gs and gtot:
            return "%d of %d achievements, %d of %d G" % (cur, tot, gs, gtot)
        return "%d of %d achievements" % (cur, tot)
    return "%d achievements, %d G" % (tot, gtot or 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--limit", type=int, default=20, help="how many games to write")
    ap.add_argument("--all-devices", action="store_true",
                    help="include titles never played on an Xbox 360")
    ap.add_argument("--force", action="store_true", help="re-download art already present")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    auth, xuid = xbl_auth.sign_in()
    r = xbl_auth.get(TITLEHUB % xuid, auth, contract="2")
    if r.status_code != 200:
        raise SystemExit("titlehub refused (%d): %s" % (r.status_code, r.text[:200]))
    titles = r.json().get("titles", [])
    if not args.all_devices:
        titles = [t for t in titles if "Xbox360" in (t.get("devices") or [])]

    # Most recently played first, which is the order a games row wants.
    def last_played(t):
        return ((t.get("titleHistory") or {}).get("lastTimePlayed") or "")
    titles.sort(key=last_played, reverse=True)
    print("  %d Xbox 360 title(s); taking %d" % (len(titles), min(len(titles), args.limit)))

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "games"
    if not args.dry_run:
        imgdir.mkdir(parents=True, exist_ok=True)

    rows = []
    for t in titles:
        if len(rows) >= args.limit:
            break
        try:
            tid = int(t["titleId"])
        except (KeyError, TypeError, ValueError):
            continue
        name = (t.get("name") or "").strip()
        if not name:
            continue
        hexid = "%08X" % tid

        url = ""
        for im in (t.get("images") or []):
            if (im.get("type") or "").lower() == "boxart" and im.get("url"):
                url = im["url"]
                break
        url = url or (t.get("displayImage") or "")
        if not url:
            print("    %-34s no artwork; skipped" % name[:34])
            continue

        rel = "images/games/%s.png" % hexid
        dest = imgdir / ("%s.png" % hexid)
        if not args.dry_run and (args.force or not dest.exists()):
            data = fetch(url, TILE_QS)
            if not data:
                print("    %-34s download failed; skipped" % name[:34])
                continue
            dest.write_bytes(data)

        rows.append((name, achievement_line(t), rel, hexid))
        print("    %-36s %s" % (name[:36], achievement_line(t)))

    out = [
        "# Written by tools/fetch_games.py -- edit freely, it is only a text file.",
        "# name | subtitle | image | title id (hex)",
        "",
    ]
    out += ["%s|%s|%s|%s" % r for r in rows]
    text = "\n".join(out) + "\n"

    if args.dry_run:
        print()
        print(text)
        return 0
    (gamedir / "games.txt").write_text(text, encoding="utf-8")
    print()
    print("  wrote %d game(s) to %s" % (len(rows), gamedir / "games.txt"))
    print("  tile art in %s" % imgdir)
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(HERE))
    raise SystemExit(main())
