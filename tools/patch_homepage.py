#!/usr/bin/env python3
"""Write a homepage channel manifest into homepage.xzp.

The top-level tabs come from an XML manifest stored as plain text inside
homepage.xzp, a XUIZ package. The package has an offset/size index, so the
manifest cannot change length without rebuilding the whole container -- but it
does not need to: the original is loosely indented and heavily commented, so a
denser rewrite leaves well over a kilobyte of slack. This writes the new text
into the same byte range and pads with spaces after </homepage>, which the
parser never reads.

Always rebuilds from homepage.xzp.orig, so running it repeatedly is safe and
re-running with the original source restores the stock manifest.

    python tools/patch_homepage.py [source.xml]
"""
import sys
from pathlib import Path

GAMEDIR = Path(r"C:/Desktop/NXE Dashboard/nxe_dash_gamedir")
PKG = GAMEDIR / "homepage.xzp"
ORIG = GAMEDIR / "homepage.xzp.orig"

# Byte range of the <homepage>...</homepage> document inside the package.
START, END = 44807, 49459
LENGTH = END - START  # 4652


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("homepage_manifest.xml")
    if not ORIG.exists():
        print(f"missing pristine backup {ORIG}; refusing to guess a base", file=sys.stderr)
        return 1

    base = bytearray(ORIG.read_bytes())
    if bytes(base[START:START + 9]) != b"<homepage":
        print("the manifest is not where it was expected in the backup", file=sys.stderr)
        return 1

    text = src.read_text(encoding="utf-8")
    if "</homepage>" not in text:
        print("source has no </homepage>", file=sys.stderr)
        return 1

    blob = text.encode("latin-1")
    if len(blob) > LENGTH:
        print(f"manifest is {len(blob)} bytes, {len(blob) - LENGTH} over the {LENGTH} available.\n"
              f"Trim indentation or comments -- the content itself is not the limit.", file=sys.stderr)
        return 1

    blob = blob + b" " * (LENGTH - len(blob))
    base[START:END] = blob
    PKG.write_bytes(bytes(base))
    print(f"wrote {len(text.encode('latin-1'))} bytes of manifest "
          f"({LENGTH - len(text.encode('latin-1'))} spare) into {PKG.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
