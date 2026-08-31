#!/usr/bin/env python3
"""Extract the profile's avatar manifest to a file the runtime can load.

The manifest is profile setting 0x63E80044, a 1000-byte binary blob inside the
dashboard GPD. The SDK's avatar support reads a manifest from whatever
avatar_manifest_path points at and checks its length is exactly 1000, so
lifting the blob out to a file is all that is needed to give the runtime a real
avatar instead of nothing.

    python tools/extract_avatar_manifest.py [out-path]

The layout is the one xenia-canary PR #768 documents, and this checks the
manifest against it before writing -- a blob that does not walk cleanly to the
offline XUID at 0x380 is not a manifest and is not worth staging:

    0x000 header (4)          0x120 body component (0x20)
    0x004 weight factor       0x140 head component (0x20)
    0x008 height factor       0x160 13 components (0x1A0)
    0x00C 3 blend shapes      0x300 4 previously required (0x80)
    0x03C 6 replacement textures
    0x0FC 9 colours           0x380 offline XUID (be64)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from profile_summary import xdbf_entries  # noqa: E402

PROFILE = (r"A:\Xbox360Storage\Content\ECF094C2048FC0CD\FFFE07D1\00010000"
           r"\ECF094C2048FC0CD")
DEFAULT_OUT = r"A:\Xbox360Storage\Cache\avatar_manifest.bin"

SETTING_AVATAR_MANIFEST = 0x63E80044
MANIFEST_SIZE = 1000
OFFLINE_XUID_OFFSET = 0x380

# Body asset ids, as GUID(Data1, Data2, Data3, Data4[8]).
MALE = struct.pack(">IHH", 2, 0, 1) + bytes([193, 200, 241, 9, 161, 156, 178, 224])
FEMALE = struct.pack(">IHH", 2, 1, 2) + bytes([193, 200, 241, 9, 161, 156, 178, 224])


def read_manifest(profile_dir):
    gpd = os.path.join(profile_dir, "FFFE07D1.gpd")
    data = open(gpd, "rb").read()
    for ns, ident, off, length in xdbf_entries(data):
        if ns == 3 and ident == SETTING_AVATAR_MANIFEST:
            # Binary setting: id, type 6, then the payload length at +0x10 and
            # the payload itself at +0x18.
            size = struct.unpack_from(">I", data, off + 0x10)[0]
            if size != MANIFEST_SIZE:
                raise SystemExit(f"manifest is {size} bytes, expected {MANIFEST_SIZE}")
            return data[off + 0x18: off + 0x18 + size]
    raise SystemExit("no avatar manifest setting in the dashboard GPD")


def describe(manifest):
    weight, height = struct.unpack_from(">ff", manifest, 4)
    body = manifest[0x120:0x130]
    xuid = struct.unpack_from(">Q", manifest, OFFLINE_XUID_OFFSET)[0]
    body_type = "male" if body == MALE else "female" if body == FEMALE else "unknown"
    print(f"  body type    : {body_type}")
    print(f"  weight/height: {weight:.3f} / {height:.3f}")
    print(f"  offline XUID : {xuid:016X}")
    return body_type != "unknown" and xuid != 0


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUT
    manifest = read_manifest(PROFILE)
    print(f"avatar manifest: {len(manifest)} bytes")
    if not describe(manifest):
        print("  refusing to stage: does not parse as an avatar manifest")
        return 1

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as handle:
        handle.write(manifest)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
