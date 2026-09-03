# NXE Dashboard Recompilation

A static recompilation of the Xbox 360 NXE dashboard (2.0.9199.0) to native
Windows, built on the [ReXGlue](https://github.com/) SDK. The dashboard's
PowerPC code is translated to C++ ahead of time; the code in `src/` is the layer
that makes it run on a PC — kernel hooks, storage, profiles, the game library,
launching titles, and the parts of the shell that have no PC equivalent.

## What this repository does not contain

**No Microsoft code or data, and no game content.** To run this you supply your
own files:

- A 9199 system update dump (`$flash_dash.xex` and the packages beside it)
- Your own Xbox 360 profile and content, if you want your gamertag and saves
- Cover art and icons for your own games

`generated/` — the several hundred megabytes of C++ that rexglue produces from
`$flash_dash.xex` — is deliberately not committed. It is derived from
Microsoft's executable, and it is reproducible in one command from a dump you
already have.

## Prerequisites

- Windows, CMake 3.25+, Clang (LLVM), Ninja
- The ReXGlue SDK, built and installed
- A 9199 `$SystemUpdate` dump

### SDK patches

`patches/` holds changes this project needs in the SDK. Four are genuine runtime
bugs found while building this, and the dashboard will not work correctly
without them:

| Patch | What it fixes |
| --- | --- |
| `xboxkrnl_io_cpp_trailingsep.patch` | `NtCreateFile` refusing a path with a trailing separator |
| `entry_cpp_emptycomponent.patch` | `Entry::ResolvePath` failing on an empty component, so no mount root could ever resolve |
| `virtual_file_system_cpp_mountroot.patch` | `OpenFile` asking a parent for a child that is itself a mount point |
| `xex_module_cpp_retryalloc.patch` | The two-key XEX retry leaking the image reservation, so the second key was never really tried |
| `xboxkrnl_memory_cpp_debugmemory.patch` | Devkit-memory asserts killing the process where the allocator only warns |
| `xam_avatar_cpp_byxuid.patch` | Avatar asset pack selection |

Apply them against the SDK checkout before building it:

```
cd <rexglue-sdk>
git apply <this-repo>/patches/*.patch
```

## Building

Generate the recompiled sources from your dump, then build:

```
rexglue codegen nxe_dash_manifest.toml
cmake --preset win-amd64-debug
cmake --build out/build/win-amd64-debug --target nxe_dash
```

`nxe_dash_manifest.toml` points at the dump; edit its paths to match yours.

## Laying out the data

Paths are resolved against the **installation root** — the directory the
executable sits in, unless `nxe_root` says otherwise. A working install looks
like:

```
NXE.exe
gamedir/        staged game directory: default.xex, the .xzp packages, fonts
gamedir/sharedres/  loose overrides for shared resources
storage/        Content/, xconfig.bin -- the console storage tree
assets/covers/  cover art, named <titleid>.png
assets/icons/   game icons, named <titleid>.png
```

Anything absolute in a config file or on the command line wins over these, so
the data can live wherever you like:

```
NXE.exe --storage_root="D:/Xbox360Storage" --game_art_dir="D:/covers"
```

### Optional settings

| Setting | Purpose |
| --- | --- |
| `game_emulator` | Emulator run when a game is launched. Empty disables launching. |
| `boot_video` | Video played over the loading dashboard. Empty disables it. |
| `avatar_editor_exe` | The avatar editor recompilation, a separate project. |
| `guide_enable` | Route the Guide into real recompiled XAM. Off: it corrupts the heap during start-up. |

Note that `NXE.toml` is rewritten from cvar state when the dashboard exits, so
hand-edited entries in it do not survive a run. Use the command line for
anything you want to stick.

## First-run setup

On a new install the dashboard reports missing paths instead of failing
silently. The setup dialog can be opened during first run, and can be opened
again with `F7` when paths or tools need to be changed. It covers the profile,
themes, avatar data, staged games, artwork, and optional launch helpers.

The Game Library can contain both Xbox 360 and PC games:

- Put Xbox 360 game folders containing `default.xex` under `roms_dir`, then
	refresh the library. The title ID is read from the XEX execution metadata;
	the game is staged without copying its files.
- Point `full_games_dir` at a folder of PC game directories or a Steam library.
	Steam manifests are read for names and app IDs; ordinary folders use a
	detected executable. PC titles launch directly, while Xbox 360 titles use
	`game_emulator`.

## Data tools

The scripts in `tools/` import local profile data and retrieve optional Xbox
Live, marketplace, achievement, social, cover, and video data. The combined
workflow is:

```
python tools/xbl_auth.py
python tools/sync_all.py
python tools/import_profile.py
python tools/import_gamerpics.py
python tools/import_genre_cards.py
```

Run individual `fetch_*.py` scripts when only one data set is needed. The Xbox
Live token is cached in `tools/.xbl_token.json`; the video importer can use a
TMDB key in `tools/.tmdb_key`. Both files are ignored by Git and must never be
committed. Review each script's `--help` output for source and destination
overrides before importing data.

## State of things

Working: boot, themes, profile and gamercard, the game library with real sizes
and artwork, achievements with icons, launching titles through an emulator and
returning, the disc tile with cover art, Discord rich presence.

Partial: the Guide. Pressing Messages opens a real dashboard scene rather than
XAM's Guide — the authentic blades live in `huduiskin.xex`, which this runtime
cannot decrypt. Running real XAM gets through start-up but corrupts the
runtime's heap, so it stays behind `guide_enable`.

## Licence

The code in `src/`, `tools/` and `patches/` is provided under the terms in
`LICENSE`. It carries no Microsoft code and no game content, and nothing here
distributes either.
