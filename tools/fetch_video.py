#!/usr/bin/env python3
"""Write gamedir/video.txt: films and TV for the Video & Music Marketplace.

Unlike the games side, nothing preserved the Xbox 360's video catalogue. The
marketplace archive that holds 72,337 game-side entries -- add-ons, themes,
demos, avatar items, gamer pictures -- carries no movie or TV content type at
all, and Microsoft's surviving catalogue will resolve a film by id but will not
be browsed: search returns nothing and the movie and TV listings are capped at
four titles each with no paging. The kiosk discs have game trailers, not a store.

So this fills the row from TMDB instead: real films and television with real
poster art, genres and years. It is a film database rather than Microsoft's
catalogue, and the titles here were not necessarily ever sold on Xbox -- that is
the trade, and it is the reason this is a separate tool with its own file rather
than something folded into the marketplace fetch.

    python tools/fetch_video.py --api-key <key>
    python tools/fetch_video.py                    # key from tools/.tmdb_key
    python tools/fetch_video.py --movies-only
    python tools/fetch_video.py --dry-run

A key is free: themoviedb.org -> sign up -> Settings -> API -> request an API
key ("Developer", personal use). Paste the v3 key into tools/.tmdb_key, or pass
--api-key. The key is only used here; the dashboard reads the text file this
writes and never touches the network. Poster art comes from TMDB's image CDN,
which needs no key at all.
"""
import argparse
import os
import sys
from pathlib import Path

try:
    import requests
except ImportError:  # pragma: no cover
    raise SystemExit("this needs 'requests':  python -m pip install requests")

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

# The card compositing is the marketplace tool's -- same 420x320 slot, so the
# video tiles sit on the shelf exactly like the game ones.
from fetch_marketplace import (CARD_H, CARD_W, ascii_safe,  # noqa: E402
                               make_category_card, make_tile, safe_name)

API = "https://api.themoviedb.org/3"
IMG = "https://image.tmdb.org/t/p/w500%s"
KEY_FILE = HERE / ".tmdb_key"
UA = {"User-Agent": "Mozilla/5.0", "Accept": "application/json"}


def read_key(arg):
    if arg:
        return arg.strip()
    env = os.environ.get("TMDB_API_KEY", "").strip()
    if env:
        return env
    if KEY_FILE.exists():
        return KEY_FILE.read_text(encoding="utf-8").strip()
    raise SystemExit(
        "no TMDB key.\n"
        "  Get one free: https://www.themoviedb.org/settings/api  (v3 key)\n"
        "  Then either put it in %s\n"
        "  or pass --api-key <key>." % KEY_FILE)


def api(path, key, **params):
    params["api_key"] = key
    try:
        r = requests.get(API + path, headers=UA, params=params, timeout=40)
    except Exception as e:
        raise SystemExit("TMDB unreachable: %s" % str(e)[:120])
    if r.status_code == 401:
        raise SystemExit("TMDB rejected that key (401). Check tools/.tmdb_key.")
    if r.status_code != 200:
        raise SystemExit("TMDB %s -> %d: %s" % (path, r.status_code, r.text[:160]))
    return r.json()


def fetch_bytes(url):
    try:
        r = requests.get(url, headers=UA, timeout=40)
        if r.status_code == 200 and r.content:
            return r.content
    except Exception:
        pass
    return b""


def year_of(item):
    d = item.get("release_date") or item.get("first_air_date") or ""
    return d[:4] if len(d) >= 4 else ""


def title_of(item):
    return (item.get("title") or item.get("name") or "").strip()


def mostly_latin(s):
    """The shell's fonts are Latin; a title it cannot draw is a row of blanks."""
    s = (s or "").strip()
    if not s:
        return False
    return sum(1 for c in s if ord(c) < 128) / len(s) > 0.9


def collect(key, kind, genres, want, pages, latin_only):
    """Popular titles per genre. Returns {genre name: [item, ...]}."""
    path = "/discover/movie" if kind == "movie" else "/discover/tv"
    out = {}
    for gid, gname in genres.items():
        picked, seen = [], set()
        for page in range(1, pages + 1):
            if len(picked) >= want:
                break
            data = api(path, key, with_genres=gid, sort_by="popularity.desc",
                       include_adult="false", page=page, language="en-US")
            for item in data.get("results", []):
                if len(picked) >= want:
                    break
                name = title_of(item)
                if not name or not item.get("poster_path"):
                    continue
                if latin_only and not mostly_latin(name):
                    continue
                if item["id"] in seen:
                    continue
                seen.add(item["id"])
                item["_kind"] = kind
                item["_genre"] = gname
                picked.append(item)
            if not data.get("results"):
                break
        if picked:
            out[gname] = picked
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--api-key", default="", help="TMDB v3 key (else tools/.tmdb_key)")
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--per-category", type=int, default=12,
                    help="titles under each genre (default 12)")
    ap.add_argument("--max-tiles", type=int, default=400,
                    help="total rows written, headings included")
    ap.add_argument("--pages", type=int, default=2, help="TMDB pages per genre")
    ap.add_argument("--movies-only", action="store_true", help="skip television")
    ap.add_argument("--tv-only", action="store_true", help="skip films")
    ap.add_argument("--any-script", action="store_true",
                    help="include titles the dashboard's Latin fonts cannot draw")
    ap.add_argument("--force", action="store_true", help="re-download art already present")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    key = read_key(args.api_key)
    latin_only = not args.any_script

    try:
        from PIL import Image
        composer = ("pillow", Image)
    except ImportError:
        import shutil
        composer = ("ffmpeg", None) if shutil.which("ffmpeg") else ("none", None)

    buckets = {}
    if not args.tv_only:
        g = {x["id"]: x["name"] for x in api("/genre/movie/list", key)["genres"]}
        print("  %d film genre(s)" % len(g))
        for name, items in collect(key, "movie", g, args.per_category, args.pages,
                                   latin_only).items():
            buckets.setdefault(name, []).extend(items)
    if not args.movies_only:
        g = {x["id"]: x["name"] for x in api("/genre/tv/list", key)["genres"]}
        print("  %d television genre(s)" % len(g))
        for name, items in collect(key, "tv", g, args.per_category, args.pages,
                                   latin_only).items():
            buckets.setdefault(name, []).extend(items)

    if not buckets:
        raise SystemExit("TMDB returned nothing")

    # Biggest genres first, the way the marketplace row opens on its largest.
    order = sorted(buckets, key=lambda n: (-len(buckets[n]), n))

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "video"
    if not args.dry_run:
        imgdir.mkdir(parents=True, exist_ok=True)

    rows, composited, cards, budget = [], 0, 0, args.max_tiles
    for gname in order:
        items = buckets[gname]
        if budget < 2:
            break
        take = items[:min(len(items), budget - 1)]
        cat_at = len(rows)          # the heading goes here once its card exists
        budget -= 1
        print("  [%s]" % ascii_safe(gname))

        made = []
        for item in take:
            name = title_of(item)
            ident = "%s%d" % ("m" if item["_kind"] == "movie" else "t", item["id"])
            rel = "images/video/%s.png" % ident
            dest = imgdir / ("%s.png" % ident)

            if not args.dry_run and (args.force or not dest.exists()):
                art = fetch_bytes(IMG % item["poster_path"])
                if not art:
                    print("    %-38s no poster; skipped" % ascii_safe(name[:38]))
                    continue
                tile, did = make_tile(art, composer)
                if did:
                    composited += 1
                dest.write_bytes(tile)

            made.append(dest)
            year = year_of(item)
            what = "Film" if item["_kind"] == "movie" else "TV"
            sub = "%s · %s" % (what, year) if year else what
            rows.append((name, sub, rel, ident, ""))
            budget -= 1
            print("    %-38s %s" % (ascii_safe(name[:38]), ascii_safe(sub)))

        # The genre's own tile, built from the posters filed under it.
        cat_rel, cat_file = "", imgdir / ("cat_%s.png" % safe_name(gname))
        if not args.dry_run and made:
            if args.force or not cat_file.exists():
                card = make_category_card(made, composer)
                if card:
                    cat_file.write_bytes(card)
                    cards += 1
            if cat_file.exists():
                cat_rel = "images/video/%s" % cat_file.name
        elif args.dry_run:
            cat_rel = "images/video/%s" % cat_file.name

        rows.insert(cat_at, (gname, "%d title%s" % (len(items), "" if len(items) == 1 else "s"),
                             cat_rel, "", "category"))

    out = [
        "# Written by tools/fetch_video.py -- edit freely, it is only a text file.",
        "# Films and television from TMDB (themoviedb.org), with its poster art.",
        "# The Xbox 360's own video catalogue was never preserved and Microsoft's",
        "# surviving one cannot be browsed, so this stands in for it.",
        "# name | subtitle | image | id | kind ('category' marks a heading)",
        "",
    ]
    out += ["%s|%s|%s|%s|%s" % r for r in rows]
    text = "\n".join(out) + "\n"

    if args.dry_run:
        print()
        print(ascii_safe(text))
        return 0

    (gamedir / "video.txt").write_text(text, encoding="utf-8")
    print()
    print("  wrote %d row(s) to %s" % (len(rows), gamedir / "video.txt"))
    print("  tile art in %s" % imgdir)
    if composer[0] == "none":
        print("  Neither Pillow nor ffmpeg is here, so raw posters were written and the")
        print("  shell will stretch them. Install either, then re-run with --force.")
    else:
        print("  %d composited onto %dx%d cards via %s, plus %d category card(s)"
              % (composited, CARD_W, CARD_H, composer[0], cards))
    return 0


if __name__ == "__main__":
    sys.exit(main())
