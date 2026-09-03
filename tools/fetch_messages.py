#!/usr/bin/env python3
"""Write gamedir/messages.txt: your Xbox Live inbox.

The Messages item on the gamer blade calls XamShowMessagesUI, which is XAM's own
Guide blade and cannot be opened here. The service behind it is still up though,
so the messages can be shown without it.

Two services exist and only one of them is the real inbox:

    msg.xboxlive.com          the legacy one. Returns service mail only -- one
                              item here, and no folders or sent endpoint.
    xblmessaging.xboxlive.com the current one. Returns the actual conversations,
                              read state, and each message's parts.

This reads the second. A conversation's last message is what the tile shows,
which is what a message list is: who it is from, when, and enough of it to
recognise.

    python tools/fetch_messages.py
    python tools/fetch_messages.py --limit 20
    python tools/fetch_messages.py --dry-run

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
from fetch_marketplace import (CARD_H, CARD_W, ascii_safe,  # noqa: E402
                               fit_to_card, make_category_card)

INBOX = "https://xblmessaging.xboxlive.com/network/Xbox/users/xuid(%s)/inbox"
PROFILE = ("https://profile.xboxlive.com/users/xuid(%s)/profile/settings"
           "?settings=Gamertag,GameDisplayPicRaw")

MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")


def when(stamp):
    """'2026-08-13T09:11:31Z' -> '13 Aug'. Empty when it will not parse."""
    text = str(stamp or "")
    if len(text) < 10:
        return ""
    try:
        return "%d %s" % (int(text[8:10]), MONTHS[int(text[5:7]) - 1])
    except (ValueError, IndexError):
        return ""


def preview(text, room=58):
    """The message, short enough for a tile's second line.

    A slot draws one small line, so this is a preview and not the message. Cut on
    a word so it does not end mid-syllable.
    """
    flat = " ".join(str(text or "").split())
    if len(flat) <= room:
        return flat
    cut = flat.rfind(" ", 0, room)
    return flat[:cut if cut > room // 2 else room].rstrip() + "..."


def parts_of(conversation):
    """The last message's parts: its text, and its first image if it has one."""
    last = conversation.get("lastMessage") or {}
    content = ((last.get("contentPayload") or {}).get("content") or {})
    text, image = "", ""
    for part in content.get("parts") or []:
        kind = part.get("contentType")
        if kind == "text" and not text:
            text = part.get("text") or ""
        elif kind == "image" and not image:
            image = part.get("downloadUri") or ""
    return text, image


def other_party(conversation, me):
    """Who the conversation is with. '0' is the service itself."""
    for xuid in conversation.get("participants") or []:
        if str(xuid) != str(me):
            return str(xuid)
    return "0"


def profile_of(xuid, auth, cache):
    """(gamertag, gamerpic url) for a sender, looked up once each."""
    if xuid in cache:
        return cache[xuid]
    result = ("", "")
    if xuid and xuid != "0":
        try:
            r = xbl_auth.get(PROFILE % xuid, auth, contract="3")
            if r.status_code == 200:
                settings = {s["id"]: s["value"]
                            for s in r.json()["profileUsers"][0]["settings"]}
                result = (settings.get("Gamertag", ""), settings.get("GameDisplayPicRaw", ""))
        except Exception:
            pass
    cache[xuid] = result
    return result


def download(url):
    if not url:
        return b""
    try:
        r = requests.get(url, timeout=40)
        if r.status_code == 200 and r.content:
            return r.content
    except Exception:
        pass
    return b""


def gamerpic(url):
    """The picture ladder a gamerpic wants; a 360-era one needs both dimensions."""
    if not url:
        return b""
    sep = "&" if "?" in url else "?"
    for candidate in (url + sep + "w=424&h=424", url + sep + "w=424", url):
        art = download(candidate)
        if art:
            return art
    return b""


def system_card(path):
    """A plain card for a message whose sender has no picture.

    Service messages come from participant "0", so there is nothing to show --
    and a tile with no picture at all is not an option: the category page crashes
    on open when one reaches it. A flat card in the console's own green is honest
    about being a system message and costs nothing.
    """
    if path.exists():
        return True
    if not shutil.which("ffmpeg"):
        return False
    r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi",
                        "-i", "color=c=0x107C10:s=%dx%d" % (CARD_W, CARD_H),
                        "-frames:v", "1", str(path)], capture_output=True)
    return r.returncode == 0 and path.exists()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--limit", type=int, default=40,
                    help="how many conversations to write; a channel holds 64 slots")
    ap.add_argument("--force", action="store_true", help="re-download pictures already present")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    auth, xuid = xbl_auth.sign_in()
    r = xbl_auth.get(INBOX % xuid, auth, contract="1")
    if r.status_code != 200:
        raise SystemExit("the message service refused (%d): %s" % (r.status_code, r.text[:200]))
    folder = r.json().get("primary") or {}
    conversations = folder.get("conversations") or []
    unread = folder.get("unreadCount", 0)
    print("  %d conversation(s), %d unread" % (folder.get("totalCount", len(conversations)),
                                               unread))

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "messages"
    if not args.dry_run:
        imgdir.mkdir(parents=True, exist_ok=True)

    try:
        from PIL import Image
        composer = ("pillow", Image)
    except ImportError:
        composer = ("ffmpeg", None) if shutil.which("ffmpeg") else ("none", None)

    rows, made, cache = [], [], {}
    for c in conversations[:args.limit]:
        who = other_party(c, xuid)
        tag, pic_url = profile_of(who, auth, cache)
        sender = tag or ("Xbox" if who == "0" else who)

        text, image_url = parts_of(c)
        date = when(c.get("timestamp"))
        line = preview(text) or ("Photo" if image_url else "")
        if not c.get("isRead"):
            line = ("New · " + line) if line else "New"
        elif date and line:
            line = "%s · %s" % (date, line)
        elif date:
            line = date

        rel = ""
        if not args.dry_run:
            name = safe_name(sender + "_" + str(c.get("conversationId") or "")[-8:]) + ".png"
            dest = imgdir / name
            if args.force or not dest.exists():
                # The photo in the message reads better than a gamerpic when
                # there is one; failing that the sender, failing that a card.
                art = download(image_url) or gamerpic(pic_url)
                if art:
                    tile, _ = fit_to_card(art)
                    dest.write_bytes(tile)
            if not dest.exists() and system_card(imgdir / "system.png"):
                dest = imgdir / "system.png"
            if dest.exists():
                rel = "images/messages/" + dest.name
                made.append(dest)

        rows.append((sender, line, rel, str(c.get("conversationId") or ""), ""))
        print("    %-18s %s" % (ascii_safe(sender)[:18], ascii_safe(line)[:58]))

    if not rows:
        print("  the inbox is empty; nothing written")
        return 0

    card_rel, card = "", imgdir / "cat_Messages.png"
    if not args.dry_run and made:
        if args.force or not card.exists():
            art = make_category_card(made, composer)
            if art:
                card.write_bytes(art)
        if card.exists():
            card_rel = "images/messages/" + card.name

    label = "%d message%s" % (len(rows), "" if len(rows) == 1 else "s")
    if unread:
        label = "%d unread of %s" % (unread, label)

    out = [
        "# Written by tools/fetch_messages.py -- edit freely, it is only a text file.",
        "# Your Xbox Live inbox, from xblmessaging.xboxlive.com.",
        "# sender | preview | image | conversation id | kind ('category' marks the heading)",
        "",
        "Messages|%s|%s||category" % (label, card_rel),
    ]
    out += ["|".join(str(x).replace("|", "/") for x in r) for r in rows]
    text = "\n".join(out) + "\n"

    if args.dry_run:
        print()
        print(ascii_safe(text))
        return 0

    (gamedir / "messages.txt").write_text(text, encoding="utf-8")
    print()
    print("  wrote %d message(s) to %s" % (len(rows), gamedir / "messages.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
