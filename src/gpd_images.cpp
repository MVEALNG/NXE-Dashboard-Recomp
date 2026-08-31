// Pulling achievement and title icons out of the profile's GPDs.
//
// Each title the profile has played has a <title_id>.gpd beside the dashboard's
// own FFFE07D1.gpd, and those files carry the artwork the dashboard draws:
// namespace 2 of the XDBF container is a set of PNGs keyed by id, and each
// achievement record in namespace 1 names the id of its icon at +0x08.
//
//     454109C9.gpd   67 achievements, 40 images   (PvZ Garden Warfare)
//     545408A7.gpd   51 achievements,  1 image    (GTA V -- icons never synced)
//
// So icons are available per title rather than universally. A title whose GPD
// was written without them carries only 0x8000, its own icon, and there is
// nothing to draw for its achievements -- which is the same thing a console
// shows before it has fetched them.
//
// Whole GPDs are cached rather than individual images. They are read once per
// title and the largest here is 515 KB, while an achievement list asks for
// dozens of icons from the same file in a row.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#include <rex/logging.h>

#include "gpd_images.h"
#include "storage_device.h"

namespace nxe_profile {

// Defined in profile_settings.cpp: the staged profile package's directory.
const std::filesystem::path& ProfileDirectory();

namespace {

constexpr uint16_t kNamespaceImage = 2;

uint16_t Be16(const uint8_t* p) {
  return static_cast<uint16_t>((uint32_t(p[0]) << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t Be64(const uint8_t* p) {
  return (static_cast<uint64_t>(Be32(p)) << 32) | Be32(p + 4);
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || !size) {
    return data;
  }
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    return data;
  }
  data.resize(static_cast<size_t>(size));
  if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
    data.clear();
  }
  std::fclose(f);
  return data;
}

// One title's images, keyed by the id the achievement records use.
using ImageMap = std::map<uint64_t, std::vector<uint8_t>>;

ImageMap ParseImages(uint32_t title_id) {
  ImageMap images;

  const auto& dir = ProfileDirectory();
  if (dir.empty()) {
    return images;
  }

  char name[16] = {};
  std::snprintf(name, sizeof(name), "%08X.gpd", title_id);
  const auto path = dir / name;

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return images;
  }

  const auto data = ReadFile(path);
  if (data.size() < 24 || std::memcmp(data.data(), "XDBF", 4) != 0) {
    return images;
  }

  const uint32_t entry_table_len = Be32(&data[8]);
  const uint32_t entry_count = Be32(&data[12]);
  const uint32_t free_table_len = Be32(&data[16]);
  const size_t base = 24 + size_t(entry_table_len) * 18 + size_t(free_table_len) * 8;

  size_t off = 24;
  for (uint32_t i = 0; i < entry_count && off + 18 <= data.size(); ++i, off += 18) {
    if (Be16(&data[off]) != kNamespaceImage) {
      continue;
    }
    const uint64_t id = Be64(&data[off + 2]);
    const size_t entry_off = base + Be32(&data[off + 10]);
    const size_t entry_len = Be32(&data[off + 14]);
    if (!entry_len || entry_off + entry_len > data.size()) {
      continue;
    }
    images[id].assign(data.begin() + entry_off, data.begin() + entry_off + entry_len);
  }

  REXLOG_INFO("GPD images: {} image(s) in {}", images.size(), path.string());
  return images;
}

// Icons the profile never synced, fetched from Xbox Live's image server by
// tools/fetch_achievement_icons.py and left beside the storage device rather
// than written back into the GPDs, which stay exactly as the console wrote
// them:
//
//     A:/Xbox360Storage/Cache/achievement_icons/<TITLEID>/<imageid>.png
//
// 26 of the 44 title GPDs here carry no achievement icons at all and 18 carry
// only some, so without this most achievement lists draw blank.
std::vector<uint8_t> LoadCachedIcon(uint32_t title_id, uint64_t image_id) {
  char relative[64] = {};
  std::snprintf(relative, sizeof(relative), "%08X/%llu.png", title_id,
                static_cast<unsigned long long>(image_id));

  const auto path = nxe_storage::Root() / "Cache" / "achievement_icons" / relative;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return {};
  }
  auto data = ReadFile(path);
  if (!data.empty()) {
    REXLOG_INFO("GPD images: {:08X}/{} from the icon cache", title_id,
                static_cast<unsigned long long>(image_id));
  }
  return data;
}

std::mutex g_mutex;
std::map<uint32_t, ImageMap> g_cache;

}  // namespace

// The GPD file itself, cached per title.
//
// ParseImages already reads and discards it; the achievement list needs the
// same bytes for namespace 1, so keep them rather than reading the file twice.
const std::vector<uint8_t>& ReadTitleGpd(uint32_t title_id) {
  static std::mutex mutex;
  static std::map<uint32_t, std::vector<uint8_t>> cache;
  static const std::vector<uint8_t> kNone;

  std::lock_guard<std::mutex> lock(mutex);
  const auto found = cache.find(title_id);
  if (found != cache.end()) {
    return found->second;
  }

  const auto& dir = ProfileDirectory();
  if (dir.empty()) {
    return kNone;
  }
  char name[16] = {};
  std::snprintf(name, sizeof(name), "%08X.gpd", title_id);
  const auto path = dir / name;

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return cache.emplace(title_id, std::vector<uint8_t>{}).first->second;
  }
  auto data = ReadFile(path);
  REXLOG_INFO("Title GPD: {} ({} bytes)", path.string(), data.size());
  return cache.emplace(title_id, std::move(data)).first->second;
}

const std::vector<uint8_t>& GpdImage(uint32_t title_id, uint64_t image_id) {
  static const std::vector<uint8_t> kEmpty;

  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_cache.find(title_id);
  if (it == g_cache.end()) {
    it = g_cache.emplace(title_id, ParseImages(title_id)).first;
  }

  auto image = it->second.find(image_id);
  if (image == it->second.end()) {
    // Not in the GPD. Fall back to the icon cache, then remember the answer
    // either way so a miss costs one stat rather than one per redraw.
    auto fetched = LoadCachedIcon(title_id, image_id);
    image = it->second.emplace(image_id, std::move(fetched)).first;
  }
  return image->second.empty() ? kEmpty : image->second;
}

}  // namespace nxe_profile
