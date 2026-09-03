// The achievements shown against a game in the library.
//
// The dashboard asks for them at guest 0x922F24B8:
//
//     result = XamUserCreateAchievementEnumerator(title, user, xuid, 0, count,
//                                                 &buffer_size, &handle, ovl);
//     buf = malloc(buffer_size);
//     XamEnumerate(handle, 2, buf, buffer_size, 0, overlapped);
//
// The runtime's own implementation cannot serve that call, for two reasons.
//
// It reads the wrong registers. The XUID is 64-bit and consumes an aligned
// register PAIR, so every argument after it arrives one register early -- the
// same shift the played-title enumerator needed a raw hook for, see
// title_library.cpp. A typed hook cannot express it.
//
// And it answers from the runtime's global achievement store, which is loaded
// once for the running title. The dashboard is asking about a different title,
// so that store is the wrong source even when it is populated. A console reads
// the title's own GPD, which is what this does: <title id>.gpd beside the
// dashboard's own, achievement records in namespace 1, artwork in namespace 2.
// The record layout was read off the real GPDs on this drive rather than
// assumed -- the same approach played_titles.cpp took for title records.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "install_paths.h"
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "game_launch.h"
#include "gpd_images.h"

using namespace rex;
using namespace rex::system;

namespace {

// What XamEnumerate hands back per achievement. 36 bytes; the strings live in a
// block after the fixed records, when the caller asks for any.
#pragma pack(push, 1)
struct X_ACHIEVEMENT_DETAILS {
  be<uint32_t> id;
  be<uint32_t> label_ptr;
  be<uint32_t> description_ptr;
  be<uint32_t> unachieved_ptr;
  be<uint32_t> image_id;
  be<uint32_t> gamerscore;
  be<uint32_t> unlock_time_lo;
  be<uint32_t> unlock_time_hi;
  be<uint32_t> flags;
};
#pragma pack(pop)
static_assert(sizeof(X_ACHIEVEMENT_DETAILS) == 36, "achievement record is 36 bytes");

// Achievements for titles with no GPD of their own.
//
// A per-title GPD exists only for a game this console has played. The titles
// merged into the library out of the account's LIVE history have none, and
// without this they arrive with a count in the list and an empty page behind
// it -- which is worse than not listing them, because the number promises
// something the page then fails to show.
REXCVAR_DEFINE_STRING(achievement_data, "gamedir/achdata.txt", "Dashboard",
                      "Pipe-delimited achievements for titles with no GPD: "
                      "titleid|id|gamerscore|flags|imageid|name|description|locked, "
                      "one per line, # for comments. Empty disables the fallback. "
                      "tools/fetch_achievements.py writes it.");

constexpr size_t kStringBufferSize = 464;
constexpr uint16_t kNamespaceAchievement = 1;
constexpr uint32_t kMaxAchievements = 512;

struct Achievement {
  uint32_t id = 0;
  uint32_t image_id = 0;
  uint32_t gamerscore = 0;
  uint32_t flags = 0;
  uint64_t unlock_time = 0;
  std::u16string label;
  std::u16string description;
  std::u16string unachieved;
};

uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>(uint16_t(p[0]) << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

// A NUL-terminated UTF-16BE string, bounded by the record it sits in.
std::u16string ReadString(const uint8_t* data, size_t size, size_t& offset) {
  std::u16string out;
  while (offset + 1 < size) {
    const uint16_t ch = Be16(data + offset);
    offset += 2;
    if (!ch) {
      break;
    }
    out.push_back(static_cast<char16_t>(ch));
  }
  return out;
}

// The title's achievements, straight out of its GPD:
//
//   0x00 struct size (0x1C)   0x0C gamerscore
//   0x04 achievement id       0x10 flags
//   0x08 image id             0x14 unlock time (FILETIME)
//   0x1C label, description and locked description, UTF-16BE, NUL separated
// The same records, out of a text file instead of an XDBF one.
//
// flags is copied through rather than rebuilt: the service states it, and it is
// the field the console reads to decide whether an achievement is earned, shown
// online, or secret. Halo 3's "Landfall" arrives as 0x12000E and means exactly
// what it would have meant in the GPD.
std::vector<Achievement> ParseAchievementsFromFile(uint32_t title_id) {
  std::vector<Achievement> found;
  const std::string setting = REXCVAR_GET(achievement_data);
  if (setting.empty()) {
    return found;
  }
  std::ifstream file(nxe_paths::Resolve(setting));
  if (!file) {
    return found;
  }

  char wanted[9] = {};
  std::snprintf(wanted, sizeof(wanted), "%08X", title_id);

  std::string line;
  while (std::getline(file, line)) {
    while (!line.empty() && (line.back() == 13 || line.back() == 10)) line.pop_back();
    if (line.empty() || line[0] == 35) continue;  // 35 == '#'
    if (line.compare(0, 8, wanted) != 0 || line.size() < 9 || line[8] != 124) {
      continue;  // 124 == '|'; a cheap prefix test before splitting
    }

    std::string field[8];
    size_t at = 0;
    for (int i = 0; i < 8 && at <= line.size(); ++i) {
      const size_t bar = line.find(124, at);
      field[i] = line.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
      if (bar == std::string::npos) break;
      at = bar + 1;
    }

    Achievement a;
    a.id = uint32_t(std::strtoul(field[1].c_str(), nullptr, 10));
    a.gamerscore = uint32_t(std::strtoul(field[2].c_str(), nullptr, 10));
    a.flags = uint32_t(std::strtoul(field[3].c_str(), nullptr, 10));
    a.image_id = uint32_t(std::strtoul(field[4].c_str(), nullptr, 10));
    a.unlock_time = 0;  // the service reports 1753 for these, which is "unknown"
    // UTF-8 on disk, UTF-16 in the record. Anything outside ASCII becomes a
    // question mark rather than half a codepoint, which would draw as a box.
    auto widen = [](const std::string& text) {
      std::u16string out;
      out.reserve(text.size());
      for (unsigned char ch : text) out.push_back(ch < 0x80 ? char16_t(ch) : char16_t(63));
      return out;
    };
    a.label = widen(field[5]);
    a.description = widen(field[6]);
    a.unachieved = widen(field[7].empty() ? field[6] : field[7]);
    found.push_back(std::move(a));
    if (found.size() >= kMaxAchievements) break;
  }

  std::sort(found.begin(), found.end(),
            [](const Achievement& a, const Achievement& b) { return a.id < b.id; });
  return found;
}

std::vector<Achievement> ParseAchievements(uint32_t title_id) {
  std::vector<Achievement> found;
  const auto data = nxe_profile::ReadTitleGpd(title_id);
  if (data.size() < 24 || std::memcmp(data.data(), "XDBF", 4) != 0) {
    return ParseAchievementsFromFile(title_id);
  }

  const uint32_t entry_capacity = Be32(&data[8]);
  const uint32_t entry_count = Be32(&data[12]);
  const uint32_t free_capacity = Be32(&data[16]);
  const size_t base = 24 + size_t(entry_capacity) * 18 + size_t(free_capacity) * 8;

  size_t off = 24;
  for (uint32_t i = 0; i < entry_count && off + 18 <= data.size(); ++i, off += 18) {
    if (Be16(&data[off]) != kNamespaceAchievement) {
      continue;
    }
    const size_t rec_off = base + Be32(&data[off + 10]);
    const size_t rec_len = Be32(&data[off + 14]);
    if (rec_len <= 0x1C || rec_off + rec_len > data.size()) {
      continue;  // namespace 1 also carries sync records, which are not achievements
    }
    const uint8_t* r = &data[rec_off];

    Achievement a;
    a.id = Be32(r + 0x04);
    a.image_id = Be32(r + 0x08);
    a.gamerscore = Be32(r + 0x0C);
    a.flags = Be32(r + 0x10);
    a.unlock_time = Be64(r + 0x14);
    size_t s = 0x1C;
    a.label = ReadString(r, rec_len, s);
    a.description = ReadString(r, rec_len, s);
    a.unachieved = ReadString(r, rec_len, s);
    found.push_back(std::move(a));
  }

  if (found.empty()) {
    // A GPD that exists but records nothing is the same problem as no GPD.
    return ParseAchievementsFromFile(title_id);
  }

  std::sort(found.begin(), found.end(),
            [](const Achievement& a, const Achievement& b) { return a.id < b.id; });
  return found;
}

// Writes the fixed records, and the strings after them when any were asked for.
// The strings are always written, whatever the caller asked for.
//
// The runtime's version keys them off the detail flags -- bit 1 the label, 2 the
// description, 4 the locked description -- and writes a null pointer for any not
// requested. This caller passes 0 and then dereferences the pointers anyway:
// with them left null the dashboard faulted reading guest address 0
//
//     code 0xC0000005  access read  fault address 0x0000000100000000
//
// which is the guest null translated into host space. Since the size this hook
// reports through pcbBuffer is what the caller then allocates, making room for
// the strings unconditionally is free, and it is the only form of the record
// this caller survives.
class AchievementEnumerator : public XEnumerator {
 public:
  AchievementEnumerator(KernelState* kernel_state, size_t items_per_enumerate, uint32_t flags,
                        std::vector<Achievement> items)
      : XEnumerator(kernel_state, items_per_enumerate,
                    sizeof(X_ACHIEVEMENT_DETAILS) + kStringBufferSize),
        flags_(flags),
        items_(std::move(items)) {}

  size_t total() const { return items_.size(); }

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    const size_t count = std::min(items_.size() - current_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    auto* details = reinterpret_cast<X_ACHIEVEMENT_DETAILS*>(buffer_data);
    const size_t string_offset = items_per_enumerate() * sizeof(X_ACHIEVEMENT_DETAILS);
    uint32_t string_ptr = buffer_ptr + static_cast<uint32_t>(string_offset);
    uint8_t* string_data = buffer_data + string_offset;
    size_t remaining = count * kStringBufferSize;

    const auto append = [&](const std::u16string& text) -> uint32_t {
      const size_t bytes = (text.size() + 1) * 2;
      if (bytes > remaining) {
        return 0;
      }
      for (size_t i = 0; i < text.size(); ++i) {
        string_data[i * 2] = static_cast<uint8_t>(text[i] >> 8);
        string_data[i * 2 + 1] = static_cast<uint8_t>(text[i] & 0xFF);
      }
      string_data[text.size() * 2] = 0;
      string_data[text.size() * 2 + 1] = 0;
      const uint32_t at = string_ptr;
      string_ptr += static_cast<uint32_t>(bytes);
      string_data += bytes;
      remaining -= bytes;
      return at;
    };

    for (size_t i = 0; i < count; ++i, ++current_) {
      const auto& a = items_[current_];
      std::memset(&details[i], 0, sizeof(details[i]));
      details[i].id = a.id;
      details[i].label_ptr = append(a.label);
      details[i].description_ptr = append(a.description);
      details[i].unachieved_ptr = append(a.unachieved);
      details[i].image_id = a.image_id;
      details[i].gamerscore = a.gamerscore;
      details[i].unlock_time_lo = static_cast<uint32_t>(a.unlock_time & 0xFFFFFFFFu);
      details[i].unlock_time_hi = static_cast<uint32_t>(a.unlock_time >> 32);
      details[i].flags = a.flags;
    }

    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }
    return X_ERROR_SUCCESS;
  }

 private:
  uint32_t flags_;
  std::vector<Achievement> items_;
  size_t current_ = 0;
};

}  // namespace

// Raw, because of the 64-bit XUID pair. Registers, after the shift:
//
//     r3 title_id   r4 user_index    r5:r6 xuid   r7 flags
//     r8 count      r9 &buffer_size  r10 &handle
//
// The register dump is logged once per title so the mapping stays checkable
// against a real call rather than resting on the prototype alone.
REX_HOOK_RAW(__imp__XamUserCreateAchievementEnumerator) {
  const uint32_t title_id = ctx.r3.u32;
  const uint32_t flags = ctx.r7.u32;
  uint32_t count = ctx.r8.u32;
  const uint32_t buffer_size_ptr = ctx.r9.u32;
  const uint32_t handle_ptr = ctx.r10.u32;

  static uint32_t s_logged_regs = 0;
  if (s_logged_regs != title_id) {
    s_logged_regs = title_id;
    REXKRNL_INFO("AchievementEnumerator regs: r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} "
                 "r8={:#x} r9={:#x} r10={:#x}",
                 ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                 ctx.r9.u32, ctx.r10.u32);
  }

  if (!handle_ptr || !buffer_size_ptr) {
    ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
    return;
  }
  if (count == 0 || count > kMaxAchievements) {
    count = kMaxAchievements;
  }

  // The detail page asks for this title's achievements by id, which is a
  // reliable statement of what the library is showing -- and what Play Game
  // will launch. See game_launch.h.
  nxe_game::NoteTitleShown(title_id);

  auto items = ParseAchievements(title_id);
  const size_t total = items.size();
  const size_t item_size = sizeof(X_ACHIEVEMENT_DETAILS) + kStringBufferSize;

  auto e = rex::system::make_object<AchievementEnumerator>(REX_KERNEL_STATE(), count, flags,
                                                           std::move(items));
  const auto result = e->Initialize(0xFB, 0xFB, 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) {
    REXKRNL_WARN("XamUserCreateAchievementEnumerator: Initialize failed {:#x}", result);
    ctx.r3.u64 = result;
    return;
  }

  *reinterpret_cast<rex::be<uint32_t>*>(base + buffer_size_ptr) =
      static_cast<uint32_t>(item_size * count);
  *reinterpret_cast<rex::be<uint32_t>*>(base + handle_ptr) = e->handle();

  REXKRNL_INFO("XamUserCreateAchievementEnumerator(title {:#010x}, flags {:#x}, count {}) -> "
               "{} achievement(s), handle {:#x}",
               title_id, flags, count, total, e->handle());
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
