#!/usr/bin/env python3
"""Import Xbox 360 theme packages into the dashboard's storage tree.

A theme downloaded from a console is a signed STFS package. The dashboard does
not read those directly: ContentManager::GetPackages lists the content directory
and skips anything that is not a directory, so a package has to be unpacked into

    Content/0000000000000000/FFFE07D1/00030000/<content id>/

with a "<content id>.header" file in a *sibling* tree:

    Content/0000000000000000/FFFE07D1/Headers/00030000/<content id>.header

ResolvePackageHeaderPath puts headers under Headers/<content type>/, not beside
the content, and a header written next to the directory is simply never read --
the theme then falls back to being named after its folder, which is a forty
character hash. The name itself comes out of the package's own metadata at
offset 0x411, and the layout is XCONTENT_AGGREGATE_DATA, confirmed against a
header the runtime wrote itself.

    python tools/import_themes.py <themes dir> <storage dir> [--limit N] [--dry-run]

Nothing is deleted or overwritten. A theme whose directory already has files in
it is left alone and counted as skipped, so the import can be re-run and can be
interrupted without losing what it has already done.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_stfs import Stfs  # noqa: E402

CONTENT_TYPE_THEME = 0x00030000
DASH_TITLE_ID = 0xFFFE07D1
DEVICE_ID_HDD = 1

# XCONTENT_AGGREGATE_DATA, 0x148 bytes. Offsets from the SDK's struct:
# device_id, content_type, a 128-character UTF-16BE display name, a 42-byte
# file name, then the 8-aligned xuid and the title id.
HEADER_SIZE = 0x148
OFF_DEVICE_ID = 0x00
OFF_CONTENT_TYPE = 0x04
OFF_DISPLAY_NAME = 0x08
OFF_FILE_NAME = 0x108
OFF_XUID = 0x138
OFF_TITLE_ID = 0x140

PACKAGE_NAME = re.compile(r"[0-9A-F]{40,42}")


def package_display_name(path):
    """The theme's own name, out of its XContent metadata."""
    with open(path, "rb") as handle:
        handle.seek(0x411)
        raw = handle.read(256)
    text = raw.decode("utf-16-be", errors="ignore")
    return text.split("\x00", 1)[0].strip()


def build_header(display_name, file_name):
    header = bytearray(HEADER_SIZE)
    struct.pack_into(">I", header, OFF_DEVICE_ID, DEVICE_ID_HDD)
    struct.pack_into(">I", header, OFF_CONTENT_TYPE, CONTENT_TYPE_THEME)

    encoded = display_name.encode("utf-16-be")[: 127 * 2]
    header[OFF_DISPLAY_NAME : OFF_DISPLAY_NAME + len(encoded)] = encoded

    name = file_name.encode("ascii", errors="ignore")[:42]
    header[OFF_FILE_NAME : OFF_FILE_NAME + len(name)] = name

    struct.pack_into(">Q", header, OFF_XUID, 0)
    struct.pack_into(">I", header, OFF_TITLE_ID, DASH_TITLE_ID)
    return bytes(header)


def extract_package(package, out_dir):
    """Unpack one STFS package, following extract_stfs's own record shape."""
    stfs = Stfs(package)
    records = stfs.entries()

    def full_path(index):
        parts = []
        seen = set()
        while index >= 0 and index not in seen:
            seen.add(index)
            parts.append(records[index]["name"])
            index = records[index]["parent"]
        return os.path.join(*reversed(parts)) if parts else ""

    os.makedirs(out_dir, exist_ok=True)
    written = short = 0
    for index, record in enumerate(records):
        target = os.path.join(out_dir, full_path(index))
        if record["directory"]:
            os.makedirs(target, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(target) or out_dir, exist_ok=True)
        data, remaining = stfs.read_chain(record["start_block"], record["length"])
        if remaining:
            short += 1
        with open(target, "wb") as handle:
            handle.write(data)
        written += 1
    return written, short


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    limit = None
    for flag in flags:
        if flag.startswith("--limit"):
            limit = int(flag.split("=", 1)[1])
    dry_run = "--dry-run" in flags

    if len(args) != 2:
        print(__doc__)
        return 2
    themes_dir, storage_dir = args

    title_root = os.path.join(storage_dir, "Content", "0000000000000000",
                              f"{DASH_TITLE_ID:08X}")
    dest_root = os.path.join(title_root, f"{CONTENT_TYPE_THEME:08X}")
    header_root = os.path.join(title_root, "Headers", f"{CONTENT_TYPE_THEME:08X}")
    os.makedirs(dest_root, exist_ok=True)
    os.makedirs(header_root, exist_ok=True)

    packages = []
    for root, _, files in os.walk(themes_dir):
        for name in files:
            if PACKAGE_NAME.fullmatch(name):
                packages.append(os.path.join(root, name))
    packages.sort()
    print(f"{len(packages)} package(s) found; writing into {dest_root}")

    imported = skipped = failed = 0
    for package in packages:
        if limit is not None and imported >= limit:
            break
        content_id = os.path.basename(package)
        out_dir = os.path.join(dest_root, content_id)

        # Already there: leave it exactly as it is.
        if os.path.isdir(out_dir) and os.listdir(out_dir):
            skipped += 1
            continue

        try:
            name = package_display_name(package) or content_id
            if dry_run:
                print(f"  would import {name!r} -> {content_id}")
                imported += 1
                continue
            os.makedirs(out_dir, exist_ok=True)
            count, short = extract_package(package, out_dir)
            with open(os.path.join(header_root, content_id + ".header"), "wb") as handle:
                handle.write(build_header(name, content_id))
            imported += 1
            note = f", {short} short" if short else ""
            print(f"  {name} -> {content_id} ({count} file(s){note})")
        except Exception as error:  # noqa: BLE001
            failed += 1
            # Do not leave a half-made theme behind.
            #
            # The first version of this created the directory before unpacking
            # and left it there when unpacking threw, which put 868 empty
            # directories into the content tree -- every one of them a theme the
            # dashboard would have listed and been unable to draw. rmdir refuses
            # anything that is not empty, so a partial extraction is kept for
            # inspection rather than silently discarded.
            try:
                os.rmdir(out_dir)
            except OSError:
                pass
            print(f"  FAILED {content_id}: {error}")

    print(f"\nimported {imported}, skipped {skipped} already present, {failed} failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
