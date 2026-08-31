// Where sharedres:// actually goes.
//
// Every sharedres:// load in this port fails -- twenty thousand attempts in a
// single run, the button glyphs, the gamerscore icon and the unearned
// achievement placeholder among them, all with 0x80300013.
//
// The chain is short. Guest 0x92140318 matches the URL prefix and hands the
// remainder to XamBuildLegacySystemResourceLocator:
//
//     if ( !strncmp(url, "sharedres://", 12) )
//         XamBuildLegacySystemResourceLocator(url + 24, out, 128);
//
// The runtime builds "file://media:/shrdres.xzp#<name>" from that, and
// shrdres.xzp is present in the game directory in the same XUIZ format as the
// packages that do work. The failure is further in: guest 0x921B8BA0 looks the
// resource name up in the loaded package's table and returns 0x80300013 when it
// is not there --
//
//     if ( sub_921B53F8(table, name) == -1 )  v6 = -2144337901;  // 0x80300013
//
// -- so the package opens and the *name* does not match. This logs both halves,
// once per distinct request, so the name being asked for and the locator being
// built can be compared against what the package actually contains.
//
// It reimplements the locator rather than wrapping it because a REX_EXPORT
// override replaces the runtime's version outright; the string built here is
// byte for byte what keXamBuildResourceLocator produces.

#include <cstdint>
#include <filesystem>
#include <system_error>
#include <set>
#include <string>

#include <rex/cvar.h>

#include "install_paths.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/types.h>

using namespace rex;

// Where a supplied resource is looked for before the package is consulted.
//
// Its own cvar rather than the runtime's game_data_root, because that one is
// defined inside the runtime DLL and GetFlagByName does not reach it from here
// -- the lookup came back empty and every request fell through to the package.
REXCVAR_DEFINE_STRING(sharedres_dir,
                      "gamedir/sharedres", "Resources",
                      "Folder of loose shared resources, tried before shrdres.xzp.");

namespace {

// A NUL-terminated UTF-16BE guest string.
std::u16string GuestWide(const uint8_t* base, uint32_t address, size_t limit) {
  std::u16string out;
  if (!address) {
    return out;
  }
  const auto* p = reinterpret_cast<const be<uint16_t>*>(base + address);
  for (size_t i = 0; i < limit; ++i) {
    const uint16_t ch = p[i];
    if (!ch) {
      break;
    }
    out.push_back(static_cast<char16_t>(ch));
  }
  return out;
}

void WriteWide(uint8_t* base, uint32_t address, const std::u16string& text, uint32_t capacity) {
  if (!address || !capacity) {
    return;
  }
  auto* p = reinterpret_cast<be<uint16_t>*>(base + address);
  const size_t count = text.size() + 1 < capacity ? text.size() : capacity - 1;
  for (size_t i = 0; i < count; ++i) {
    p[i] = static_cast<uint16_t>(text[i]);
  }
  p[count] = 0;
}

uint32_t BuildLocator(PPCContext& ctx, uint8_t* base, const char16_t* container) {
  const uint32_t name_ptr = ctx.r3.u32;
  const uint32_t out_ptr = ctx.r4.u32;
  const uint32_t capacity = ctx.r5.u32;

  const std::u16string name = GuestWide(base, name_ptr, 128);
  const auto utf8_name = rex::string::to_utf8(name);

  // A loose file wins over the package.
  //
  // Some of what the dashboard asks for through sharedres:// is simply not in
  // this dump. unearnedAchievement.png -- the tile every locked achievement
  // draws, and the reason those rows show an empty frame -- appears in none of
  // the forty packages, nor in any other file in the game directory. There is
  // nothing to extract, so it is supplied as a file instead, and anything
  // dropped into <game dir>/sharedres/ is picked up the same way.
  //
  // This also sidesteps the package lookup, which is broken independently:
  // guest 0x921B8BA0 fails the name lookup inside a package it has already
  // loaded and returns 0x80300013, and it does that even for names the package
  // demonstrably contains. The plain-file form instead takes the file:// path
  // that already works here -- the theme wallpapers load through it and return
  // 0x0.
  std::u16string locator;
  bool from_file = false;
  const std::string root = REXCVAR_GET(sharedres_dir);
  if (!root.empty() && !name.empty()) {
    std::error_code ec;
    const auto candidate = std::filesystem::path(root) / utf8_name;
    if (std::filesystem::exists(candidate, ec)) {
      locator = u"file://media:/sharedres/";
      locator += name;
      from_file = true;
    }
  }
  if (!from_file) {
    locator = u"file://media:/";
    locator += container;
    locator += u".xzp#";
    locator += name;
  }

  WriteWide(base, out_ptr, locator, capacity);

  static std::set<std::string> seen;
  if (seen.size() < 40 && seen.insert(utf8_name).second) {
    REXKRNL_INFO("ResourceLocator: '{}' -> '{}'{}", utf8_name, rex::string::to_utf8(locator),
                 from_file ? "  [supplied as a file]" : "");
  }
  return 0;
}

}  // namespace

extern "C" {

void __imp__XamBuildLegacySystemResourceLocator(PPCContext& __restrict ctx, uint8_t* base) {
  ctx.r3.u64 = BuildLocator(ctx, base, u"shrdres");
}

void __imp__XamBuildSharedSystemResourceLocator(PPCContext& __restrict ctx, uint8_t* base) {
  ctx.r3.u64 = BuildLocator(ctx, base, u"shrdres");
}

}  // extern "C"
