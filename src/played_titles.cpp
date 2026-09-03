// Reading the profile's played-games history out of its dashboard GPD.
//
// The Game Library used to report every title with zero achievements and zero
// gamerscore, and said so in src/title_library.cpp:
//
//     achievements  zero, and gamerscore zero, because there is no achievement
//                   data for these titles -- no GPD records them
//
// That was true of the profile staged at the time, whose FFFE07D1.gpd held a
// single 24-byte setting of zeros. It is not true of a real profile: the
// dashboard GPD of the one now staged carries 37 title records alongside 35
// settings, and 43 further per-title GPDs sit beside it.
//
// A GPD is an XDBF file: a header, a table of entries tagged with a namespace
// and an id, then the entry data. Namespace 4 is the played-title list, and
// each record has the same shape as the XAM played-title record the Game
// Library hands back -- title id, achievements possible and earned, gamerscore
// total and earned, a FILETIME, then the name as UTF-16BE:
//
//     0x00 title_id              0x14 reserved_achievement_count
//     0x04 achievements_possible 0x16 all_avatar_awards
//     0x08 achievements_earned   0x18 male_avatar_awards
//     0x0C gamerscore_total      0x1A female_avatar_awards
//     0x10 gamerscore_earned     0x1C reserved_flags
//     0x20 last_played           0x28 name (UTF-16BE, NUL terminated)
//
// so this is a copy rather than a translation.
//
// On trusting these numbers
// -------------------------
// The totals here are cross-checked against a second, independent source: the
// per-title GPDs, where each achievement record carries its own gamerscore and
// an unlocked bit. Walking those gives 1270 unlocked worth 26033, against
// 1253 worth 25960 from the title records read here -- the difference being
// the seven title GPDs the dashboard GPD has no title record for. Two
// independently parsed layouts landing that close is what says the offsets
// above are right; tools/profile_summary.py reports both.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/logging.h>

#include <rex/cvar.h>

#include "install_paths.h"
#include "played_titles.h"
#include "profile_list.h"

// Games from Xbox LIVE, merged into the profile's own history.
//
// The history in the GPD is what this console has played: two titles. The
// account has played 47, and titlehub knows all of them with their achievement
// counts and gamerscore. Merging them here rather than showing them somewhere
// else puts them in the Game Library itself -- the same list, the same records,
// the same achievement columns -- because PlayedTitles() is where the library is
// built from.
//
// It also keeps the count honest. The library asks for 0x10040004 and then asks
// the enumerator for that many titles, and the two disagreeing is what the note
// in profile_read.cpp warns about; both come off this list, so they cannot.
//
// The GPD wins on a title both know. It is the console's own record, and it is
// the one with per-achievement detail behind it.
REXCVAR_DEFINE_STRING(library_list, "gamedir/library.txt", "Dashboard",
                      "Pipe-delimited games to merge into the Game Library: "
                      "titleid|name|earned|possible|gamerscore|total|filetime|platform, "
                      "one per line, # for comments. Empty merges nothing. "
                      "tools/fetch_title_stats.py writes it.");

namespace nxe_profile {

// Defined in profile_settings.cpp: the staged profile package's directory.
const std::filesystem::path& ProfileDirectory();

namespace {

constexpr uint16_t kNamespaceTitle = 4;

// Fixed part of a title record, up to the name.
constexpr size_t kTitleRecordHeader = 0x28;

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

// Add the titles in library_list that the GPD did not already account for.
//
// Deduplicated by title id, and the GPD entry is kept when both have one: the
// console's own record is the one with a per-title GPD behind it, so replacing it
// would trade real achievement detail for a summary.
void MergeFromFile(std::vector<PlayedTitle>& titles) {
  const std::string setting = REXCVAR_GET(library_list);
  if (setting.empty()) {
    return;
  }
  const auto path = nxe_paths::Resolve(setting);
  std::ifstream file(path);
  if (!file) {
    REXLOG_INFO("Played titles: no library list at {}", path.string());
    return;
  }

  std::vector<uint32_t> known;
  known.reserve(titles.size());
  for (const auto& title : titles) {
    known.push_back(title.title_id);
  }

  std::string line;
  size_t added = 0;
  while (std::getline(file, line)) {
    while (!line.empty() && (line.back() == 13 || line.back() == 10)) line.pop_back();
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == 35) continue;  // 35 == '#'

    std::string field[8];
    size_t at = 0;
    for (int i = 0; i < 8 && at <= line.size(); ++i) {
      const size_t bar = line.find(124, at);  // 124 == '|'
      field[i] = line.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
      if (bar == std::string::npos) break;
      at = bar + 1;
    }
    if (field[0].empty() || field[1].empty()) continue;

    PlayedTitle title;
    title.title_id = uint32_t(std::strtoul(field[0].c_str(), nullptr, 16));
    if (!title.title_id) continue;
    if (std::find(known.begin(), known.end(), title.title_id) != known.end()) continue;

    title.achievements_earned = uint32_t(std::strtoul(field[2].c_str(), nullptr, 10));
    title.achievements_possible = uint32_t(std::strtoul(field[3].c_str(), nullptr, 10));
    title.gamerscore_earned = uint32_t(std::strtoul(field[4].c_str(), nullptr, 10));
    title.gamerscore_total = uint32_t(std::strtoul(field[5].c_str(), nullptr, 10));
    title.last_played = std::strtoull(field[6].c_str(), nullptr, 10);
    // The name is UTF-8 on disk and UTF-16 in the record. Everything past the
    // ASCII range is dropped to a question mark rather than mangled: a title
    // name is a label here, and half a codepoint would draw as a box.
    for (unsigned char ch : field[1]) {
      title.name.push_back(ch < 0x80 ? char16_t(ch) : char16_t(63));
    }
    known.push_back(title.title_id);
    titles.push_back(std::move(title));
    ++added;
  }
  if (added) {
    REXLOG_INFO("Played titles: {} more from {}", added, path.string());
  }
}

std::vector<PlayedTitle> Parse() {
  std::vector<PlayedTitle> titles;

  const auto& dir = ProfileDirectory();
  if (dir.empty()) {
    REXLOG_INFO("Played titles: no profile staged");
    return titles;
  }

  const auto path = dir / "FFFE07D1.gpd";
  const auto data = ReadFile(path);
  if (data.size() < 24 || std::memcmp(data.data(), "XDBF", 4) != 0) {
    REXLOG_WARN("Played titles: {} is not an XDBF file", path.string());
    return titles;
  }

  const uint32_t entry_table_len = Be32(&data[8]);
  const uint32_t entry_count = Be32(&data[12]);
  const uint32_t free_table_len = Be32(&data[16]);
  const size_t base = 24 + size_t(entry_table_len) * 18 + size_t(free_table_len) * 8;

  size_t off = 24;
  for (uint32_t i = 0; i < entry_count && off + 18 <= data.size(); ++i, off += 18) {
    if (Be16(&data[off]) != kNamespaceTitle) {
      continue;
    }
    const size_t entry_off = base + Be32(&data[off + 10]);
    const size_t entry_len = Be32(&data[off + 14]);
    if (entry_len < kTitleRecordHeader || entry_off + entry_len > data.size()) {
      continue;
    }

    const uint8_t* record = &data[entry_off];

    PlayedTitle title;
    title.title_id = Be32(record);
    // Namespace 4 also carries a couple of bookkeeping records with no title
    // id; they are not games and their fields decode as nonsense.
    if (title.title_id == 0) {
      continue;
    }
    title.achievements_possible = Be32(record + 0x04);
    title.achievements_earned = Be32(record + 0x08);
    title.gamerscore_total = Be32(record + 0x0C);
    title.gamerscore_earned = Be32(record + 0x10);
    title.last_played = Be64(record + 0x20);

    // UTF-16BE up to the terminator or the end of the record.
    for (size_t n = kTitleRecordHeader; n + 1 < entry_len; n += 2) {
      const uint16_t ch = Be16(record + n);
      if (!ch) {
        break;
      }
      title.name.push_back(static_cast<char16_t>(ch));
    }

    titles.push_back(std::move(title));
  }

  MergeFromFile(titles);

  uint32_t earned = 0;
  uint32_t score = 0;
  for (const auto& title : titles) {
    earned += title.achievements_earned;
    score += title.gamerscore_earned;
  }
  REXLOG_INFO("Played titles: {} title(s) from {}, {} achievement(s), {} gamerscore",
              titles.size(), path.string(), earned, score);
  return titles;
}

}  // namespace

const std::vector<PlayedTitle>& PlayedTitles() {
  return nxe_profile::ProfileScoped([] { return Parse(); });
}

}  // namespace nxe_profile
