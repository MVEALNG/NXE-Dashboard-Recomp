#!/usr/bin/env python3
"""Write gamedir/titles.txt and gamedir/gamestats.txt: the games you have played.

    titlehub  /users/xuid(..)/titles/titlehistory/decoration/scid,image,achievement
    userstats /users/xuid(..)/scids/<scid>/stats/MinutesPlayed

    -> gamedir/titles.txt      a "My Games" row, one tile per title
    -> gamedir/gamestats.txt   a page per title: achievements, gamerscore, time played

fetch_games.py has written games.txt since before there was anywhere to put it,
and nothing in the dashboard ever read it -- there is no games_list setting. So
this writes the row in the shape the dashboard actually consumes, the same one
recent.txt uses: one category with the titles under it.

On stat names
-------------
userstats answers per title, and the names are the title's own: each game's
service configuration decides what it records, so there is no list that works
everywhere. MinutesPlayed is the one the platform defines rather than the title,
so it is the one asked for. A title that does not keep it simply has no time on
its page, which is why the row is built from titlehub and only decorated from
userstats -- a stat that is missing must not cost the tile.

    python tools/fetch_title_stats.py
    python tools/fetch_title_stats.py --limit 20
    python tools/fetch_title_stats.py --no-stats     # titlehub only, no per-title calls
    python tools/fetch_title_stats.py --dry-run

Everything written is a plain text file. Tile art comes from fetch_games.py, and
this reuses it rather than downloading it again.
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
from fetch_marketplace import ascii_safe, make_category_card  # noqa: E402
from fetch_recent_players import when  # noqa: E402

TITLEHUB = ("https://titlehub.xboxlive.com/users/xuid(%s)/titles/titlehistory"
            "/decoration/scid,image,achievement")
USERSTATS = "https://userstats.xboxlive.com/users/xuid(%s)/scids/%s/stats/MinutesPlayed"


def filetime(stamp):
    """ISO 8601 -> FILETIME: 100ns ticks since 1601, which is what a GPD stores.

    The Game Library sorts Recent Games on this, so a title arriving with zero
    sinks to the bottom rather than landing wherever it happens to fall.
    """
    text = str(stamp or "")
    if len(text) < 19:
        return 0
    try:
        from datetime import datetime, timezone
        dt = datetime(int(text[0:4]), int(text[5:7]), int(text[8:10]),
                      int(text[11:13]), int(text[14:16]), int(text[17:19]),
                      tzinfo=timezone.utc)
        epoch = datetime(1601, 1, 1, tzinfo=timezone.utc)
        return int((dt - epoch).total_seconds()) * 10000000
    except (ValueError, TypeError):
        return 0


def commas(n):
    return "{:,}".format(int(n))


def playtime(minutes):
    """3 -> '3 minutes'; 90 -> '1 hour 30 minutes'; 1440 -> '1 day'."""
    minutes = int(minutes)
    if minutes < 60:
        return "%d minute%s" % (minutes, "" if minutes == 1 else "s")
    hours, mins = divmod(minutes, 60)
    if hours < 24:
        out = "%d hour%s" % (hours, "" if hours == 1 else "s")
        return out if not mins else out + " %d minute%s" % (mins, "" if mins == 1 else "s")
    days, hours = divmod(hours, 24)
    out = "%d day%s" % (days, "" if days == 1 else "s")
    return out if not hours else out + " %d hour%s" % (hours, "" if hours == 1 else "s")


def minutes_played(xuid, scid, auth):
    """MinutesPlayed for one title, or None when the title does not keep it."""
    if not scid:
        return None
    try:
        r = requests.get(USERSTATS % (xuid, scid), timeout=30, headers={
            "Authorization": auth, "x-xbl-contract-version": "2"})
        if r.status_code != 200:
            return None
        for coll in r.json().get("statlistscollection", []):
            for stat in coll.get("stats", []):
                if stat.get("name") == "MinutesPlayed" and stat.get("value") is not None:
                    return int(float(stat["value"]))
    except Exception:
        return None
    return None


def achievement_line(t):
    """"12 of 59 achievements, 245 of 1345 G", or empty when nothing is earned."""
    a = t.get("achievement") or {}
    cur, tot = a.get("currentAchievements"), a.get("totalAchievements")
    gs, gtot = a.get("currentGamerscore"), a.get("totalGamerscore")
    if not tot:
        return ""
    if cur:
        if gs and gtot:
            return "%s of %s achievements, %s of %s G" % (commas(cur), commas(tot),
                                                          commas(gs), commas(gtot))
        return "%s of %s achievements" % (commas(cur), commas(tot))
    return "%s achievements, %s G" % (commas(tot), commas(gtot or 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--limit", type=int, default=60,
                    help="how many titles; a channel holds 64 slots (default 60)")
    ap.add_argument("--no-stats", action="store_true",
                    help="skip userstats, one request per title")
    ap.add_argument("--all-devices", action="store_true",
                    help="include titles never played on an Xbox 360")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    auth, xuid = xbl_auth.sign_in()
    r = xbl_auth.get(TITLEHUB % xuid, auth, contract="2")
    if r.status_code != 200:
        raise SystemExit("titlehub refused (%d): %s" % (r.status_code, r.text[:200]))
    titles = r.json().get("titles", [])
    if not args.all_devices:
        titles = [t for t in titles if "Xbox360" in (t.get("devices") or [])]
    titles.sort(key=lambda t: ((t.get("titleHistory") or {}).get("lastTimePlayed") or ""),
                reverse=True)
    print("  %d Xbox 360 title(s); taking %d" % (len(titles), min(len(titles), args.limit)))

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "games"

    # Which games have an achievement list, so their page can link to it
    # instead of merely counting. Written by fetch_achievements.py, which runs
    # separately -- absent, the count stays a plain row and nothing breaks.
    have_achievements = set()
    ach_file = gamedir / "achievements.txt"
    if ach_file.exists():
        for line in ach_file.read_text(encoding="utf-8").splitlines():
            if line.rstrip().endswith("|category"):
                have_achievements.add(line.split("|", 1)[0])

    rows, pages, made, library = [], [], [], []
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
        art = imgdir / ("%s.png" % hexid)
        # A tile with no picture faults the page it is on, so a title whose art
        # fetch_games.py never fetched is left out rather than drawn blank.
        if not art.exists():
            continue
        rel = "images/games/%s.png" % hexid
        made.append(art)

        detail = []
        a = t.get("achievement") or {}
        if a.get("totalAchievements"):
            earned = "%s of %s" % (commas(a.get("currentAchievements") or 0),
                                   commas(a["totalAchievements"]))
            if name in have_achievements:
                # A link row: the tile opens the achievement list rather than
                # stating a number you cannot press.
                detail.append(("Achievements", earned, "ach:" + name))
            else:
                detail.append(("Achievements", earned))
        if a.get("totalGamerscore"):
            detail.append(("Gamerscore", "%s of %s G"
                           % (commas(a.get("currentGamerscore") or 0),
                              commas(a["totalGamerscore"]))))
        last = when((t.get("titleHistory") or {}).get("lastTimePlayed"))
        if last:
            detail.append(("Last played", last))
        if not args.no_stats:
            mins = minutes_played(xuid, t.get("serviceConfigId") or t.get("scid"), auth)
            if mins:
                detail.append(("Time played", playtime(mins)))

        # The Game Library wants the raw numbers rather than a sentence, and
        # the platform, so an original Xbox title can be filed as one.
        library.append((hexid, name.replace("|", "/"),
                        int(a.get("currentAchievements") or 0),
                        int(a.get("totalAchievements") or 0),
                        int(a.get("currentGamerscore") or 0),
                        int(a.get("totalGamerscore") or 0),
                        filetime((t.get("titleHistory") or {}).get("lastTimePlayed")),
                        "Xbox360" if "Xbox360" in (t.get("devices") or []) else "Xbox"))

        subtitle = achievement_line(t)
        rows.append((name, subtitle, rel, hexid, ""))
        if detail:
            pages.append((name, subtitle or "Xbox 360", rel, detail))
        print("    %-34s %s" % (ascii_safe(name)[:34],
                                ascii_safe(", ".join("%s %s" % (d[0], d[1]) for d in detail))[:60]))

    if not rows:
        raise SystemExit("no titles with artwork; run tools/fetch_games.py first")

    card_rel, card = "", imgdir / "cat_My_Games.png"
    if not args.dry_run and made:
        try:
            from PIL import Image
            composer = ("pillow", Image)
        except ImportError:
            composer = ("none", None)
        art = make_category_card(made, composer)
        if art:
            card.write_bytes(art)
        if card.exists():
            card_rel = "images/games/" + card.name

    row_out = [
        "# Written by tools/fetch_title_stats.py -- edit freely, it is only a text file.",
        "# The games you have played, from titlehub.",
        "# name | subtitle | image | title id (hex) | kind ('category' marks the heading)",
        "",
        "My Games|%d %s|%s||category"
        % (len(rows), "game" if len(rows) == 1 else "games", card_rel),
    ]
    row_out += ["|".join(str(x).replace("|", "/") for x in r) for r in rows]

    page_out = [
        "# Written by tools/fetch_title_stats.py -- edit freely, it is only a text file.",
        "# A page per game: achievements, gamerscore, and time played.",
        "# label | value | image | id | kind ('category' marks a game's heading)",
    ]
    for name, subtitle, rel, detail in pages:
        page_out.append("")
        page_out.append("%s|%s|%s||category" % (name.replace("|", "/"), subtitle, rel))
        for item in detail:
            label, value = item[0], item[1]
            link = item[2] if len(item) > 2 else ""
            page_out.append("%s|%s|%s|%s|%s"
                            % (label, str(value).replace("|", "/"), rel, link,
                               "link" if link else ""))

    if args.dry_run:
        print()
        print(ascii_safe("\n".join(row_out)))
        return 0

    lib_out = [
        "# Written by tools/fetch_title_stats.py -- edit freely, it is only a text file.",
        "# Games merged into the Game Library, from titlehub.",
        "# titleid(hex) | name | achievements earned | possible | gamerscore earned |"
        " total | last played (FILETIME) | platform",
    ]
    lib_out += ["|".join(str(x) for x in r) for r in library]
    (gamedir / "library.txt").write_text(chr(10).join(lib_out) + chr(10), encoding="utf-8")
    print("  wrote %d entr(ies) to %s" % (len(library), gamedir / "library.txt"))
    (gamedir / "titles.txt").write_text("\n".join(row_out) + "\n", encoding="utf-8")
    (gamedir / "gamestats.txt").write_text("\n".join(page_out) + "\n", encoding="utf-8")
    print()
    print("  wrote %d title(s) to %s" % (len(rows), gamedir / "titles.txt"))
    print("  wrote %d page(s) to %s" % (len(pages), gamedir / "gamestats.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
