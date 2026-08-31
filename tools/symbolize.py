#!/usr/bin/env python3
"""Turn a crash report's "module+RVA" back into a guest function.

src/crash_report.cpp can only name the binary and an offset into it:

    === unhandled exception 0xc0000005 at nxe_dash_xam.dll+0x9d3e1 ===

The linker maps written beside each binary carry the symbol table, so this
looks the address up in the right one and prints the enclosing symbol.

    python tools/symbolize.py nxe_dash_xam.dll+0x9d3e1
    python tools/symbolize.py nxe_dash.exe+0x179d9

Anything with no map beside it (rexruntime.dll, system DLLs) is reported as
such rather than guessed at.
"""
import bisect
import re
import sys
from pathlib import Path

BUILD = Path(__file__).resolve().parent.parent / "out" / "build" / "win-amd64-debug"

# " 0001:00000000       sub_84B90000    0000000180001000 f   foo.obj"
ENTRY = re.compile(r"^\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{16})\s")
PREFERRED = re.compile(r"Preferred load address is ([0-9A-Fa-f]{16})")

_cache = {}


def load(map_path: Path):
    if map_path in _cache:
        return _cache[map_path]
    text = map_path.read_text(encoding="utf-8", errors="replace")
    match = PREFERRED.search(text)
    base = int(match.group(1), 16) if match else 0
    rvas, names = [], []
    for line in text.splitlines():
        m = ENTRY.match(line)
        if m:
            rvas.append(int(m.group(2), 16) - base)
            names.append(m.group(1))
    order = sorted(range(len(rvas)), key=lambda i: rvas[i])
    result = ([rvas[i] for i in order], [names[i] for i in order])
    _cache[map_path] = result
    return result


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    for arg in sys.argv[1:]:
        if "+" not in arg:
            print(f"{arg}: expected module+0xRVA")
            continue
        module, _, offset = arg.partition("+")
        rva = int(offset, 0)

        stem = Path(module).stem
        map_path = BUILD / f"{stem}.map"
        if not map_path.is_file():
            print(f"{arg} -> no map for {module} (not one of ours)")
            continue

        rvas, names = load(map_path)
        index = bisect.bisect_right(rvas, rva) - 1
        if index < 0:
            print(f"{arg} -> before the first symbol in {map_path.name}")
            continue
        print(f"{arg} -> {names[index]} +{rva - rvas[index]:#x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
