// Box art for a title. See cover_art.h.

#include "install_paths.h"
#include "cover_art.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(game_art_dir, "assets/covers", "Games",
                      "Folder of cover art PNGs named by title id.");

namespace nxe_art {
namespace {

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || !size) {
    return {};
  }
  std::vector<uint8_t> data(static_cast<size_t>(size));
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    return {};
  }
  if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
    data.clear();
  }
  std::fclose(f);
  return data;
}

}  // namespace

const std::vector<uint8_t>& CoverFor(uint32_t title_id, size_t limit) {
  static std::mutex mutex;
  static std::map<uint32_t, std::vector<uint8_t>> cache;
  static const std::vector<uint8_t> kNone;

  std::lock_guard<std::mutex> lock(mutex);
  const auto found = cache.find(title_id);
  if (found != cache.end()) {
    return found->second;
  }

  const std::string dir = REXCVAR_GET(game_art_dir);
  if (dir.empty() || !title_id) {
    return kNone;
  }

  char name[16] = {};
  std::snprintf(name, sizeof(name), "%08X", title_id);

  // Full size first; the fitted copy only when it will not fit. Anything still
  // over the limit is refused rather than truncated -- half a PNG is not an
  // image, and the loader would reject it and leave the shell retrying.
  std::vector<uint8_t> art;
  for (const char* suffix : {".png", ".thumb.png"}) {
    const auto candidate = std::filesystem::path(dir) / (std::string(name) + suffix);
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec)) {
      continue;
    }
    auto data = ReadFile(candidate);
    if (!data.empty() && data.size() <= limit) {
      REXLOG_INFO("Cover art: {} ({} bytes, limit {})", candidate.string(), data.size(), limit);
      art = std::move(data);
      break;
    }
    REXLOG_INFO("Cover art: {} is {} bytes, over the {} byte limit; trying a smaller one",
                candidate.string(), data.size(), limit);
  }

  return cache.emplace(title_id, std::move(art)).first->second;
}

}  // namespace nxe_art
