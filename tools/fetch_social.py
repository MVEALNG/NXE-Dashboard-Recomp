#!/usr/bin/env python3
"""Write the social graph: following, followers, and a page per person.

The profile page already said "16 following, 18 followers", because
social.xboxlive.com's summary hands those two numbers over for free. They were
dead text: a line you could read and not press. peoplehub serves the lists
themselves from the same account, so the counts became pages.

    peoplehub /users/me/people/social         the people you follow
    peoplehub /users/me/people/followers      the people who follow you
    peoplehub /users/me/people/recentplayers  the people you have played with

    -> gamedir/following.txt   the first, as a page
    -> gamedir/followers.txt   the second, as a page
    -> gamedir/people.txt      a page each, for everybody in all three

"social" is the one the Xbox app calls Friends, and it is the same view
fetch_friends.py reads -- so following.txt and friends.txt cover the same people.
They are not redundant. friends.txt is a row of tiles on the channel, capped by
the 64 slots a channel has; this is the whole list behind a heading, the way
Recent Players already works. One is a shelf, the other is the drawer.

Recent players are here for their pages only. recent.txt is fetch_recent_players'
to write, and this does not touch it -- but the people on it had nowhere to go
when selected, and now they do.

    python tools/fetch_social.py
    python tools/fetch_social.py --limit 40 --recent-limit 30
    python tools/fetch_social.py --no-summaries      # skip the per-person counts
    python tools/fetch_social.py --dry-run

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
from fetch_friends import dim_offline, is_online, safe_name  # noqa: E402
from fetch_marketplace import (ascii_safe, fit_to_card,  # noqa: E402
                               make_category_card)
from fetch_recent_players import gamerpic, subtitle_for as played_together  # noqa: E402

PEOPLEHUB = ("https://peoplehub.xboxlive.com/users/me/people/%s"
             "/decoration/presenceDetail")
RECENT = "https://peoplehub.xboxlive.com/users/me/people/recentplayers"
SUMMARY = "https://social.xboxlive.com/users/xuid(%s)/summary"

# view on peoplehub, heading on the page, file to write
VIEWS = (
    ("social", "Following", "following.txt"),
    ("followers", "Followers", "followers.txt"),
)


def subtitle_for(person):
    """The second line: what they are doing, or what they have scored.

    presenceText is the better line and the one the blade showed -- "Online -
    Halo 3", "Last seen 2d ago: Minecraft". It comes back empty for everyone on
    the recentplayers view and for some followers, so gamerscore stands in
    rather than leaving the tile with a blank second line.
    """
    presence = (person.get("presenceText") or "").strip()
    if presence:
        return presence
    try:
        score = int(person.get("gamerScore") or 0)
    except (TypeError, ValueError):
        return ""
    return "{:,} G".format(score) if score else ""


def relationship(person):
    """Which way round the follow goes, in the words the console used."""
    theirs = bool(person.get("isFollowingCaller"))    # they follow you
    yours = bool(person.get("isFollowedByCaller"))    # you follow them
    if theirs and yours:
        return "Friends"
    if yours:
        return "You follow them"
    if theirs:
        return "Follows you"
    return ""


def summary_of(xuid, auth, cache):
    """(following, followers) for anybody, not just you.

    social.xboxlive.com's summary is not restricted to the signed-in account --
    the same call answers for any XUID -- so a person's page can carry the two
    numbers their own profile would show. One request each, cached, and skipped
    entirely with --no-summaries because it is the only part of this that costs
    a round trip per person.
    """
    if not xuid:
        return None
    if xuid in cache:
        return cache[xuid]
    result = None
    try:
        r = requests.get(SUMMARY % xuid, timeout=30, headers={
            "Authorization": auth, "x-xbl-contract-version": "1"})
        if r.status_code == 200:
            d = r.json()
            result = (int(d.get("targetFollowingCount") or 0),
                      int(d.get("targetFollowerCount") or 0))
    except Exception:
        result = None
    cache[xuid] = result
    return result


def fetch(view, auth):
    r = xbl_auth.get(PEOPLEHUB % view, auth, contract="5")
    if r.status_code != 200:
        raise SystemExit("peoplehub refused %s (%d): %s"
                         % (view, r.status_code, r.text[:200]))
    return r.json().get("people", [])


def picture(person, imgdir, dry_run, force):
    """The person's tile, fitted and dimmed if they are offline.

    Returns the gamedir-relative path, or "" when there is no usable picture --
    which matters more than it looks: a tile with no art faults the page when it
    is opened, so anybody without one is dropped rather than drawn.
    """
    tag = (person.get("gamertag") or person.get("modernGamertag") or "").strip()
    name = safe_name(tag) + ".png"
    dest = imgdir / name
    rel = "images/social/" + name
    if dry_run:
        return rel
    if force or not dest.exists():
        art = gamerpic(person.get("displayPicRaw"), bool(person.get("isXbox360Gamerpic")))
        if not art:
            return ""
        tile, _ = fit_to_card(art)
        if not is_online(person):
            tile = dim_offline(tile)
        dest.write_bytes(tile)
    return rel if dest.exists() else ""


def write_people(gamedir, everyone, auth, summaries, dry_run):
    """One page per person, as label/value rows under their own heading.

    Built the way profile.txt is, because it is the same kind of page and the
    dashboard already draws that one correctly. The alternative was the
    marketplace detail page, which would have meant filing a bio under
    "description" and a gamerscore under "rating" and leaving both field names
    lying about their contents from then on.

    Every row carries the person's own gamerpic. Not decoration: a row with no
    picture faults the page when it is opened.
    """
    if not everyone:
        return
    out = [
        "# Written by tools/fetch_social.py -- edit freely, it is only a text file.",
        "# A page per person: everyone you follow, who follows you, or you have played with.",
        "# label | value | image | id | kind ('category' marks a person's heading)",
    ]
    cache = {}
    written = 0
    for tag in sorted(everyone, key=str.lower):
        person, art = everyone[tag]
        detail = []
        try:
            score = int(person.get("gamerScore") or 0)
        except (TypeError, ValueError):
            score = 0
        if score:
            detail.append(("Gamerscore", "{:,} G".format(score)))
        presence = (person.get("presenceText") or "").strip()
        if presence:
            detail.append(("Status", presence))
        rel = relationship(person)
        if rel:
            detail.append(("Relationship", rel))
        real = (person.get("realName") or "").strip()
        if real:
            detail.append(("Name", real))
        # What you played together, when peoplehub knows -- the line the 360's
        # own Recent Players list showed.
        together = played_together(person)
        if together:
            detail.append(("Played together", together))
        if summaries:
            counts = summary_of(str(person.get("xuid") or ""), auth, cache)
            if counts:
                detail.append(("Following", "{:,} people".format(counts[0])))
                detail.append(("Followers", "{:,} people".format(counts[1])))
        if not detail:
            continue  # a heading with nothing under it is not a page
        out.append("")
        out.append("%s|%s|%s||category" % (tag, subtitle_for(person) or "Xbox Live", art))
        out += ["%s|%s|%s||" % (label, str(value).replace("|", "/"), art)
                for label, value in detail]
        written += 1
    text = "\n".join(out) + "\n"
    if dry_run:
        print(ascii_safe(text[:2000]))
        return
    (gamedir / "people.txt").write_text(text, encoding="utf-8")
    print("  wrote %d person page(s) to %s" % (written, gamedir / "people.txt"))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--limit", type=int, default=60,
                    help="how many people per list; a channel holds 64 slots (default 60)")
    ap.add_argument("--recent-limit", type=int, default=60,
                    help="how many recent players also get a page (default 60)")
    ap.add_argument("--no-summaries", action="store_true",
                    help="skip the per-person following/follower counts (one request each)")
    ap.add_argument("--force", action="store_true", help="re-download gamerpics already present")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    auth, _ = xbl_auth.sign_in()

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "social"
    if not args.dry_run:
        imgdir.mkdir(parents=True, exist_ok=True)

    try:
        from PIL import Image
        composer = ("pillow", Image)
    except ImportError:
        import shutil
        composer = ("ffmpeg", None) if shutil.which("ffmpeg") else ("none", None)

    # Everyone seen anywhere, deduplicated by gamertag, for the per-person pages.
    # First writer wins, and the lists come before recent players on purpose:
    # the social views carry presence and relationship flags that recentplayers
    # does not, so the richer record is the one kept.
    everyone = {}

    for view, heading, filename in VIEWS:
        people = fetch(view, auth)
        # Online first, as on the friends row, and for the same reason: the
        # write loop stops at --limit, so this has to happen before it.
        people.sort(key=lambda p: 0 if is_online(p) else 1)
        print("  %s: %d person(s); taking %d"
              % (heading, len(people), min(len(people), args.limit)))

        rows, made = [], []
        for p in people:
            if len(rows) >= args.limit:
                break
            tag = (p.get("gamertag") or p.get("modernGamertag") or "").strip()
            if not tag:
                continue
            rel = picture(p, imgdir, args.dry_run, args.force)
            if not rel:
                print("    %-22s no gamerpic; skipped" % ascii_safe(tag)[:22])
                continue
            made.append(imgdir / Path(rel).name)
            everyone.setdefault(tag, (p, rel))
            rows.append((tag, subtitle_for(p), rel, str(p.get("xuid") or ""), ""))

        if not rows:
            print("    (nobody with a usable gamerpic; %s not written)" % filename)
            continue

        card_rel, card = "", imgdir / ("cat_%s.png" % heading)
        if not args.dry_run and made:
            if args.force or not card.exists():
                art = make_category_card(made, composer)
                if art:
                    card.write_bytes(art)
            if card.exists():
                card_rel = "images/social/" + card.name

        out = [
            "# Written by tools/fetch_social.py -- edit freely, it is only a text file.",
            "# %s, from peoplehub's %s view." % (heading, view),
            "# name | subtitle | image | xuid | kind ('category' marks the heading)",
            "",
            "%s|%d %s|%s||category"
            % (heading, len(rows), "person" if len(rows) == 1 else "people", card_rel),
        ]
        out += ["|".join(str(x).replace("|", "/") for x in r) for r in rows]
        text = "\n".join(out) + "\n"
        if args.dry_run:
            print(ascii_safe(text))
            continue
        (gamedir / filename).write_text(text, encoding="utf-8")
        print("  wrote %d to %s" % (len(rows), gamedir / filename))

    # Recent players, for their pages only. recent.txt belongs to
    # fetch_recent_players.py and is not touched here.
    r = xbl_auth.get(RECENT, auth, contract="5")
    recent = r.json().get("people", []) if r.status_code == 200 else []
    added = 0
    print("  Recent players: %d; giving pages to %d"
          % (len(recent), min(len(recent), args.recent_limit)))
    for p in recent:
        if added >= args.recent_limit:
            break
        tag = (p.get("gamertag") or p.get("modernGamertag") or "").strip()
        if not tag or tag in everyone:
            continue
        rel = picture(p, imgdir, args.dry_run, args.force)
        if not rel:
            continue
        everyone[tag] = (p, rel)
        added += 1

    if not args.no_summaries and not args.dry_run:
        print("  fetching follower counts for %d person(s)" % len(everyone))
    write_people(gamedir, everyone, auth, not args.no_summaries, args.dry_run)

    if not args.dry_run:
        print("  gamerpics in %s" % imgdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
