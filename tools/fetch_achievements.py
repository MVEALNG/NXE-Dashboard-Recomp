#!/usr/bin/env python3
"""Write gamedir/achievements.txt: the achievement list for each game you own.

    achievements.xboxlive.com /users/xuid(..)/achievements?titleId=..

Contract version matters more than anything else here. The modern API is
contract 2 and it answers 200 with an empty list for every Xbox 360 title --
which reads exactly like "you have no achievements" rather than "wrong API", and
is the reason this looked impossible at first. Contract 1 returns all 79 of
Halo 3's, names and descriptions and unlock state included. Contract 3 works too;
1 is asked for because it is the one the 360 itself used.

A record carries more than the count ever did:

    name              "Landfall"
    description       "Completed the first mission of the Campaign."
    lockedDescription "Finish the first mission of the Campaign on Normal, ..."
    gamerscore        20
    unlocked          true
    imageId           2          -> image.xboxlive.com/global/t.<titleid>/ach/0/<id>

Locked achievements show their lockedDescription, which is what the console did:
a locked one still tells you how to get it unless it is secret, and a secret one
says so instead of spoiling itself.

On the row budget
-----------------
The dashboard's list parser stops at 1024 entries per file, so this cannot simply
be appended to the per-game pages -- 47 games at 50 achievements apiece is more
than double the ceiling. It gets its own file and its own budget, and --per-game
trims the longest lists so that one enormous game cannot crowd out the rest.

    python tools/fetch_achievements.py
    python tools/fetch_achievements.py --per-game 30
    python tools/fetch_achievements.py --earned-only
    python tools/fetch_achievements.py --dry-run

Everything written is a plain text file. Row art is the game's own tile, which
fetch_games.py has already fetched -- no icon is downloaded per achievement,
because every picture on a prebuilt page is held decoded and the loader gives up
around 84MB.
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
from fetch_marketplace import ascii_safe  # noqa: E402
from fetch_recent_players import when  # noqa: E402

ACHIEVEMENTS = ("https://achievements.xboxlive.com/users/xuid(%s)/achievements"
                "?titleId=%d&maxItems=%d")
TITLEHUB = ("https://titlehub.xboxlive.com/users/xuid(%s)/titles/titlehistory"
            "/decoration/achievement")

# The parser stops here, so the file is built to fit rather than be truncated
# arbitrarily at whatever game happens to fall on the boundary.
ROW_CEILING = 1000


def achievements_for(xuid, title_id, auth, want):
    r = requests.get(ACHIEVEMENTS % (xuid, title_id, want), timeout=30, headers={
        "Authorization": auth, "x-xbl-contract-version": "1"})
    if r.status_code != 200:
        return []
    try:
        return r.json().get("achievements", []) or []
    except Exception:
        return []


def clean(text):
    """One line, no pipes -- the field separator has to survive the data."""
    return " ".join(str(text or "").split()).replace("|", "/")


def describe(a):
    """What the second line says: how it was earned, or how to earn it."""
    if a.get("unlocked"):
        text = (a.get("description") or "").strip()
        stamp = when(a.get("timeUnlocked"))
        # 1753 is the SQL minimum date and means "unlocked, date unknown" --
        # every Halo 3 record here carries it. Printing it is worse than not.
        if stamp and not str(a.get("timeUnlocked", "")).startswith("1753"):
            return "%s  ·  %s" % (text, stamp) if text else stamp
        return text
    if a.get("isSecret"):
        return "Secret achievement -- keep playing to unlock"
    return (a.get("lockedDescription") or a.get("description") or "").strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--per-game", type=int, default=24,
                    help="most achievements to list per game (default 24)")
    ap.add_argument("--earned-only", action="store_true",
                    help="only games with at least one achievement earned")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    auth, xuid = xbl_auth.sign_in()
    r = xbl_auth.get(TITLEHUB % xuid, auth, contract="2")
    if r.status_code != 200:
        raise SystemExit("titlehub refused (%d): %s" % (r.status_code, r.text[:200]))
    titles = [t for t in r.json().get("titles", [])
              if "Xbox360" in (t.get("devices") or [])]
    titles.sort(key=lambda t: ((t.get("titleHistory") or {}).get("lastTimePlayed") or ""),
                reverse=True)

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "games"

    out = [
        "# Written by tools/fetch_achievements.py -- edit freely, it is only a text file.",
        "# The achievements for each game, from achievements.xboxlive.com (contract 1).",
        "# name | description | image | id | kind ('category' marks a game's heading)",
    ]
    rows = 0
    games = 0
    raw = []
    for t in titles:
        if rows >= ROW_CEILING:
            print("  row ceiling reached; %d game(s) listed" % games)
            break
        try:
            tid = int(t["titleId"])
        except (KeyError, TypeError, ValueError):
            continue
        name = (t.get("name") or "").strip()
        art = imgdir / ("%08X.png" % tid)
        # A row with no picture faults the page when it opens, so a game whose
        # tile was never fetched is skipped rather than listed blank.
        if not name or not art.exists():
            continue
        counts = t.get("achievement") or {}
        if args.earned_only and not counts.get("currentAchievements"):
            continue
        if not counts.get("totalAchievements"):
            continue

        got = achievements_for(xuid, tid, auth, args.per_game)
        if not got:
            continue
        # Unlocked first, then by gamerscore: the ones you earned are what you
        # came to look at, and the biggest of the rest are what you would go for.
        got.sort(key=lambda a: (0 if a.get("unlocked") else 1,
                                -int(a.get("gamerscore") or 0)))
        got = got[:args.per_game]

        rel = "images/games/%08X.png" % tid
        earned = sum(1 for a in got if a.get("unlocked"))
        out.append("")
        out.append("%s|%d of %d earned|%s||category"
                   % (name.replace("|", "/"), counts.get("currentAchievements") or 0,
                      counts.get("totalAchievements"), rel))
        for a in got:
            label = (a.get("name") or "Achievement").replace("|", "/")
            gs = int(a.get("gamerscore") or 0)
            mark = "" if a.get("unlocked") else "Locked -- "
            value = "%s%s" % (mark, describe(a).replace("|", "/"))
            if gs:
                value = "%d G  ·  %s" % (gs, value) if value else "%d G" % gs
            out.append("%s|%s|%s||" % (label, value, rel))
        # The library wants every achievement, not the page's trimmed list, and
        # wants them as records rather than sentences.
        for a in achievements_for(xuid, tid, auth, 512):
            raw.append((
                "%08X" % tid,
                int(a.get("id") or 0),
                int(a.get("gamerscore") or 0),
                int(a.get("flags") or 0),
                int(a.get("imageId") or 0),
                clean(a.get("name")),
                clean(a.get("description")),
                clean(a.get("lockedDescription")),
            ))

        rows += len(got) + 1
        games += 1
        print("    %-34s %d listed, %d earned" % (ascii_safe(name)[:34], len(got), earned))

    if args.dry_run:
        print()
        print(ascii_safe("\n".join(out[:60])))
        return 0

    data_out = [
        "# Written by tools/fetch_achievements.py -- edit freely, it is only a text file.",
        "# The Game Library's own view: one record per achievement.",
        "# titleid(hex) | id | gamerscore | flags | image id | name | description |"
        " locked description",
    ]
    data_out += ["|".join(str(x) for x in r) for r in raw]
    (gamedir / "achdata.txt").write_text(chr(10).join(data_out) + chr(10),
                                         encoding="utf-8")
    print("  wrote %d achievement record(s) to %s" % (len(raw), gamedir / "achdata.txt"))
    (gamedir / "achievements.txt").write_text("\n".join(out) + "\n", encoding="utf-8")
    print()
    print("  wrote %d game(s), %d row(s) to %s" % (games, rows, gamedir / "achievements.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
