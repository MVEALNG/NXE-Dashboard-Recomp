#!/usr/bin/env python3
"""Write gamedir/marketplace.txt: the real Xbox 360 Game Marketplace, as tiles.

The Xbox 360 storefront was shut down on 29 July 2024 and there is nothing left
to call. Every EDS host answers 404 -- marketplace.xboxlive.com, catalog.xboxlive
.com, eds.xboxlive.com -- and the modern catalogue holds no 360 entries: ask
displaycatalog to look up Halo 3's title id (1297287142) as an XboxTitleId and it
returns zero products. The live marketplace cannot be imported, because it is no
longer there to import.

What does still exist is the catalogue itself, preserved. This reads x360db, a
recreation of the Xbox Marketplace database covering ~5,900 titles with their
real title ids, genres, publishers and release dates, and pulls each game's real
360 box art -- the same 219x300 boxartlg.jpg the marketplace served, from
Microsoft's own download.xbox.com, which is still up.

    python tools/fetch_marketplace.py                    # top rated, 20 tiles
    python tools/fetch_marketplace.py --sort newest
    python tools/fetch_marketplace.py --genre Shooter --limit 12
    python tools/fetch_marketplace.py --search halo
    python tools/fetch_marketplace.py --list-genres
    python tools/fetch_marketplace.py --dry-run

Everything written is a plain text file plus PNGs. The dashboard reads them off
disk and never touches the network itself.

Tile art
--------
Box art is portrait (219x300) and a slot card is landscape (420x320), so the art
is centred on a transparent card rather than stretched to fit -- which is what
the real marketplace did too. Pillow does that compositing when it is installed
and ffmpeg does it otherwise; both are optional, and with neither the raw box art
is written and the shell stretches it.
"""
import argparse
import hashlib
import io
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    import requests
except ImportError:  # pragma: no cover
    raise SystemExit("this needs 'requests':  python -m pip install requests")

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

X360DB = "https://xenia-manager.github.io/x360db/games.json"
# Per-title metadata: developer, publisher, release date, rating, and the store
# copy the marketplace itself ran. One request per game that makes the row.
INFO = "https://xenia-manager.github.io/x360db/titles/%s/info.json"
# The 360's own art CDN, still serving by title id. Only 64x64, so it is a last
# resort for the few entries whose boxart url has rotted.
ICON = "http://image.xboxlive.com/global/t.%s/icon/0/8000"

UA = {"User-Agent": "Mozilla/5.0"}

# A slot card is 420x320, but these are written at half that on purpose.
#
# Every category page's pictures are held decoded at once, and there are close to
# 500 of them across the two marketplaces. At 420x320 RGBA that is about 260MB
# and the loader starts refusing at roughly 84MB -- 0x922D99A8 hands back
# 0x8000FFFF and the rest of the tiles stay blank forever. At 210x160 the whole
# set fits with room to spare and every tile draws. The shell scales them up to
# the card, so a tile is softer than it would be, which is the price of all of
# them being there at all.
CARD_W, CARD_H = 210, 160

# x360db files trailers, music videos and avatar items alongside the games.
# The Game Marketplace channel is for games.
NOT_GAMES = {
    "Movie Trailers", "Movie Trailers and Short Films", "TV Shows",
    "Music Videos", "Music and Short Videos", "Independent & Music Videos",
    "Behind the Scenes", "Gaming Community Videos", "Game Tips and Support Videos",
    "More Entertainment", "Avatar", "Xbox LIVE Marketplace",
}

# The eight genres the Xbox 360 actually shipped category art for.
#
# Experience Disc 1.6 carries mta-ge-*.png -- 352x198 cards, a game screenshot
# with the genre's glyph in the corner -- and this is the whole set that exists:
# Action, ActionAdventure, Adventure, OriginalXboxGames, Racing, RolePlaying,
# RPG, Sports, SportsAndRacing, Strategy. RPG and SportsAndRacing are second
# cards for genres already covered, so eight genres remain.
#
# The row is built from those eight rather than from the catalogue's own
# eighteen, so every category on it wears real Xbox artwork. x360db's genres are
# folded in below; the mapping is a judgement call in places -- a platformer is
# filed under Action & Adventure, and Adventure carries the puzzle, family and
# oddments that have nowhere better to go -- but nothing is dropped, every game
# still lands on a shelf.
XBOX_GENRES = ["Action", "Action & Adventure", "Adventure", "Racing", "Role Playing",
               "Sports", "Strategy", "Original Xbox Games"]

GENRE_MAP = {
    "Shooter": "Action",
    "Fighting": "Action",
    "Action": "Action",
    "Action & Adventure": "Action & Adventure",
    "Platformer": "Action & Adventure",
    "Kinect": "Action & Adventure",
    "Puzzle & Trivia": "Adventure",
    "Family": "Adventure",
    "Educational": "Adventure",
    "Music": "Adventure",
    "Other": "Adventure",
    "Racing & Flying": "Racing",
    "Role Playing": "Role Playing",
    "Sports & Recreation": "Sports",
    "Sports": "Sports",
    "Strategy & Simulation": "Strategy",
    "Card & Board": "Strategy",
    "Classics": "Original Xbox Games",
    "Xbox 360 Exclusives": "Original Xbox Games",
}

YEAR = re.compile(r"(19|20)\d{2}")


def safe_name(s):
    """A genre turned into a filename. "Sci-Fi & Fantasy" is not one."""
    return re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_") or "category"


def ascii_safe(s):
    """Console-safe echo. The terminal here is cp1252 and the titles are not."""
    return s.encode("ascii", "replace").decode("ascii")


def mostly_latin(title):
    """The shell's fonts are the Latin XenonCLatin set.

    A Japanese or Cyrillic title is in the archive for good reason but renders as
    a row of blanks on this dashboard, so those are left out by default rather
    than shown broken. --any-script puts them back.
    """
    t = (title or "").strip()
    if not t:
        return False
    return sum(1 for c in t if ord(c) < 128) / len(t) > 0.9


def year_of(g):
    m = YEAR.search(g.get("release_date") or "")
    return m.group(0) if m else ""


def rating_of(g):
    try:
        return float(g.get("user_rating") or 0)
    except (TypeError, ValueError):
        return 0.0


def subtitle_for(g):
    """What goes on the second line of the tile.

    The real marketplace put a price here. This database has no prices, and
    inventing them would be worse than leaving them out, so the tile carries what
    is actually known about the game: its genre and the year it came out.
    """
    genres = [x for x in (g.get("genre") or []) if x not in NOT_GAMES]
    bits = ", ".join(genres[:2])
    year = year_of(g)
    if bits and year:
        return "%s · %s" % (bits, year)
    return bits or year or (g.get("publisher") or "").strip()


def details_for(hexid):
    """The marketplace's own words about a game, for its detail page.

    games.json has enough to draw a tile; this has what a product page needs --
    who made it, who published it, when it came out, and the description
    Microsoft's storefront actually showed. Missing entries are not an error,
    they just leave the page shorter.
    """
    try:
        r = requests.get(INFO % hexid, headers=UA, timeout=30)
        if r.status_code != 200:
            return {}
        d = r.json()
    except Exception:
        return {}
    desc = d.get("description") or {}
    text = (desc.get("short") or desc.get("full") or "").strip()
    # The full copy runs to legal boilerplate and store notices; a page wants the
    # first couple of sentences, not the refund policy.
    text = " ".join(text.split())
    # A Japanese description is real, but the shell's fonts are the Latin
    # XenonCLatin set and it would draw as a row of blanks. Better an absent
    # card than an empty one.
    if text and not mostly_latin(text):
        text = ""
    if len(text) > 300:
        cut = text.rfind(". ", 0, 300)
        text = text[:cut + 1] if cut > 120 else text[:297].rstrip() + "..."
    return {
        "developer": (d.get("developer") or "").strip(),
        "publisher": (d.get("publisher") or "").strip(),
        "released": (d.get("release_date") or "").strip(),
        "rating": (d.get("user_rating") or "").strip(),
        "description": text,
    }


def fetch(url, timeout=40):
    try:
        r = requests.get(url, headers=UA, timeout=timeout)
        if r.status_code == 200 and r.content:
            return r.content
    except Exception:
        pass
    return b""


def compose_pillow(art, Image):
    src = Image.open(io.BytesIO(art)).convert("RGBA")
    scale = min(CARD_W / src.width, CARD_H / src.height)
    w, h = max(1, int(src.width * scale)), max(1, int(src.height * scale))
    src = src.resize((w, h), Image.LANCZOS)
    card = Image.new("RGBA", (CARD_W, CARD_H), (0, 0, 0, 0))
    card.paste(src, ((CARD_W - w) // 2, (CARD_H - h) // 2), src)
    buf = io.BytesIO()
    card.save(buf, "PNG")
    return buf.getvalue()


def compose_ffmpeg(art):
    """Same result as compose_pillow, for machines without Pillow."""
    tmp = Path(tempfile.mkdtemp(prefix="nxe_tile_"))
    try:
        src, dst = tmp / "in", tmp / "out.png"
        src.write_bytes(art)
        vf = ("scale=%d:%d:force_original_aspect_ratio=decrease,format=rgba,"
              "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x00000000"
              % (CARD_W, CARD_H, CARD_W, CARD_H))
        r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(src),
                            "-vf", vf, str(dst)], capture_output=True)
        if r.returncode == 0 and dst.exists():
            return dst.read_bytes()
    except Exception:
        pass
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return b""


def make_tile(art, composer):
    """Centre the box art on a 420x320 card, or hand it back untouched.

    Returns (png_bytes, composited?). The card is transparent, so the slot's own
    background shows through and the tile looks like a boxed game on a shelf
    rather than a stretched rectangle.
    """
    kind, tool = composer
    try:
        if kind == "pillow":
            return compose_pillow(art, tool), True
        if kind == "ffmpeg":
            out = compose_ffmpeg(art)
            if out:
                return out, True
    except Exception:
        pass
    return art, False


def make_category_card(tile_paths, composer):
    """A category's own cover: a grid of the covers filed under it.

    Nothing ships a picture for "Shooter", and inventing genre artwork would be
    a different job from this one, so the category wears what is behind it --
    six of its own titles in a 3x2 grid. It reads as a shelf, and every piece of
    it is art already on disk.

    The tiles are cropped before they go in the grid. Each one is art centred on
    a transparent card, so tiling them whole would leave a gap between every
    cover; the crop takes the middle, which is the art itself.
    """
    if not tile_paths:
        return b""
    cols, rows = 3, 2
    cw, ch = CARD_W // cols, CARD_H // rows
    # Cycle rather than pad: a category with four titles still fills its grid.
    picks = [tile_paths[i % len(tile_paths)] for i in range(cols * rows)]

    kind, tool = composer
    if kind == "pillow":
        try:
            Image = tool
            card = Image.new("RGBA", (CARD_W, CARD_H), (0, 0, 0, 0))
            for i, path in enumerate(picks):
                src = Image.open(path).convert("RGBA")
                keep = min(src.width, int(src.height * 0.75))
                left = (src.width - keep) // 2
                src = src.crop((left, 0, left + keep, src.height)).resize(
                    (cw, ch), Image.LANCZOS)
                card.paste(src, ((i % cols) * cw, (i // cols) * ch), src)
            buf = io.BytesIO()
            card.save(buf, "PNG")
            return buf.getvalue()
        except Exception:
            return b""

    if kind != "ffmpeg":
        return b""

    tmp = Path(tempfile.mkdtemp(prefix="nxe_cat_"))
    try:
        dst = tmp / "out.png"
        cmd = ["ffmpeg", "-y", "-loglevel", "error"]
        for path in picks:
            cmd += ["-i", str(path)]
        parts, names = [], ""
        for i in range(cols * rows):
            parts.append(r"[%d:v]crop=min(iw\,ih*0.75):ih:(iw-min(iw\,ih*0.75))/2:0,"
                         r"scale=%d:%d,format=rgba[c%d]" % (i, cw, ch, i))
            names += "[c%d]" % i
        layout = "|".join("%d_%d" % ((i % cols) * cw, (i // cols) * ch)
                          for i in range(cols * rows))
        parts.append("%sxstack=inputs=%d:layout=%s:fill=none[v]" % (names, cols * rows, layout))
        cmd += ["-filter_complex", ";".join(parts), "-map", "[v]", str(dst)]
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode == 0 and dst.exists():
            return dst.read_bytes()
    except Exception:
        pass
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return b""


def fit_to_card(art):
    """A square picture cut to the shape of a slot card.

    A gamerpic is square and a card is 4:3, so it is filled and cropped rather
    than padded -- padded, it sits as a square island with the card showing down
    both sides. The crop takes a little off the top and bottom, which a portrait
    has room for. Box art goes through make_tile instead, which pads, because
    cropping a cover would cut the game's own title off it.

    Size matters as much as shape. Every picture on screen is held decoded at
    once and the loader gives up around 84MB; sixteen gamerpics at 424x424 is
    11MB of that against 1.5MB at card size, and the marketplace rows need the
    rest.

    Returns (png_bytes, fitted?) and hands the art back untouched when neither
    Pillow nor ffmpeg is available.
    """
    try:
        from PIL import Image
        src = Image.open(io.BytesIO(art)).convert("RGBA")
        scale = max(CARD_W / src.width, CARD_H / src.height)
        w, h = max(1, int(src.width * scale)), max(1, int(src.height * scale))
        src = src.resize((w, h), Image.LANCZOS)
        left, top = (w - CARD_W) // 2, (h - CARD_H) // 2
        buf = io.BytesIO()
        src.crop((left, top, left + CARD_W, top + CARD_H)).save(buf, "PNG")
        return buf.getvalue(), True
    except ImportError:
        pass
    except Exception:
        return art, False

    if not shutil.which("ffmpeg"):
        return art, False
    tmp = Path(tempfile.mkdtemp(prefix="nxe_pic_"))
    try:
        src, dst = tmp / "in", tmp / "out.png"
        src.write_bytes(art)
        vf = ("scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d,format=rgba"
              % (CARD_W, CARD_H, CARD_W, CARD_H))
        r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(src),
                            "-vf", vf, str(dst)], capture_output=True)
        if r.returncode == 0 and dst.exists():
            return dst.read_bytes(), True
    except Exception:
        pass
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return art, False


def primary_genre(g):
    """The category a game files under.

    A title can carry several genres -- Borderlands is Role Playing and Shooter
    -- and the marketplace still had to shelve it somewhere, so the first one
    the catalogue lists wins. That keeps each game in exactly one category, the
    way a shelf works.
    """
    for x in (g.get("genre") or []):
        if x in NOT_GAMES:
            continue
        mapped = GENRE_MAP.get(x)
        if mapped:
            return mapped
    return ""


def build_plan(pool, args):
    """Order the row: either one flat run, or every category with its games.

    `pool` is already in the order --sort asked for, so each category's pick
    inherits it, and the categories themselves are ordered by how much of the
    catalogue they hold -- which opens on Action & Adventure and Shooter, the way
    the storefront's own genre list did.

    The row is a fixed size: 0x922D6018 refuses any slot past the 64th and
    returns null instead of failing, so anything built beyond that would vanish
    silently. Headings count against that budget too, which is what decides how
    many categories fit.
    """
    if args.flat:
        return [("game", g) for g in pool[:args.limit]]

    by_genre = {}
    for g in pool:
        name = primary_genre(g)
        if name:
            by_genre.setdefault(name, []).append(g)

    # In the order Microsoft's own genre list ran, not by size: these are its
    # categories now, so they keep its order.
    order = [n for n in XBOX_GENRES if n in by_genre]
    if args.categories:
        order = order[:args.categories]

    plan, budget = [], args.max_tiles
    for name in order:
        picks = by_genre[name][:args.per_category]
        if not picks:
            continue
        if budget < 1 + len(picks):
            # Room for the heading and at least one game under it, or leave the
            # category out entirely -- a heading with nothing beneath it reads
            # as a bug.
            if budget < 2:
                break
            picks = picks[:budget - 1]
        plan.append(("category", (name, len(by_genre[name]))))
        plan += [("game", g) for g in picks]
        budget -= 1 + len(picks)
        if budget < 2:
            break
    return plan


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", default=str(REPO.parent / "nxe_dash_gamedir"))
    ap.add_argument("--limit", type=int, default=20, help="how many tiles to write")
    ap.add_argument("--sort", default="rating",
                    choices=("rating", "newest", "oldest", "name", "random"))
    ap.add_argument("--genre", action="append", default=[],
                    help="only this genre; repeatable")
    ap.add_argument("--publisher", default="", help="only this publisher (substring)")
    ap.add_argument("--search", default="", help="only titles containing this")
    ap.add_argument("--min-rating", type=float, default=4.5,
                    help="floor for --sort rating (default 4.5)")
    ap.add_argument("--any-script", action="store_true",
                    help="include titles the dashboard's Latin fonts cannot draw")
    ap.add_argument("--flat", action="store_true",
                    help="one ungrouped run of tiles instead of genre categories")
    ap.add_argument("--per-category", type=int, default=3,
                    help="games under each category heading (default 3)")
    ap.add_argument("--categories", type=int, default=0,
                    help="cap the number of categories (default 0, meaning as "
                         "many as fit)")
    ap.add_argument("--max-tiles", type=int, default=60,
                    help="total tiles including headings; the channel takes 64 "
                         "at most (default 60)")
    ap.add_argument("--no-details", action="store_true",
                    help="skip the per-title metadata fetch (one request per game)")
    ap.add_argument("--force", action="store_true", help="re-download art already present")
    ap.add_argument("--list-genres", action="store_true", help="print the genres and stop")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    try:
        from PIL import Image
        composer = ("pillow", Image)
    except ImportError:
        composer = ("ffmpeg", None) if shutil.which("ffmpeg") else ("none", None)

    print("  reading the preserved marketplace catalogue...")
    r = requests.get(X360DB, headers=UA, timeout=90)
    if r.status_code != 200:
        raise SystemExit("x360db refused (%d)" % r.status_code)
    games = r.json()
    print("  %d title(s) in the catalogue" % len(games))

    if args.list_genres:
        seen = {}
        for g in games:
            for x in (g.get("genre") or []):
                seen[x] = seen.get(x, 0) + 1
        for name, n in sorted(seen.items(), key=lambda kv: -kv[1]):
            print("   %-36s %d" % (name, n))
        return 0

    # Games only, and only entries complete enough to make a tile.
    pool = []
    seen_titles = set()
    for g in games:
        title = (g.get("title") or "").strip()
        genres = g.get("genre") or []
        if not title or not g.get("id") or not g.get("boxart"):
            continue
        if genres and all(x in NOT_GAMES for x in genres):
            continue
        if not args.any_script and not mostly_latin(title):
            continue
        key = title.lower()
        if key in seen_titles:       # the database carries duplicate listings
            continue
        seen_titles.add(key)
        pool.append(g)

    if args.genre:
        want = {x.lower() for x in args.genre}
        pool = [g for g in pool
                if any((x or "").lower() in want for x in (g.get("genre") or []))]
    if args.publisher:
        p = args.publisher.lower()
        pool = [g for g in pool if p in (g.get("publisher") or "").lower()]
    if args.search:
        s = args.search.lower()
        pool = [g for g in pool if s in (g.get("title") or "").lower()]

    if args.sort == "rating":
        # Ratings bunch hard -- 245 titles clear 4.5 and most sit exactly on it --
        # so an alphabetical tie-break would fill the whole row from the As. Break
        # ties on a hash of the title instead: the spread looks like a storefront
        # and, being a hash rather than a shuffle, the same run gives the same row.
        #
        # The floor only applies to the flat row, where it is the one thing
        # keeping the run watchable. Grouped, it would starve the smaller
        # categories -- Card & Board and Educational have few titles between them
        # clearing 4.5 -- and each category takes its own best anyway.
        if args.flat:
            pool = [g for g in pool if rating_of(g) >= args.min_rating]
        pool.sort(key=lambda g: (-rating_of(g),
                                 hashlib.md5((g.get("title") or "").strip().lower()
                                             .encode("utf-8")).hexdigest()))
    elif args.sort == "newest":
        pool = [g for g in pool if year_of(g)]
        pool.sort(key=lambda g: year_of(g), reverse=True)
    elif args.sort == "oldest":
        pool = [g for g in pool if year_of(g)]
        pool.sort(key=lambda g: year_of(g))
    elif args.sort == "name":
        pool.sort(key=lambda g: (g.get("title") or "").strip().lower())
    else:
        import random
        random.shuffle(pool)

    if not pool:
        raise SystemExit("nothing matched those filters")

    plan = build_plan(pool, args)
    n_games = sum(1 for kind, _ in plan if kind == "game")
    n_cats = len(plan) - n_games
    if args.flat:
        print("  %d match(es); taking %d" % (len(pool), n_games))
    else:
        print("  %d match(es); taking %d game(s) across %d categor(ies)"
              % (len(pool), n_games, n_cats))

    gamedir = Path(args.gamedir)
    imgdir = gamedir / "images" / "marketplace"
    if not args.dry_run:
        imgdir.mkdir(parents=True, exist_ok=True)

    # Group the plan so a category's own card can be built from the covers
    # underneath it, which are only on disk once its games have been fetched.
    groups, current = [], None
    for kind, item in plan:
        if kind == "category":
            current = (item, [])
            groups.append(current)
        elif current is not None:
            current[1].append(item)

    rows, composited, cards = [], 0, 0
    for (gname, gcount), items in groups:
        print("  [%s]" % ascii_safe(gname))
        made = []
        for g in items:
            title = (g.get("title") or "").strip()
            hexid = g["id"].upper()
            rel = "images/marketplace/%s.png" % hexid
            dest = imgdir / ("%s.png" % hexid)

            if not args.dry_run and (args.force or not dest.exists()):
                art = fetch(g["boxart"]) or fetch(ICON % hexid.lower())
                if not art:
                    print("    %-38s no art; skipped" % ascii_safe(title[:38]))
                    continue
                tile, did = make_tile(art, composer)
                if did:
                    composited += 1
                dest.write_bytes(tile)

            made.append(dest)
            info = {} if args.no_details else details_for(hexid)
            rows.append((title, subtitle_for(g), rel, hexid, "",
                         info.get("developer", ""), info.get("publisher", ""),
                         info.get("released", ""), info.get("rating", ""),
                         info.get("description", "")))
            print("    %-38s %s" % (ascii_safe(title[:38]), ascii_safe(subtitle_for(g))))

        # The category's own tile, from the covers filed under it.
        # A real Xbox card if there is one, and there is for all eight of these.
        # genre_*.png comes off Experience Disc 1.6 and is never regenerated --
        # --force rebuilds montages, it does not overwrite Microsoft's artwork.
        real = imgdir / ("genre_%s.png" % safe_name(gname))
        if real.exists():
            rows.insert(len(rows) - len(items),
                        (gname, "%d game%s" % (gcount, "" if gcount == 1 else "s"),
                         "images/marketplace/%s" % real.name, "", "category",
                         "", "", "", "", ""))
            continue

        cat_rel, cat_file = "", imgdir / ("cat_%s.png" % safe_name(gname))
        if not args.dry_run and made:
            if args.force or not cat_file.exists():
                card = make_category_card(made, composer)
                if card:
                    cat_file.write_bytes(card)
                    cards += 1
            if cat_file.exists():
                cat_rel = "images/marketplace/%s" % cat_file.name
        elif args.dry_run:
            cat_rel = "images/marketplace/%s" % cat_file.name

        rows.insert(len(rows) - len(items),
                    (gname, "%d game%s" % (gcount, "" if gcount == 1 else "s"),
                     cat_rel, "", "category", "", "", "", "", ""))

    out = [
        "# Written by tools/fetch_marketplace.py -- edit freely, it is only a text file.",
        "# The Xbox 360 Marketplace, preserved: catalogue from x360db, box art from",
        "# Microsoft's download.xbox.com.",
        "# name | subtitle | image | title id (hex) | kind ('category' marks a heading)",
        "#      | developer | publisher | released | rating | description",
        "",
    ]
    out += ["|".join(str(x).replace("|", "/") for x in r) for r in rows]
    text = "\n".join(out) + "\n"

    if args.dry_run:
        print()
        print(ascii_safe(text))
        return 0

    (gamedir / "marketplace.txt").write_text(text, encoding="utf-8")
    print()
    print("  wrote %d tile(s) to %s" % (len(rows), gamedir / "marketplace.txt"))
    print("  tile art in %s" % imgdir)
    if composer[0] == "none":
        print("  Neither Pillow nor ffmpeg is here, so raw 219x300 box art was written")
        print("  and the shell will stretch it. For properly composed tiles install")
        print("  either one, then re-run with --force.")
    else:
        print("  %d composited onto %dx%d cards via %s, plus %d category card(s)"
              % (composited, CARD_W, CARD_H, composer[0], cards))
    return 0


if __name__ == "__main__":
    sys.exit(main())
