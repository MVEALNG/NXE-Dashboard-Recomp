// Gamer tiles (the profile picture).
//
// XamReadTileEx ships as a bare REX_EXPORT_STUB, and its implemented sibling
// XamReadTileToTexture just fills the caller's buffer with 0xFF -- so gamer
// tiles rendered as blank white blocks even though the profile on disk carries
// real tile_64.png and tile_32.png images.
//
// The contract comes from the caller at guest 0x922F2C70:
//
//     size = (tile_type == 15) ? 0x10000 : 0x4000;
//     *(obj+80) = size;
//     buffer = malloc(size);
//     result = XamReadTileEx(tile_type, title_id, id_lo, id_hi, flag, user,
//                            buffer, &size);
//     if (result == 997) return E_PENDING;          // ERROR_IO_PENDING
//     if (result >  0  ) return result | 0x80070000; // Win32 -> HRESULT
//     return result;                                 // 0 is success
//
// so zero means success, and the buffer size tells us the expected dimensions:
// 0x4000 is 64x64x4 and 0x10000 is 128x128x4, both 32-bit pixels. The profile
// ships a 64x64 and a 32x32 tile, so anything larger is scaled up rather than
// left partly blank.
//
// Pixel order
// -----------
// The runtime's own XamPngDecode documents the convention this guest expects:
// stb_image produces RGBA, but the dashboard wants big-endian ARGB, and that
// function shuffles per channel rather than memcpy for exactly that reason. The
// same ordering is used here. If tiles ever come out with swapped channels this
// is the one line to change -- the shuffle in WritePixels -- not the decode.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>
#include <rex/ui/image_decode.h>

#include <rex/cvar.h>
#include <rex/string.h>

#include "install_paths.h"
#include "game_icon.h"
#include "gpd_images.h"
#include "title_names.h"
#include "storage_device.h"

REXCVAR_DECLARE(bool, gamer_tile_decoded);

REXCVAR_DEFINE_STRING(game_icon_dir, "assets/icons", "Games",
                      "Folder of 64x64 PNG game icons, named <title id> or <game name>.");

using namespace rex;

namespace {

// Candidate tile files inside a profile package, largest first: a bigger source
// scaled down beats a smaller one scaled up.
constexpr const char* kTileFiles[] = {"tile_64.png", "tile_32.png"};

// The avatar's own picture, which a profile carries beside the gamer tile.
constexpr const char* kAvatarFiles[] = {"avtr_64.png", "avtr_32.png"};

// XamReadTile tile types, as the guest passes them in r3.
enum : uint32_t {
  kTileAchievement = 0,
  kTileGameIcon = 1,
  kTileGamerTile = 2,
  kTileGamerTileSmall = 3,
  kTileLocalGamerTile = 4,
  kTileLocalGamerTileSmall = 5,
  kTileBackground = 6,
  kTileAwardedGamerTile = 7,
  kTileAwardedGamerTileSmall = 8,
  kTileGamerTileByImageId = 9,
  kTilePersonalGamerTile = 10,
  kTilePersonalGamerTileSmall = 11,
  kTileGamerTileByKey = 12,
  kTileAvatarGamerTile = 13,
  kTileAvatarGamerTileSmall = 14,
  kTileAvatarFullBody = 15,
};

bool IsGamerTileType(uint32_t type) {
  switch (type) {
    case kTileGamerTile:
    case kTileGamerTileSmall:
    case kTileLocalGamerTile:
    case kTileLocalGamerTileSmall:
    case kTileAwardedGamerTile:
    case kTileAwardedGamerTileSmall:
    case kTileGamerTileByImageId:
    case kTilePersonalGamerTile:
    case kTilePersonalGamerTileSmall:
    case kTileGamerTileByKey:
      return true;
    default:
      return false;
  }
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  FILE* f = nullptr;
  if (fopen_s(&f, path.string().c_str(), "rb") != 0 || f == nullptr) {
    return data;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    data.resize(static_cast<size_t>(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
      data.clear();
    }
  }
  std::fclose(f);
  return data;
}

// Content/<XUID>/FFFE07D1/00010000/<package>/<name>
template <size_t N>
std::vector<uint8_t> LoadProfilePng(const char* const (&names)[N]) {
  std::error_code ec;
  const auto root = nxe_storage::ContentRoot();
  if (!std::filesystem::exists(root, ec)) {
    return {};
  }
  for (const auto& xuid_dir : std::filesystem::directory_iterator(root, ec)) {
    if (!xuid_dir.is_directory(ec)) continue;
    const auto profiles = xuid_dir.path() / "FFFE07D1" / "00010000";
    if (!std::filesystem::exists(profiles, ec)) continue;
    for (const auto& pkg : std::filesystem::directory_iterator(profiles, ec)) {
      if (!pkg.is_directory(ec)) continue;
      for (const char* name : names) {
        const auto candidate = pkg.path() / name;
        if (std::filesystem::exists(candidate, ec)) {
          auto data = ReadWholeFile(candidate);
          if (!data.empty()) {
            REXKRNL_INFO("Profile image: using {}", candidate.string());
            return data;
          }
        }
      }
    }
  }
  return {};
}

// Decoded once; tiles are small and requested repeatedly.
struct DecodedTile {
  // The file as it was on disk, kept alongside the decoded pixels. The
  // dashboard's own image loader is handed one or the other -- see ServeTile.
  std::vector<uint8_t> png;
  std::vector<uint8_t> rgba;
  int width = 0;
  int height = 0;
};

DecodedTile Decode(const std::vector<uint8_t>& png, const char* what) {
  DecodedTile tile;
  if (png.empty()) {
    return tile;
  }
  tile.png = png;
  tile.rgba = rex::ui::DecodeImageRGBA(png.data(), png.size(), tile.width, tile.height);
  if (tile.rgba.empty()) {
    REXKRNL_WARN("{}: PNG decode failed", what);
  } else {
    REXKRNL_INFO("{}: decoded {}x{}", what, tile.width, tile.height);
  }
  return tile;
}

const DecodedTile& Tile() {
  static const DecodedTile kTile = Decode(LoadProfilePng(kTileFiles), "Gamer tile");
  return kTile;
}

const DecodedTile& AvatarTile() {
  static const DecodedTile kTile = Decode(LoadProfilePng(kAvatarFiles), "Avatar tile");
  return kTile;
}

// Achievement and title icons, decoded on demand and kept: an achievement list
// asks for the same handful repeatedly while it scrolls.
const DecodedTile& GpdTile(uint32_t title_id, uint64_t image_id) {
  static std::mutex mutex;
  static std::map<std::pair<uint32_t, uint64_t>, DecodedTile> cache;

  std::lock_guard<std::mutex> lock(mutex);
  const auto key = std::make_pair(title_id, image_id);
  auto it = cache.find(key);
  if (it == cache.end()) {
    char what[64] = {};
    std::snprintf(what, sizeof(what), "GPD image %08X/%llx", title_id,
                  static_cast<unsigned long long>(image_id));
    it = cache.emplace(key, Decode(nxe_profile::GpdImage(title_id, image_id), what)).first;
  }
  return it->second;
}

// Game icons kept as files.
//
// A console reads a game's icon out of that title's own GPD, which is written
// when the game is first played. Nothing here has ever run these titles, so
// there are no per-title GPDs and the GPD lookup finds nothing -- the rows drew
// without an icon.
//
// Files fill that gap. A 64x64 PNG named either after the title id or after the
// game is loaded and served exactly like a gamer picture, which the dashboard's
// own loader already accepts. The GPD is still tried first, so a title that does
// have one keeps the icon the console recorded.
const DecodedTile& GameIconTile(uint32_t title_id) {
  static std::mutex mutex;
  static std::map<uint32_t, DecodedTile> cache;
  static const DecodedTile kNone;

  std::lock_guard<std::mutex> lock(mutex);
  const auto found = cache.find(title_id);
  if (found != cache.end()) {
    return found->second;
  }

  const std::string dir_text = REXCVAR_GET(game_icon_dir);
  if (dir_text.empty()) {
    return cache.emplace(title_id, DecodedTile{}).first->second;
  }

  std::error_code ec;
  const std::filesystem::path dir = dir_text;
  std::filesystem::path picked;

  // By title id first: it cannot be ambiguous, and it survives a rename.
  char id_name[16] = {};
  std::snprintf(id_name, sizeof(id_name), "%08X.png", title_id);
  if (std::filesystem::exists(dir / id_name, ec)) {
    picked = dir / id_name;
  }

  // Then by the name the game is known by, which is how a folder of icons
  // collected by hand is usually labelled.
  const auto name = rex::string::to_utf8(nxe_title::NameFor(title_id));
  if (picked.empty() && !name.empty()) {
    const auto by_name = dir / (name + ".png");
    if (std::filesystem::exists(by_name, ec)) {
      picked = by_name;
    } else {
      // Tolerate a difference in case, so "halo 3.png" is still found.
      const auto folded = [](std::string s) {
        for (char& c : s) {
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
      };
      const auto wanted = folded(name);
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (folded(entry.path().stem().string()) == wanted) {
          picked = entry.path();
          break;
        }
      }
    }
  }

  DecodedTile tile;
  if (!picked.empty()) {
    tile = Decode(ReadWholeFile(picked), "game icon");
    if (!tile.png.empty()) {
      REXKRNL_INFO("Game icon: title {:#010x} '{}' -> {} ({}x{})", title_id, name,
                   picked.string(), tile.width, tile.height);
    }
  } else {
    REXKRNL_INFO("Game icon: title {:#010x} '{}' has no icon in {}", title_id, name, dir_text);
  }
  return cache.emplace(title_id, std::move(tile)).first->second;
}

// Which image answers a given request.
//
// The dashboard asks for achievement icons and game icons through the same
// entry point as gamer pictures, distinguished only by the tile type in r3, so
// this is the one place that decides what a request means.
const DecodedTile& TileFor(uint32_t tile_type, uint32_t title_id, uint64_t item_id) {
  static const DecodedTile kNone;

  if (IsGamerTileType(tile_type)) {
    return Tile();
  }
  if (tile_type == kTileAvatarGamerTile || tile_type == kTileAvatarGamerTileSmall) {
    return AvatarTile();
  }
  if (tile_type == kTileAchievement) {
    // The id arrives as a 64-bit value; achievement records store the icon id
    // in 32 bits, so accept it from either half rather than assuming which.
    const uint32_t low = static_cast<uint32_t>(item_id);
    const uint32_t high = static_cast<uint32_t>(item_id >> 32);
    const auto& by_low = GpdTile(title_id, low);
    if (!by_low.rgba.empty()) {
      return by_low;
    }
    if (high && high != low) {
      return GpdTile(title_id, high);
    }
    return kNone;
  }
  if (tile_type == kTileGameIcon) {
    const auto& from_gpd = GpdTile(title_id, nxe_profile::kTitleIconImageId);
    if (!from_gpd.rgba.empty()) {
      return from_gpd;
    }
    return GameIconTile(title_id);
  }
  return kNone;
}

// One line per distinct request, so an unserved tile type is visible without
// drowning the log -- these are called hundreds of times a screen.
void LogRequestOnce(const char* who, uint32_t tile_type, uint32_t title_id, uint64_t item_id,
                    const char* outcome) {
  static std::mutex mutex;
  static std::set<std::pair<uint32_t, uint32_t>> seen;

  std::lock_guard<std::mutex> lock(mutex);
  if (!seen.emplace(tile_type, title_id).second) {
    return;
  }
  REXKRNL_INFO("{}: type={} title={:#010x} id={:#x} -> {}", who, tile_type, title_id,
               static_cast<unsigned long long>(item_id), outcome);
}


// Square edge length implied by a 32-bit-pixel buffer, or 0 if it is not square.
uint32_t EdgeFromBufferSize(uint32_t bytes) {
  const uint32_t pixels = bytes / 4;
  uint32_t edge = 0;
  while (edge * edge < pixels) {
    ++edge;
  }
  return edge * edge == pixels ? edge : 0;
}

// Nearest-neighbour into the guest buffer, converting RGBA to big-endian ARGB.
void WritePixels(uint8_t* dst, uint32_t edge, const DecodedTile& tile) {
  for (uint32_t y = 0; y < edge; ++y) {
    const int src_y = static_cast<int>(y * tile.height / edge);
    for (uint32_t x = 0; x < edge; ++x) {
      const int src_x = static_cast<int>(x * tile.width / edge);
      const size_t s = (size_t(src_y) * tile.width + src_x) * 4;
      uint8_t* p = dst + (size_t(y) * edge + x) * 4;
      p[0] = tile.rgba[s + 3];  // A
      p[1] = tile.rgba[s + 0];  // R
      p[2] = tile.rgba[s + 1];  // G
      p[3] = tile.rgba[s + 2];  // B
    }
  }
}

// Shared body: decode the right image and scale it into the guest's buffer.
uint32_t ServeTile(uint32_t tile_type, uint32_t title_id, uint64_t item_id, uint8_t* dst,
                   uint32_t* capacity, const char* who) {
  if (dst == nullptr || capacity == nullptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const DecodedTile& tile = TileFor(tile_type, title_id, item_id);
  if (tile.rgba.empty() || tile.width <= 0 || tile.height <= 0) {
    // Report "not found" rather than leaving the caller with an undefined
    // buffer, which is what the stub did. Every caller handles this.
    LogRequestOnce(who, tile_type, title_id, item_id, "no image");
    return X_ERROR_FILE_NOT_FOUND;
  }

  // Hand back the image file, not decoded pixels.
  //
  // The dashboard loads a tile by pointing its own image loader at the buffer
  // (sub_921C2850, "memory://<addr>,<size>"), and that loader wants an encoded
  // image -- given raw pixels it answers E_INVALIDARG and the gamercard draws an
  // empty box. The failure repeats every frame, which is what a log full of
  //
  //     [theme] load 'memory://9280aad4,4000' -> 0x80070057
  //
  // is: the same tile being retried forever. 0x9280aad4 is exactly the buffer
  // this function was handed (r7 in the XamReadTile trace), so the data was
  // arriving; only its form was wrong.
  //
  // gamer_tile_decoded switches back to writing pixels, in case some caller
  // does want them.
  //
  // The game icon is one caller that always does. It does not go to the loader
  // the gamercard uses; it goes through the dashboard's tile cache at guest
  // 0x921FE560, which describes the buffer to its image loader with a length it
  // hardcodes rather than the one this function reports:
  //
  //     snwprintf(url, 0x40, L"memorykey://%X,%X_%016I64x", entry + 10, 0x4000, key);
  //
  // 0x4000 is 64 * 64 * 4 -- a raw 64x64 ARGB surface. Handing that loader an
  // 8818-byte PNG in a buffer it has been told is 16384 bytes of pixels cannot
  // work, however well formed the file is, which is why the icon was served and
  // still drew nothing. WritePixels already produces exactly that surface.
  //
  // Achievement icons come the same way. Guest 0x9227DBD0 sizes its buffer the
  // same fixed way before asking for one --
  //
  //     *(a1 + 68) = 0x4000;
  //     v7 = sub_92278500(0x4000);
  //     XamReadTile(0, title, id, flags, v7, a1 + 68, a1 + 36);
  //
  // -- so an achievement row drew an empty frame for exactly the same reason a
  // game row did, and is fixed by the same answer.
  const bool wants_pixels = tile_type == kTileGameIcon || tile_type == kTileAchievement;

  if (!wants_pixels && !REXCVAR_GET(gamer_tile_decoded) && !tile.png.empty()) {
    if (tile.png.size() > *capacity) {
      LogRequestOnce(who, tile_type, title_id, item_id, "image larger than the buffer");
      REXKRNL_WARN("{}: {} byte image does not fit a {} byte buffer", who, tile.png.size(),
                   *capacity);
      return X_ERROR_INSUFFICIENT_BUFFER;
    }
    std::memcpy(dst, tile.png.data(), tile.png.size());
    *capacity = static_cast<uint32_t>(tile.png.size());
    LogRequestOnce(who, tile_type, title_id, item_id, "served (image file)");
    return X_ERROR_SUCCESS;
  }

  const uint32_t edge = EdgeFromBufferSize(*capacity);
  if (edge == 0) {
    LogRequestOnce(who, tile_type, title_id, item_id, "buffer not a square 32-bit image");
    REXKRNL_WARN("{}: buffer of {} bytes is not a square 32-bit image", who, *capacity);
    return X_ERROR_INVALID_PARAMETER;
  }

  WritePixels(dst, edge, tile);
  *capacity = edge * edge * 4;
  LogRequestOnce(who, tile_type, title_id, item_id, "served (pixels)");
  return X_ERROR_SUCCESS;
}

u32 XamReadTileEx_entry(u32 tile_type, u32 title_id, u32 id_lo, u32 id_hi, u32 flag, u32 user_index,
                        mapped_void buffer, mapped_u32 buffer_size) {
  (void)flag;
  (void)user_index;

  auto* dst = buffer.as<uint8_t*>();
  if (dst == nullptr || !buffer_size) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const uint64_t item_id = (static_cast<uint64_t>(id_hi) << 32) | id_lo;
  uint32_t capacity = *buffer_size;
  const uint32_t result = ServeTile(tile_type, title_id, item_id, dst, &capacity, "XamReadTileEx");
  if (result == X_ERROR_SUCCESS) {
    *buffer_size = capacity;
  }
  return result;
}

}  // namespace

// DELIBERATELY NOT EXPORTED -- see below.
//
// This implementation works: it finds the profile's tile, decodes it, and hands
// back the right pixels. The log shows exactly that:
//
//     Gamer tile: using .../E000683F000088EB/tile_64.png
//     Gamer tile: decoded 64x64
//     XamReadTileEx(type=2) -> 64x64 tile scaled to 64x64
//
// Enabling it nonetheless makes the dashboard markedly less stable, and that was
// measured rather than guessed -- same build, only this export toggled:
//
//     export ON    3 of 4 runs aborted, every run ~350 log lines
//     export OFF   3 of 3 runs clean, 6793 / 164 / 5011 lines
//
// The abort is not in this code. It lands in XamUserReadProfileSettingsEx,
// reached from guest 0x922F2710, inside the runtime's setting serialisation --
// the ByteStream in user_profile.h asserts on writing past the caller's buffer.
// Returning a real tile is simply what lets the dashboard get far enough to
// build a gamercard, which then reads profile settings and trips that bug.
//
// So this is a working feature blocked behind a separate defect, not a broken
// feature.
//
// That defect is now fixed, so this is exported again. The abort was
// XamUserReadProfileSettingsEx sizing its reply from the length encoded in a
// setting id while Append wrote the stored value's real length, running
// SettingByteStream past the end of the caller's buffer -- see profile_read.cpp,
// which now sizes from the actual payload and additionally refuses to write a
// setting that will not fit. Nothing in this file changed; it was never at
// fault.
//
// If instability returns with this on, the measurement to repeat is the one
// above: same build, this export toggled, several runs each. Blank tiles beat an
// unstable dashboard, and that trade was right while the overflow stood.
namespace nxe_tile {

const std::vector<uint8_t>& GameIconPng(uint32_t title_id) { return GameIconTile(title_id).png; }

}  // namespace nxe_tile

REX_EXPORT(__imp__XamReadTileEx, XamReadTileEx_entry)

// XamReadTile -- the non-Ex sibling, and the reason for a retry spin.
//
// It ships as a bare REX_EXPORT_STUB, and the logs show it being called 1624
// times in a single session. That is not a busy screen; that is a caller
// retrying, because an undefined r3 sometimes reads as ERROR_IO_PENDING (997)
// and the callers treat 997 as "ask again".
//
// The contract is from guest 0x921FCD50, the gamercard load:
//
//     v16[0] = 0x4000;
//     result = XamReadTile(10, 0, v10, 254, v9, v16, 0);
//     if ( result ) { memset(v9, 0, 0x4000); result = 0; }   // handles failure
//
// The register map, measured rather than assumed. An earlier version of this
// comment had the XUID consuming an aligned register PAIR, by analogy with
// XamUserCreateTitlesPlayedEnumerator, and placed the buffer and size one
// register too far along. Logging r3-r10 plus the words they point at, once per
// distinct tile type, settles it -- two unrelated call sites agree:
//
//     type 10  r3=a r4=0        r5=babebabe r6=fe r7=9280aad4 r8=703ff990 r9=0
//     type  1  r3=1 r4=58410b0f r5=8000     r6=0  r7=40468f78 r8=927a9c84 r9=927a9c88
//
// with [r8] reading 0x4000 in both -- which is the "v16[0] = 0x4000" the call
// above sets up. So the XUID occupies one register here, not a pair:
//
//     r3 tile_type   r4 title_id   r5 id / xuid low   r6 flags
//     r7 buffer      r8 &size      r9 overlapped
//
// Reading r8/r9 as buffer and size instead gave a size of zero and every
// request was refused with "buffer of 0 bytes is not a square 32-bit image".
//
// What it reports, and why
// ------------------------
// This used to answer a flat "no tile" for everything. That was deliberate:
// serving tiles had been measured to destabilise the dashboard, and a definite
// failure at least ended the retry loop instead of feeding it. The instability
// was later traced to a buffer overrun in XamUserReadProfileSettingsEx and
// fixed (see profile_read.cpp), and XamReadTileEx was re-enabled on the back of
// that -- but this sibling was left reporting failure, so the gamercard, which
// calls *this* one, still drew no picture.
//
// It now serves the same images XamReadTileEx does. A request it has nothing
// for still reports "not found", which the caller above handles by blanking its
// own buffer.
REX_HOOK_RAW(__imp__XamReadTile) {
  {
    // Per distinct tile type, once: the argument layout is not the same at
    // every call site and a wrong guess shows up as a zero buffer size.
    static std::mutex mutex;
    static std::set<uint32_t> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.emplace(ctx.r3.u32).second) {
      uint32_t size_at_r9 = 0;
      uint32_t size_at_r8 = 0;
      if (ctx.r9.u32) {
        size_at_r9 = *reinterpret_cast<rex::be<uint32_t>*>(base + ctx.r9.u32);
      }
      if (ctx.r8.u32) {
        size_at_r8 = *reinterpret_cast<rex::be<uint32_t>*>(base + ctx.r8.u32);
      }
      REXKRNL_INFO(
          "XamReadTile regs: r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} r8={:#x} r9={:#x} "
          "r10={:#x} [r9]={:#x} [r8]={:#x}",
          ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
          ctx.r10.u32, size_at_r9, size_at_r8);
    }
  }

  const uint32_t tile_type = ctx.r3.u32;
  const uint32_t title_id = ctx.r4.u32;
  const uint64_t item_id = ctx.r5.u32;
  const uint32_t buffer_ptr = ctx.r7.u32;
  const uint32_t size_ptr = ctx.r8.u32;

  if (!buffer_ptr || !size_ptr) {
    ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
    return;
  }

  auto* size = reinterpret_cast<rex::be<uint32_t>*>(base + size_ptr);
  uint32_t capacity = *size;
  const uint32_t result =
      ServeTile(tile_type, title_id, item_id, base + buffer_ptr, &capacity, "XamReadTile");
  if (result == X_ERROR_SUCCESS) {
    *size = capacity;
  }
  ctx.r3.u64 = result;
}
