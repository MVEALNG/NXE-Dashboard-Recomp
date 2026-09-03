#!/usr/bin/env python3
"""Import an Xbox 360 profile, so the dashboard can sign in without Xbox LIVE.

Signing in to a Microsoft account fills the dashboard from the service. This is
the other way in: a profile you already have, from a real console or from Xenia,
copied into the storage tree where the dashboard looks for one.

    python tools/import_profile.py <profile> [--storage storage]

<profile> is either of the two shapes these come in:

  a file    A profile downloaded from a console is a single signed STFS package
            named for its XUID -- E030000000A8C189, no extension. It has to be
            unpacked, which tools/extract_stfs.py does.

  a folder  Xenia keeps profiles unpacked already, in the same layout this uses:
            content/<xuid>/FFFE07D1/00010000/<xuid>/. Point at either the inner
            folder holding FFFE07D1.gpd or the <xuid> folder above it.

Either way it ends up at

    <storage>/Content/<XUID>/FFFE07D1/00010000/<XUID>/

which is where FindDashboardGpd looks (see src/profile_settings.cpp). Nothing is
moved or altered at the source; this only ever copies.

The XUID is taken from the folder or file name, because that is what the name of
a profile package is -- and the dashboard finds a profile by that directory name,
so a profile imported under the wrong one would never be found.
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

XUID = re.compile(r"^[0-9A-Fa-f]{16}$")


def find_xuid(path):
    """The XUID this profile belongs to, from its own name or a parent's."""
    for candidate in [path.name] + [p.name for p in path.parents]:
        if XUID.match(candidate):
            return candidate.upper()
    return None


def find_gpd_root(folder):
    """The folder holding FFFE07D1.gpd, wherever it is under here.

    Xenia and a console lay a profile out the same way, but somebody pointing at
    "their profile" might reasonably choose any level of it. Searching is kinder
    than insisting on one.
    """
    if (folder / "FFFE07D1.gpd").exists():
        return folder
    for found in folder.rglob("FFFE07D1.gpd"):
        return found.parent
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile", help="the profile package or folder to import")
    ap.add_argument("--storage", default=str(REPO / "storage"),
                    help="the dashboard's storage root (default: storage beside the repo)")
    args = ap.parse_args()

    source = Path(args.profile).expanduser()
    if not source.exists():
        print("There is nothing at %s" % source)
        return 1

    xuid = find_xuid(source)
    if not xuid:
        print("Could not work out the XUID from '%s'." % source.name)
        print("A profile is named for its XUID -- sixteen hex digits, like E030000000A8C189 --")
        print("either as the file itself or as a folder somewhere above it.")
        return 1

    destination = Path(args.storage) / "Content" / xuid / "FFFE07D1" / "00010000" / xuid
    if destination.exists() and any(destination.iterdir()):
        print("A profile is already imported at %s" % destination)
        print("Delete it first if you want to replace it.")
        return 1
    destination.mkdir(parents=True, exist_ok=True)

    if source.is_file():
        print("Unpacking %s ..." % source.name)
        result = subprocess.run(
            [sys.executable, str(HERE / "extract_stfs.py"), str(source), str(destination)])
        if result.returncode != 0:
            print("extract_stfs.py could not unpack it; is it really an STFS package?")
            return 1
    else:
        root = find_gpd_root(source)
        if not root:
            print("No FFFE07D1.gpd anywhere under %s" % source)
            print("That file is the profile itself, so without it there is nothing to import.")
            return 1
        print("Copying from %s ..." % root)
        shutil.copytree(root, destination, dirs_exist_ok=True)

    gpd = destination / "FFFE07D1.gpd"
    if not gpd.exists():
        print("Imported, but there is no FFFE07D1.gpd in the result -- the dashboard will not")
        print("see this as a profile. Left at %s for you to look at." % destination)
        return 1

    files = sum(1 for _ in destination.rglob("*") if _.is_file())
    print()
    print("Imported %s: %d file(s) at %s" % (xuid, files, destination))

    # The gamertag lives encrypted in the Account blob; profile_summary reads it.
    # Worth showing, because a XUID says nothing about whose profile this is.
    summary = HERE / "profile_summary.py"
    if summary.exists():
        subprocess.run([sys.executable, str(summary), str(destination)])

    print()
    print("Start the dashboard and choose it from Sign In, or set profile_xuid = %s" % xuid)
    return 0


if __name__ == "__main__":
    sys.exit(main())
