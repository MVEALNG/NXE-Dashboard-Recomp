#!/usr/bin/env python3
"""Write gamedir/friends.txt from your Xbox LIVE friends list.

The Xbox PC app was the obvious place to read this from, and it is not one. Its
AsyncCache.db holds several thousand rows of store, library and achievement data
and no social graph at all, and a search of every Microsoft.Xbox* package on the
machine -- in both ASCII and UTF-16 -- finds not one gamertag from the friends
list. Game Bar fetches people from peoplehub.xboxlive.com when it draws the panel
and keeps them in memory. There is nothing cached to import.

So this asks Xbox LIVE directly, the same way every other third-party Xbox tool
does, and writes the result to the plain file the dashboard reads. The dashboard
itself never talks to the network and never sees a token; it only ever reads
friends.txt, so it keeps working with no account at all.

    python tools/fetch_friends.py --all      # sign in in a browser, write the file
    python tools/fetch_friends.py --dry-run  # sign in and print, write nothing

Sign-in shows an eight-character code to type at microsoft.com/link. Nothing is
copied out of a browser and no password is handled here. No application
registration is needed -- it uses the public client id Microsoft's own Xbox apps
use. The refresh token is cached in tools/.xbl_token.json so later runs are
silent; delete that file to sign out.

--client-id switches to Microsoft's device-code flow against an application you
registered yourself, for anyone who would rather not use the public one.
"""
import argparse
import json
import io
import os
import re
import sys
import time
import urllib.parse
from pathlib import Path

try:
    import requests
except ImportError:
    print("this needs 'requests':  python -m pip install requests", file=sys.stderr)
    raise SystemExit(1)

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

# One card size and one fitter across every row, so a gamerpic, a box art and
# a genre card all cost the same and sit the same.
from fetch_marketplace import fit_to_card  # noqa: E402
TOKEN_CACHE = HERE / ".xbl_token.json"

DEVICECODE = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode"
TOKEN = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token"
SCOPE = "XboxLive.signin offline_access"

# The default sign-in needs no app registration and no copying.
#
# login.live.com serves the device-code flow to the public client id Microsoft's
# own Xbox apps use: an eight-character code typed at microsoft.com/link. The
# --client-id path does the same thing against login.microsoftonline.com with an
# application you registered yourself.
LIVE_CLIENT_ID = "000000004C12AE6F"
LIVE_CONNECT = "https://login.live.com/oauth20_connect.srf"
LIVE_TOKEN = "https://login.live.com/oauth20_token.srf"
LIVE_SCOPE = "Xboxlive.signin Xboxlive.offline_access"

XBL_USER = "https://user.auth.xboxlive.com/user/authenticate"
XSTS = "https://xsts.auth.xboxlive.com/xsts/authorize"
# social + the presence decoration, which is what fills the second line.
PEOPLE = ("https://peoplehub.xboxlive.com/users/me/people/social"
          "/decoration/presenceDetail")

REGISTER_HELP = """
No client id. Register one once, then pass it with --client-id or set
XBOX_CLIENT_ID:

  1. https://portal.azure.com  ->  Microsoft Entra ID  ->  App registrations
  2. New registration. Any name. Under "Supported account types" pick
     "Personal Microsoft accounts only".
  3. Leave the redirect URI blank and create it.
  4. Open Authentication, and under "Advanced settings" turn
     "Allow public client flows" to Yes. Save.
  5. Copy the Application (client) ID from Overview.

The id is not a secret -- it names the application, not the account.
"""


def device_login(client_id: str) -> dict:
    """Microsoft device-code flow. Returns the token response."""
    r = requests.post(DEVICECODE, data={"client_id": client_id, "scope": SCOPE}, timeout=30)
    if r.status_code != 200:
        raise SystemExit("device code request refused (%d): %s\n"
                         "A 400 here usually means the app does not have "
                         "'Allow public client flows' enabled." % (r.status_code, r.text[:300]))
    d = r.json()
    print()
    print("  Sign in at: %s" % d["verification_uri"])
    print("  Enter code: %s" % d["user_code"])
    print()
    print("  Waiting for you to finish in the browser...")

    deadline = time.time() + int(d.get("expires_in", 900))
    interval = int(d.get("interval", 5))
    while time.time() < deadline:
        time.sleep(interval)
        t = requests.post(TOKEN, timeout=30, data={
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "client_id": client_id,
            "device_code": d["device_code"],
        })
        if t.status_code == 200:
            return t.json()
        err = t.json().get("error", "")
        if err == "authorization_pending":
            continue
        if err == "slow_down":
            interval += 5
            continue
        raise SystemExit("sign-in failed: %s" % t.text[:300])
    raise SystemExit("sign-in timed out")


def live_login() -> dict:
    """Device-code sign-in against login.live.com. No registration, no pasting.

    The first version of this used the desktop redirect flow, which ends on a
    page that says, in Microsoft's own words, "You have reached a page that is
    not normally shown. Microsoft will never ask you to copy or share this URL."
    That warning is right: the URL carries a live authorization code, and asking
    someone to copy it out of the address bar is indistinguishable from the
    phishing it exists to prevent. No tool should teach that habit.

    login.live.com/oauth20_connect.srf serves the device-code flow to the same
    public client id, so instead there is an eight-character code to type at
    microsoft.com/link. Nothing sensitive is ever displayed, copied or pasted.
    """
    r = requests.post(LIVE_CONNECT, timeout=30, data={
        "client_id": LIVE_CLIENT_ID,
        "scope": LIVE_SCOPE,
        "response_type": "device_code",
    })
    if r.status_code != 200:
        raise SystemExit("could not start sign-in (%d): %s" % (r.status_code, r.text[:300]))
    d = r.json()

    print()
    print("  Go to:   %s" % d.get("verification_uri", "https://www.microsoft.com/link"))
    print("  Enter:   %s" % d["user_code"])
    print()
    print("  Sign in and approve. Nothing needs to be copied back here.")
    print("  Waiting...", end="", flush=True)
    try:
        import webbrowser
        webbrowser.open(d.get("verification_uri", "https://www.microsoft.com/link"))
    except Exception:
        pass

    deadline = time.time() + int(d.get("expires_in", 900))
    interval = int(d.get("interval", 5))
    while time.time() < deadline:
        time.sleep(interval)
        print(".", end="", flush=True)
        t = requests.post(LIVE_TOKEN, timeout=30, data={
            "client_id": LIVE_CLIENT_ID,
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "device_code": d["device_code"],
        })
        if t.status_code == 200:
            print(" signed in.")
            out = t.json()
            out["_live"] = True
            return out
        try:
            err = t.json().get("error", "")
        except Exception:
            err = t.text[:120]
        if err in ("authorization_pending", "slow_down"):
            if err == "slow_down":
                interval += 5
            continue
        if err == "authorization_declined":
            raise SystemExit("\n  sign-in was declined")
        raise SystemExit("\n  sign-in failed: %s" % t.text[:300])
    raise SystemExit("\n  sign-in timed out")


def live_refresh(refresh_token: str):
    t = requests.post(LIVE_TOKEN, timeout=30, data={
        "client_id": LIVE_CLIENT_ID,
        "refresh_token": refresh_token,
        "grant_type": "refresh_token",
        "scope": LIVE_SCOPE,
    })
    if t.status_code != 200:
        return None
    d = t.json()
    d["_live"] = True
    return d


def refresh(client_id: str, refresh_token: str):
    t = requests.post(TOKEN, timeout=30, data={
        "grant_type": "refresh_token",
        "client_id": client_id,
        "refresh_token": refresh_token,
        "scope": SCOPE,
    })
    return t.json() if t.status_code == 200 else None


def xbox_authorize(ms_access_token: str, live_flow: bool):
    """Microsoft token -> XBL user token -> XSTS token. Returns the XBL3.0 header.

    The ticket is spelled differently depending on where the token came from: a
    login.live.com token goes in bare, an Azure v2 one has to be prefixed "d=".
    Getting this wrong is the usual cause of a bare 400 from user.auth.
    """
    # Which spelling Xbox wants is not something documentation settles, and a
    # rejection is a bare 401 with an empty body, so try the three that are
    # actually in use and report which one worked rather than guessing again.
    # "d=" is what both flows actually want -- the widespread claim that the
    # live.com flow takes the token bare is wrong, and believing it cost a 401
    # with an empty body and no clue in it. The others stay as fallbacks.
    candidates = [("d=", "d=" + ms_access_token),
                  ("bare", ms_access_token),
                  ("t=", "t=" + ms_access_token)]

    user_token = None
    problems = []
    for label, ticket in candidates:
        r = requests.post(XBL_USER, timeout=30, json={
            "Properties": {
                "AuthMethod": "RPS",
                "SiteName": "user.auth.xboxlive.com",
                "RpsTicket": ticket,
            },
            "RelyingParty": "http://auth.xboxlive.com",
            "TokenType": "JWT",
        }, headers={"x-xbl-contract-version": "1"})
        if r.status_code == 200:
            print("  RpsTicket format: %s" % label)
            user_token = r.json()["Token"]
            break
        # The useful detail is in a header, not the body: XErr carries the real
        # reason and the body is usually empty.
        xerr = r.headers.get("X-Err") or r.headers.get("WWW-Authenticate") or ""
        problems.append("%s -> %d %s" % (label, r.status_code, (xerr or r.text[:120]).strip()))
    if not user_token:
        raise SystemExit("Xbox user auth failed; tried:\n    " + "\n    ".join(problems))

    r = requests.post(XSTS, timeout=30, json={
        "Properties": {"SandboxId": "RETAIL", "UserTokens": [user_token]},
        "RelyingParty": "http://xboxlive.com",
        "TokenType": "JWT",
    }, headers={"x-xbl-contract-version": "1"})
    if r.status_code != 200:
        # 2148916233 is "this account has no Xbox profile", which is its own
        # problem and worth saying plainly rather than as a raw status.
        if "2148916233" in r.text:
            raise SystemExit("that Microsoft account has no Xbox profile on it")
        raise SystemExit("XSTS failed (%d): %s" % (r.status_code, r.text[:300]))
    d = r.json()
    return "XBL3.0 x=%s;%s" % (d["DisplayClaims"]["xui"][0]["uhs"], d["Token"])


# How an offline friend's tile is drawn.
#
# The blade did not badge the people who were online, it dimmed the ones who
# were not -- and grouped the online ones first, which the sort in main() now
# does. A green dot would read as current-Xbox language on a 2009 dashboard, so
# the difference is carried by the tile itself: most of the colour out, some of
# the brightness with it. Enough to tell the two apart across a row at a glance
# without any one tile looking broken or unfinished.
#
# Alpha is lifted out and put back because the enhancers work on RGB: run them
# over RGBA and the transparency gets dimmed along with everything else, which
# on a gamerpic with a cut-out edge shows as a grey halo against the card.
GREY = 0.75  # how much colour to take out, 1.0 being fully grey
DIM = 0.72   # what is left of the brightness


def dim_offline(png: bytes) -> bytes:
    """An offline friend's tile: desaturated and darkened.

    Hands the picture back untouched if it cannot be processed, the same
    bargain fit_to_card makes -- a friend with an undimmed tile is a cosmetic
    miss, a friend with no tile at all is dropped from the row entirely.
    """
    try:
        from PIL import Image, ImageEnhance
    except ImportError:
        return png
    try:
        src = Image.open(io.BytesIO(png)).convert("RGBA")
        alpha = src.getchannel("A")
        out = ImageEnhance.Color(src.convert("RGB")).enhance(1.0 - GREY)
        out = ImageEnhance.Brightness(out).enhance(DIM).convert("RGBA")
        out.putalpha(alpha)
        buf = io.BytesIO()
        out.save(buf, "PNG")
        return buf.getvalue()
    except Exception:
        return png


def is_online(p: dict) -> bool:
    return (p.get("presenceState") or "").lower() == "online"


def safe_name(gamertag: str) -> str:
    s = re.sub(r"[^A-Za-z0-9_-]+", "_", gamertag).strip("_")
    return s or "friend"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--client-id", default=os.environ.get("XBOX_CLIENT_ID"),
                    help="optional: use the device-code flow with your own registered "
                         "application instead of the default browser sign-in")
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"),
                    help="game directory holding friends.txt and images/")
    ap.add_argument("--all", action="store_true",
                    help="include offline friends (default: online only)")
    ap.add_argument("--limit", type=int, default=50, help="maximum friends to write")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    # Default to the flow that needs no registration; --client-id opts into
    # device code against your own application instead.
    use_live = not args.client_id

    tok = None
    if TOKEN_CACHE.exists():
        try:
            cached = json.loads(TOKEN_CACHE.read_text())
            rt = cached.get("refresh_token")
            if rt and cached.get("live", True) == use_live:
                tok = live_refresh(rt) if use_live else refresh(args.client_id, rt)
                if tok:
                    print("  signed in from the cached token")
        except Exception:
            tok = None
    if not tok:
        tok = live_login() if use_live else device_login(args.client_id)

    live_flow = bool(tok.get("_live"))
    if not args.dry_run and tok.get("refresh_token"):
        TOKEN_CACHE.write_text(json.dumps(
            {"refresh_token": tok["refresh_token"], "live": live_flow}))
        try:
            os.chmod(TOKEN_CACHE, 0o600)
        except Exception:
            pass

    auth = xbox_authorize(tok["access_token"], live_flow)

    r = requests.get(PEOPLE, timeout=30, headers={
        "Authorization": auth,
        "x-xbl-contract-version": "5",
        "Accept-Language": "en-US",
    })
    if r.status_code != 200:
        raise SystemExit("peoplehub refused (%d): %s" % (r.status_code, r.text[:300]))
    people = r.json().get("people", [])
    print("  %d friend(s) on the account" % len(people))

    # Online first, the way the blade ordered them.
    #
    # This also decides who survives --limit. The write loop stops as soon as it
    # has `limit` rows, so ordering afterwards would be too late: an account with
    # more offline friends than the limit could show none of the people actually
    # online, which is the one thing a friends list is for. Stable, so peoplehub's
    # own ordering still decides within each group.
    people.sort(key=lambda p: 0 if is_online(p) else 1)

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "friends"
    if not args.dry_run:
        imgdir.mkdir(parents=True, exist_ok=True)

    rows = []
    for p in people:
        online = is_online(p)
        if not args.all and not online:
            continue
        tag = p.get("gamertag") or ""
        if not tag:
            continue
        # presenceText is what the Xbox app shows: "Online - Halo 3", "Last seen
        # 2d ago: Minecraft", and so on.
        presence = (p.get("presenceText") or "").strip()
        pic = p.get("displayPicRaw") or ""
        rel = ""
        if pic:
            # 424 is the size the console-era tiles used and is plenty for a
            # 420x320 card, but the image service is picky about how it is asked.
            # A legacy Xbox 360 gamerpic -- isXbox360Gamerpic -- wants width and
            # height together and answers w=424 alone, or the bare URL, with
            # "Error occured while processing request." So ask for both first and
            # fall back. Two of sixteen friends here are 360-era, and they were
            # the ones silently dropped before this.
            sep = "&" if "?" in pic else "?"
            urls = [pic + sep + "w=424&h=424", pic + sep + "w=424", pic]
            name = safe_name(tag) + ".png"
            dest = imgdir / name
            if not args.dry_run:
                for u in urls:
                    try:
                        img = requests.get(u, timeout=30)
                        if img.status_code == 200 and img.content:
                            # Fitted, not raw: a 424x424 gamerpic is the wrong shape
                            # for a 4:3 card, and sixteen of them are 11MB decoded
                            # against a ceiling near 84MB shared with the marketplace
                            # rows -- which is what left tiles blank before.
                            tile, _ = fit_to_card(img.content)
                            if not online:
                                tile = dim_offline(tile)
                            dest.write_bytes(tile)
                            rel = "images/friends/" + name
                            break
                    except Exception as e:
                        print("    (gamerpic for %s: %s)" % (tag, e), file=sys.stderr)
                if not rel:
                    print("    (no gamerpic for %s; skipped)" % tag, file=sys.stderr)
            else:
                rel = "images/friends/" + name
        if not rel:
            continue  # a tile with no picture draws as a blank card
        rows.append((tag, presence, rel, str(p.get("xuid") or ""),
                     "online" if online else ""))
        if len(rows) >= args.limit:
            break

    out = [
        "# Written by tools/fetch_friends.py -- edit freely, it is only a text file.",
        "# gamertag | presence | image | xuid | online",
        "",
    ]
    out += ["%s|%s|%s|%s|%s" % r for r in rows]
    text = "\n".join(out) + "\n"

    if args.dry_run:
        print(text)
        return 0
    (gamedir / "friends.txt").write_text(text, encoding="utf-8")
    print("  wrote %d friend(s) to %s" % (len(rows), gamedir / "friends.txt"))
    print("  gamerpics in %s" % imgdir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
