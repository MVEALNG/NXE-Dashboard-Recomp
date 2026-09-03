#!/usr/bin/env python3
"""Write gamedir/profile.txt: your Xbox profile.

Profile on the gamer blade calls XamShowGamerCardUI, which is XAM's own system
gamercard and cannot be drawn here -- so the button was pointed at the LIVE
account editor instead, which is why pressing it lands in account management.
The profile itself is not gone though: profile.xboxlive.com still serves it.

    python tools/fetch_profile.py
    python tools/fetch_profile.py --dry-run

Written as one category with the details under it, the same shape Recent Players
and the inbox use, so the dashboard opens it with machinery that already works.

A note on whose profile this is. The dashboard shows the staged local profile
that boots with it; this reads the account you signed in to Xbox Live with, and
the two are different people with different gamerscores. What lands on the page
is the live one, because that is the profile with a bio and a location to show.

Everything written is a plain text file plus PNGs. The dashboard reads them off
disk and never touches the network.
"""
import argparse
import shutil
import subprocess
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
from fetch_marketplace import CARD_H, CARD_W, ascii_safe, fit_to_card  # noqa: E402

SETTINGS = ("Gamertag,GameDisplayName,GameDisplayPicRaw,Gamerscore,AccountTier,"
            "XboxOneRep,Location,Bio,RealName,TenureLevel,PreferredColor")
PROFILE = "https://profile.xboxlive.com/users/xuid(%s)/profile/settings?settings=" + SETTINGS
SOCIAL = "https://social.xboxlive.com/users/xuid(%s)/summary"

# The console's own green, for a detail that has no picture of its own. A tile
# with no picture at all is not an option -- the category page faults on open
# when one reaches it.
XBOX_GREEN = (0x10, 0x7C, 0x10)


def commas(value):
    """39700 -> '39,700'. The gamercard never printed a bare number."""
    try:
        return "{:,}".format(int(str(value).strip()))
    except (TypeError, ValueError):
        return str(value or "")


def flat_card(path, rgb):
    """A plain card in one colour, for a detail with no art behind it."""
    if path.exists():
        return True
    try:
        from PIL import Image
        Image.new("RGB", (CARD_W, CARD_H), rgb).save(str(path))
        return True
    except ImportError:
        pass
    if not shutil.which("ffmpeg"):
        return False
    spec = "color=c=0x%02X%02X%02X:s=%dx%d" % (rgb[0], rgb[1], rgb[2], CARD_W, CARD_H)
    r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi", "-i", spec,
                        "-frames:v", "1", str(path)], capture_output=True)
    return r.returncode == 0 and path.exists()


def gamerpic(url):
    """The picture ladder a gamerpic wants; a 360-era one needs both dimensions."""
    if not url:
        return b""
    sep = "&" if "?" in url else "?"
    for candidate in (url + sep + "w=424&h=424", url + sep + "w=424", url):
        try:
            r = requests.get(candidate, timeout=40)
            if r.status_code == 200 and r.content:
                return r.content
        except Exception:
            pass
    return b""


def preferred_rgb(url):
    """The account's own colour, so the detail cards are not all Xbox green."""
    if not url:
        return XBOX_GREEN
    try:
        r = requests.get(url, timeout=20)
        if r.status_code != 200:
            return XBOX_GREEN
        hexed = str(r.json().get("primaryColor") or "").strip().lstrip("#")
        if len(hexed) == 6:
            return tuple(int(hexed[i:i + 2], 16) for i in (0, 2, 4))
    except Exception:
        pass
    return XBOX_GREEN


def details(s, social):
    """The rows under the heading: (label, value), skipping what is not set.

    Order follows the 360's own gamercard -- score, then reputation and
    membership, then the things a person wrote about themselves.
    """
    rows = []
    if s.get("Gamerscore"):
        rows.append(("Gamerscore", commas(s["Gamerscore"]) + " G"))
    if s.get("XboxOneRep"):
        # 'GoodPlayer' -> 'Good Player'
        rep = "".join((" " + c) if c.isupper() else c for c in s["XboxOneRep"]).strip()
        rows.append(("Reputation", rep))
    if s.get("AccountTier"):
        rows.append(("Membership", s["AccountTier"]))
    # TenureLevel comes back 0 on this account, and "0 years on Xbox Live" is
    # worse than saying nothing, so it is only shown when the service knows.
    try:
        tenure = int(str(s.get("TenureLevel") or "0").strip())
    except ValueError:
        tenure = 0
    if tenure > 0:
        rows.append(("Member since",
                     "%s year%s on Xbox Live" % (commas(tenure), "" if tenure == 1 else "s")))
    if s.get("RealName"):
        rows.append(("Name", s["RealName"]))
    if s.get("Location"):
        rows.append(("Location", s["Location"]))
    if s.get("Bio"):
        rows.append(("Bio", " ".join(s["Bio"].split())))
    if social:
        followers = social.get("targetFollowerCount", 0)
        rows.append(("Friends", "%s following, %s follower%s"
                     % (commas(social.get("targetFollowingCount", 0)),
                        commas(followers), "" if followers == 1 else "s")))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--force", action="store_true", help="re-download pictures already present")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    auth, xuid = xbl_auth.sign_in()
    r = xbl_auth.get(PROFILE % xuid, auth, contract="3")
    if r.status_code != 200:
        raise SystemExit("the profile service refused (%d): %s" % (r.status_code, r.text[:200]))
    s = {x["id"]: x["value"] for x in r.json()["profileUsers"][0]["settings"]}

    social = {}
    try:
        rs = xbl_auth.get(SOCIAL % xuid, auth, contract="2")
        if rs.status_code == 200:
            social = rs.json()
    except Exception:
        pass

    tag = s.get("Gamertag") or s.get("GameDisplayName") or "Profile"
    rows = details(s, social)
    print("  %s -- %d detail(s)" % (ascii_safe(tag), len(rows)))
    for label, value in rows:
        print("    %-14s %s" % (label, ascii_safe(value)[:60]))

    if args.dry_run:
        return 0

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "profile"
    imgdir.mkdir(parents=True, exist_ok=True)

    # The gamerpic, which the heading tile and the identity row both wear.
    face = imgdir / (safe_name(tag) + ".png")
    if args.force or not face.exists():
        art = gamerpic(s.get("GameDisplayPicRaw"))
        if art:
            tile, _ = fit_to_card(art)
            face.write_bytes(tile)

    card = imgdir / "detail.png"
    flat_card(card, preferred_rgb(s.get("PreferredColor")))

    face_rel = "images/profile/" + face.name if face.exists() else ""
    card_rel = "images/profile/" + card.name if card.exists() else ""
    if not face_rel and not card_rel:
        raise SystemExit("no picture could be made; the page would fault with none")

    heading = commas(s["Gamerscore"]) + " G" if s.get("Gamerscore") else tag
    out = [
        "# Written by tools/fetch_profile.py -- edit freely, it is only a text file.",
        "# Your Xbox profile, from profile.xboxlive.com.",
        "# label | value | image | id | kind ('category' marks the heading)",
        "",
        "Profile|%s|%s||category" % (heading, face_rel or card_rel),
        "%s|%s|%s||" % (tag, s.get("Location") or "Xbox Live", face_rel or card_rel),
    ]
    # Following and Followers, when tools/fetch_social.py has written them.
    #
    # These take the place of the plain "Friends" row, which states the same two
    # numbers and cannot be pressed. Without those files the row stays as it was:
    # the counts come free with the profile summary, so they are worth showing
    # even when there is nothing behind them yet.
    social_links = []
    for filename, heading in (("following.txt", "Following"),
                              ("followers.txt", "Followers")):
        path = gamedir / filename
        if not path.exists():
            continue
        count = sum(1 for line in path.read_text(encoding="utf-8").splitlines()
                    if line.strip() and not line.startswith("#")
                    and not line.rstrip().endswith("|category"))
        if not count:
            continue
        # The heading card wears the faces behind it; fall back to the profile
        # picture, because a tile with no art at all faults the page on open.
        card = gamedir / "images" / "social" / ("cat_%s.png" % heading)
        art = ("images/social/cat_%s.png" % heading) if card.exists() else (
            face_rel or card_rel)
        social_links.append(
            "%s|%d %s|%s|%s:%s|link"
            % (heading, count, "person" if count == 1 else "people", art,
               heading.lower(), heading))

    if social_links:
        rows = [r for r in rows if r[0] != "Friends"]

    out += ["%s|%s|%s||" % (label, value.replace("|", "/"), card_rel or face_rel)
            for label, value in rows]
    out += social_links

    # Change Gamer Picture, when it has been imported. A 'link' row is a tile that
    # opens another page of ours rather than stating a fact -- the only thing on
    # this page you can actually do.
    picks = gamedir / "gamerpics.txt"
    if picks.exists():
        count = sum(1 for line in picks.read_text(encoding="utf-8").splitlines()
                    if line.strip() and not line.startswith("#")
                    and line.rstrip().endswith("|gamerpic"))
        if count:
            out.append("Change Gamer Picture|%d to choose from|%s|gamerpic:Gamer Picture|link"
                       % (count, face_rel or card_rel))

    (gamedir / "profile.txt").write_text("\n".join(out) + "\n", encoding="utf-8")
    print()
    print("  wrote %s" % (gamedir / "profile.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
