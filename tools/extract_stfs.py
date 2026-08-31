#!/usr/bin/env python3
"""Extract an Xbox 360 STFS package (CON/LIVE/PIRS) into a directory.

A profile downloaded from a console is a single signed STFS file named for the
profile's XUID. The dashboard does not read those directly -- it looks for the
package already unpacked, scanning for

    Content/<XUID>/FFFE07D1/00010000/<name>/FFFE07D1.gpd

(see FindDashboardGpd in src/profile_settings.cpp) -- so a downloaded profile
has to be unpacked into that shape before the dashboard can see it.

    python tools/extract_stfs.py <package> <output-dir>

The block layout follows the SDK's StfsContainerDevice
(rexglue-sdk/src/filesystem/devices/stfs_container_device.cpp) rather than
being re-derived, so the two agree on the same packages:

  - 0x1000-byte blocks, with a hash table every 170 blocks, another every
    170*170, and a third every 170^3.
  - A package that is not read-only reserves *two* backing blocks per hash
    table and picks between them per level with an "active index" bit, so
    resolving one block's hash entry can mean reading the level 1 and level 2
    tables first.
  - Files are block chains: each block's hash entry names the next block, and
    0xFFFFFF ends the chain. Contiguous files are not special-cased.

Writes into the output directory without removing anything.
"""
import os
import struct
import sys

BLOCK = 0x1000
LEVELS = (170, 28900, 4913000)
END_OF_CHAIN = 0xFFFFFF

# XContentHeader is 0x344 bytes and ends with header_size; XContentMetadata
# follows, putting the STFS volume descriptor at 0x379.
HEADER_SIZE_OFFSET = 0x340
VOLUME_DESCRIPTOR_OFFSET = 0x379


def round_up(value, multiple):
    return (value + multiple - 1) // multiple * multiple


def u24le(b):
    return b[0] | (b[1] << 8) | (b[2] << 16)


class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(0x400)
        self.magic = head[:4]
        if self.magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise SystemExit(f"{path}: not an STFS package (magic {self.magic!r})")

        self.header_size = struct.unpack_from(">I", head, HEADER_SIZE_OFFSET)[0]
        self.data_start = round_up(self.header_size, BLOCK)

        d = VOLUME_DESCRIPTOR_OFFSET
        self.descriptor_length = head[d]
        flags = head[d + 2]
        self.read_only = bool(flags & 0x01)
        self.root_active_index = bool(flags & 0x02)
        self.file_table_block_count = struct.unpack_from("<H", head, d + 3)[0]
        self.file_table_block_number = u24le(head[d + 5:d + 8])
        self.total_block_count = struct.unpack_from(">I", head, d + 0x1C)[0]

        # One backing block per hash table when read-only, otherwise two.
        self.bpht = 1 if self.read_only else 2
        self.step0 = LEVELS[0] + self.bpht
        self.step1 = LEVELS[1] + (LEVELS[0] + 1) * self.bpht

        self.hash_cache = {}

    def block_offset(self, index):
        base = LEVELS[0]
        block = index
        for _ in range(3):
            block += ((index + base) // base) * self.bpht
            if index < base:
                break
            base *= LEVELS[0]
        return self.data_start + (block << 12)

    def hash_block_number(self, index, level):
        if level == 0:
            if index < LEVELS[0]:
                return 0
            block = (index // LEVELS[0]) * self.step0
            block += ((index // LEVELS[1]) + 1) * self.bpht
            if index < LEVELS[1]:
                return block
            return block + self.bpht
        if level == 1:
            if index < LEVELS[1]:
                return self.step0
            return (index // LEVELS[1]) * self.step1 + self.bpht
        return self.step1

    def hash_block_offset(self, index, level):
        return self.data_start + (self.hash_block_number(index, level) << 12)

    def _read_table(self, offset):
        self.f.seek(offset)
        return self.f.read(LEVELS[0] * 0x18)

    def block_hash(self, index):
        """The 0x18-byte level 0 hash entry for a block."""
        secondary = BLOCK if self.root_active_index else 0
        lv0 = self.hash_block_offset(index, 0)

        if lv0 not in self.hash_cache:
            if self.read_only:
                secondary = 0
            else:
                if self.total_block_count > LEVELS[0]:
                    lv1 = self.hash_block_offset(index, 1)
                    if lv1 not in self.hash_cache:
                        if self.total_block_count > LEVELS[1]:
                            lv2 = self.hash_block_offset(index, 2)
                            if lv2 not in self.hash_cache:
                                self.hash_cache[lv2] = self._read_table(lv2 + secondary)
                            record = (index // LEVELS[1]) % LEVELS[0]
                            info = struct.unpack_from(">I", self.hash_cache[lv2],
                                                      record * 0x18 + 0x14)[0]
                            secondary = BLOCK if info & 0x40000000 else 0
                        self.hash_cache[lv1] = self._read_table(lv1 + secondary)
                    record = (index // LEVELS[0]) % LEVELS[0]
                    info = struct.unpack_from(">I", self.hash_cache[lv1],
                                              record * 0x18 + 0x14)[0]
                    secondary = BLOCK if info & 0x40000000 else 0
            self.hash_cache[lv0] = self._read_table(lv0 + secondary)

        record = index % LEVELS[0]
        return struct.unpack_from(">I", self.hash_cache[lv0], record * 0x18 + 0x14)[0]

    def next_block(self, index):
        return self.block_hash(index) & 0xFFFFFF

    def read_chain(self, start_block, length):
        out = bytearray()
        index = start_block
        remaining = length
        while remaining and index != END_OF_CHAIN:
            take = min(BLOCK, remaining)
            self.f.seek(self.block_offset(index))
            chunk = self.f.read(take)
            if not chunk:
                break
            out += chunk
            remaining -= len(chunk)
            index = self.next_block(index)
        return bytes(out), remaining

    def entries(self):
        """(name, directory?, parent_index, start_block, length) per file."""
        out = []
        index = self.file_table_block_number
        for _ in range(self.file_table_block_count):
            self.f.seek(self.block_offset(index))
            table = self.f.read(BLOCK)
            for off in range(0, len(table), 0x40):
                record = table[off:off + 0x40]
                if len(record) < 0x40 or record[0] == 0:
                    continue
                flags = record[0x28]
                name_length = flags & 0x3F
                if not name_length:
                    continue
                name = record[:name_length].decode("latin-1")
                out.append({
                    "name": name,
                    "directory": bool(flags & 0x80),
                    "start_block": u24le(record[0x2F:0x32]),
                    "parent": struct.unpack_from(">h", record, 0x32)[0],
                    "length": struct.unpack_from(">I", record, 0x34)[0],
                })
            index = self.next_block(index)
            if index == END_OF_CHAIN:
                break
        return out


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    package, out_dir = sys.argv[1], sys.argv[2]

    stfs = Stfs(package)
    print(f"{os.path.basename(package)}: {stfs.magic.decode()} "
          f"header_size={stfs.header_size:#x} blocks={stfs.total_block_count} "
          f"read_only={stfs.read_only} file_table={stfs.file_table_block_count} block(s) "
          f"@{stfs.file_table_block_number}")

    records = stfs.entries()

    # parent is an index into this same list, or -1 for the root.
    def full_path(i):
        parts = []
        seen = set()
        while i >= 0 and i not in seen:
            seen.add(i)
            parts.append(records[i]["name"])
            i = records[i]["parent"]
        return os.path.join(*reversed(parts)) if parts else ""

    os.makedirs(out_dir, exist_ok=True)
    written = short = 0
    for i, record in enumerate(records):
        target = os.path.join(out_dir, full_path(i))
        if record["directory"]:
            os.makedirs(target, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(target) or out_dir, exist_ok=True)
        data, remaining = stfs.read_chain(record["start_block"], record["length"])
        if remaining:
            print(f"  ! {record['name']}: got {len(data)} of {record['length']} bytes")
            short += 1
        with open(target, "wb") as handle:
            handle.write(data)
        written += 1

    print(f"wrote {written} file(s), {short} short")
    return 1 if short else 0


if __name__ == "__main__":
    raise SystemExit(main())
