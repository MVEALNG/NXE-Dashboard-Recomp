#!/usr/bin/env python3
"""Summarise an unpacked Xbox 360 profile: gamertag, gamerscore, achievements.

A way to see what a staged profile actually contains without booting the
dashboard -- useful for confirming an imported profile is the one intended, and
that its GPDs survived extraction.

    python tools/profile_summary.py <profile-directory>

The gamertag comes from the encrypted Account blob, decrypted the same way
src/account_decrypt.cpp does it (HMAC-SHA1 of the confounder under the retail
console key, then RC4). Gamerscore and achievements come from the XDBF GPDs:
FFFE07D1.gpd holds the dashboard's own settings, and each <titleid>.gpd holds
that title's achievements.
"""
import hashlib
import hmac
import os
import struct
import sys

CONSOLE_KEY = bytes([0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1, 0xEA, 0xE9,
                     0xAB, 0x31, 0x70, 0xF3, 0xAD, 0x47, 0xEB, 0xF3])

NAMESPACE_ACHIEVEMENT = 1
NAMESPACE_SETTING = 3
NAMESPACE_TITLE = 4

# XPROFILE ids that carry the numbers shown on a gamercard.
SETTING_GAMERSCORE = 0x10040013
SETTING_GAMERTAG = 0x40008102


def rc4(key, data):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for b in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def account_gamertag(path):
    if not os.path.exists(path):
        return None
    blob = open(path, "rb").read()
    if len(blob) < 32:
        return None
    key = hmac.new(CONSOLE_KEY, blob[:16], hashlib.sha1).digest()[:16]
    plain = rc4(key, blob[16:])
    # The tag is a UTF-16BE run; scan rather than hardcode an offset, because
    # editors move it (see the note in src/account_decrypt.cpp).
    best = ""
    i = 0
    while i + 1 < len(plain):
        run = []
        j = i
        while j + 1 < len(plain) and plain[j] == 0 and 0x20 <= plain[j + 1] < 0x7F:
            run.append(chr(plain[j + 1]))
            j += 2
        if len(run) > len(best):
            best = "".join(run)
        i = j + 2 if len(run) else i + 2
    return best or None


def xdbf_entries(data):
    """(namespace, id, offset, length) for every entry in an XDBF file."""
    if len(data) < 24 or data[:4] != b"XDBF":
        return []
    entry_table_len, entry_count, free_table_len = struct.unpack_from(">III", data, 8)
    base = 24 + entry_table_len * 18 + free_table_len * 8
    out = []
    off = 24
    for _ in range(entry_count):
        if off + 18 > len(data):
            break
        ns = struct.unpack_from(">H", data, off)[0]
        ident = struct.unpack_from(">Q", data, off + 2)[0]
        e_off, e_len = struct.unpack_from(">II", data, off + 10)
        out.append((ns, ident, base + e_off, e_len))
        off += 18
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = sys.argv[1]

    tag = account_gamertag(os.path.join(root, "Account"))
    print(f"gamertag:   {tag or '(could not decrypt)'}")

    dash = os.path.join(root, "FFFE07D1.gpd")
    if os.path.exists(dash):
        data = open(dash, "rb").read()
        for ns, ident, off, length in xdbf_entries(data):
            if ns == NAMESPACE_SETTING and ident == SETTING_GAMERSCORE:
                # A setting record is a small header then the value; the
                # gamerscore is a 32-bit int at +0x10.
                if off + 0x14 <= len(data):
                    print(f"gamerscore (dashboard GPD cached counter): "
                          f"{struct.unpack_from('>I', data, off + 0x10)[0]}")
                break

    # Achievement record: gamerscore at +0x0C, flags at +0x10, and bit 0x20000
    # is "unlocked". Confirmed by the value distribution those offsets produce
    # -- 10/20/15/25/50/100 and so on, which is what real gamerscores look like.
    titles = unlocked = total = score = 0
    for name in sorted(os.listdir(root)):
        if not name.lower().endswith(".gpd") or name.upper().startswith("FFF"):
            continue
        data = open(os.path.join(root, name), "rb").read()
        entries = [e for e in xdbf_entries(data) if e[0] == NAMESPACE_ACHIEVEMENT]
        if not entries:
            continue
        titles += 1
        for _, _, off, _length in entries:
            if off + 0x18 > len(data):
                continue
            total += 1
            if struct.unpack_from(">I", data, off + 0x10)[0] & 0x20000:
                unlocked += 1
                score += struct.unpack_from(">I", data, off + 0x0C)[0]

    print(f"titles:     {titles} with achievement data")
    print(f"achievements: {unlocked} unlocked of {total} recorded")
    print(f"gamerscore (summed from unlocked achievements): {score}")

    for extra in ("AvatarAssets", "avtr_64.png", "tile_64.png", "ThematicSkin", "PEC"):
        path = os.path.join(root, extra)
        if os.path.exists(path):
            what = "dir" if os.path.isdir(path) else f"{os.path.getsize(path)} bytes"
            print(f"  {extra}: {what}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
