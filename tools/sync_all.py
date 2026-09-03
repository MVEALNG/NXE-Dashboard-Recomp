#!/usr/bin/env python3
"""Sign in to Xbox LIVE and fill the dashboard with everything it can.

One command instead of nine, because the order matters and getting it wrong is
not obvious. fetch_profile.py signs in first -- that is the one that shows the
eight-character code to type at microsoft.com/link -- and every tool after it
reuses the token it cached, so nobody is asked to sign in twice.

    python tools/sync_all.py
    python tools/sync_all.py --quick      # skip the slow catalogue fetches
    python tools/sync_all.py --video      # include the TMDB video row

Covers come before games on purpose: a title with no artwork is left off the
shelf, because a tile with no picture faults the page it is drawn on. Fetching
the art first is what decides how many games there are to show.

The video row is not included by default. It needs a themoviedb.org key, and a
key that is not there should not look like a failure at the end of a sync that
otherwise worked.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent

# script, what it does, and whether it is slow enough to want skipping
STEPS = [
    ("fetch_profile.py", "your profile -- signs in", False),
    ("fetch_covers.py", "cover art for everything you have played", False),
    ("fetch_games.py", "tile art for the games row", False),
    ("fetch_title_stats.py", "the games, with achievements and time played", False),
    ("fetch_achievements.py", "the achievement list for each game", True),
    ("fetch_friends.py", "your friends", False),
    ("fetch_social.py", "following, followers and a page each", True),
    ("fetch_recent_players.py", "people you have played with", False),
    ("fetch_messages.py", "your inbox", False),
    ("fetch_marketplace.py", "the preserved Xbox 360 Marketplace", True),
]


def run(script, args):
    print()
    print("=" * 70)
    print("  %s" % script)
    print("=" * 70)
    started = time.time()
    result = subprocess.run([sys.executable, str(HERE / script)] + args)
    took = time.time() - started
    if result.returncode != 0:
        print("  !! %s stopped with code %d after %.0fs" % (script, result.returncode, took))
        return False
    print("  done in %.0fs" % took)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="skip the slow steps")
    ap.add_argument("--video", action="store_true",
                    help="also fetch the video row (needs a TMDB key)")
    args, rest = ap.parse_known_args()

    steps = [s for s in STEPS if not (args.quick and s[2])]
    if args.video:
        steps.append(("fetch_video.py", "films and television for the video row", True))

    print("Syncing the dashboard from Xbox LIVE.")
    print()
    print("The first step signs you in: it will print an eight-character code and a")
    print("link to microsoft.com/link. Type the code there, approve it, and the rest")
    print("runs on its own -- nothing after this asks you for anything.")
    print()
    print("%d step(s) to run." % len(steps))

    failed = []
    for script, what, _ in steps:
        print()
        print("-> %s" % what)
        if not run(script, rest):
            failed.append(script)
            # Sign-in failing means every step after it fails the same way, so
            # stop rather than print the same error nine times.
            if script == "fetch_profile.py":
                print()
                print("  Sign-in did not finish, so nothing after it can run.")
                break

    print()
    print("=" * 70)
    if not failed:
        print("  Everything synced. Press F6 in the dashboard to pick it up.")
    else:
        print("  Finished with %d step(s) unfinished: %s" % (len(failed), ", ".join(failed)))
        print("  The rest was written and is usable.")
    print("=" * 70)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
