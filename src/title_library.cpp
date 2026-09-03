// Game Library: played-title history and cached title names.
//
// Both entry points the library depends on ship as bare REX_EXPORT_STUB, so
// each returned an undefined r3 and the list was built on whatever the register
// happened to hold.
//
// XamUserCreateTitlesPlayedEnumerator
// -----------------------------------
// Contract from the two call sites, guest 0x922F30A0 and 0x921FD370:
//
//     count = clamp(*(a1+56), 1, 256);
//     result = XamUserCreateTitlesPlayedEnumerator(user, x, y, count,
//                                                  &buffer_size, &handle, ovl);
//     if (result) { if (result > 0) return result | 0x80070000; }
//     else { buf = malloc(buffer_size); ... }
//
// so zero is success and the call must report, through buffer_size, how much
// memory the caller should allocate. The other site then reads the records with
//
//     XamEnumerate(handle, 2, buf, 168 * count, &returned, 0);
//
// which fixes the record at 168 bytes.
//
// What it reports
// ---------------
// A real console builds this list from the profile's played-title history. This
// profile has none -- its GPD carries a single Title-namespace entry of 24 zero
// bytes -- so for a long time the honest answer here was an empty list, and that
// is what this returned.
//
// That is no longer the whole picture: a game is now installed on the storage
// device (Ninja Gaiden II, title 544307D5), and the Game Library's Recent Games
// and All Games views are built from THIS enumerator, not from content
// enumeration. Content enumeration works and hands the dashboard the package,
// but nothing in the library reads it, which is why the screen stayed empty even
// after the aggregate enumerator started returning items.
//
// So the list is built from what is genuinely installed. Nothing is invented:
//
//   title_id      the id under which the content is staged
//   title_name    the display name out of the package header
//   last_played   the content directory's real modification time
//   achievements  and gamerscore from the profile's played-games history,
//                 when it has an entry for the title (see played_titles.h)
//
// The history is also a source of titles in its own right: a profile has played
// games whose content is not installed here, and those carry most of its
// achievements. So the list is the union of the two, history first.
//
// A title with no history entry still shows an empty score rather than a
// fabricated one. That is what a real console shows for a game installed but
// not yet launched.
//
// The record layout is fixed at 168 bytes by the caller''''s own sizing (see
// above). The field breakdown is the standard XAM title-played record, and it
// accounts for exactly 168 bytes with nothing left over:
//
//   0x00 title_id            0x14 reserved_achievement_count
//   0x04 achievements_possible 0x16 all_avatar_awards
//   0x08 achievements_earned   0x18 male_avatar_awards
//   0x0C gamerscore_total      0x1A female_avatar_awards
//   0x10 gamerscore_earned     0x1C reserved_flags
//   0x20 last_played (FILETIME)
//   0x28 title_name[64]      -> ends at 0xA8 = 168
//
// XamGetCachedTitleName
// ---------------------
// Guest 0x922F2B80 calls it as
//
//     size = 23;
//     if (XamGetCachedTitleName(title_id, name_buffer, &size) == 997) return E_PENDING;
//
// so only 997 (ERROR_IO_PENDING) is special; anything else lets it continue.
// There is no title-name cache in this runtime, so it reports "not found" -- but
// it terminates the caller's buffer first. The stub left that buffer untouched,
// which is how an unnamed entry ends up displaying whatever was in memory.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "installed_titles.h"
#include "title_names.h"
#include "played_titles.h"
#include "storage_device.h"

using namespace rex;

namespace {

// One played-title record, 168 bytes. See the layout note above.
#pragma pack(push, 1)
struct X_TITLE_PLAYED {
  be<uint32_t> title_id;
  be<uint32_t> achievements_possible;
  be<uint32_t> achievements_earned;
  be<uint32_t> gamerscore_total;
  be<uint32_t> gamerscore_earned;
  be<uint16_t> reserved_achievement_count;
  be<uint16_t> all_avatar_awards;
  be<uint16_t> male_avatar_awards;
  be<uint16_t> female_avatar_awards;
  be<uint32_t> reserved_flags;
  be<uint64_t> last_played;
  be<uint16_t> title_name[64];
};
#pragma pack(pop)
static_assert(sizeof(X_TITLE_PLAYED) == 168, "played-title record must be 168 bytes");

constexpr uint32_t kMaxTitles = 256;

// System titles, not games.
//
// 0xFFFE#### is the range the console reserves for itself. The dashboard is
// 0xFFFE07D1, and its content is the system-update installer and the profile
// packages; the rest of the family is the built-in applications and their saved
// state. One of those is staged here -- 0xFFFE07DF, "Video Bookmark Data" --
// and it was being listed as a game, because the only exclusion was the
// dashboard's own id. Excluding the range covers the whole family rather than
// the one member that happened to show up.
bool IsSystemTitle(uint32_t title_id) {
  return nxe_content::IsSystemTitleId(title_id);
}

// Windows FILETIME for the content directory: 100ns ticks since 1601-01-01.
//
// The package's own file_name() is a bare name ("Ninja Gaiden II"), not a path,
// so the directory has to be rebuilt the same way the content resolver lays it
// out: <root>/<xuid:016X>/<title:08X>/<type:08X>/<name>.
uint64_t FileTimeFor(const rex::system::xam::XCONTENT_AGGREGATE_DATA& content) {
  char xuid_dir[17] = {};
  std::snprintf(xuid_dir, sizeof(xuid_dir), "%016llX",
                static_cast<unsigned long long>(uint64_t(content.xuid)));
  char title_dir[9] = {};
  std::snprintf(title_dir, sizeof(title_dir), "%08X", uint32_t(content.title_id));
  char type_dir[9] = {};
  std::snprintf(type_dir, sizeof(type_dir), "%08X",
                static_cast<uint32_t>(rex::system::XContentType(content.content_type)));

  const auto path =
      nxe_storage::ContentRoot() / xuid_dir / title_dir / type_dir / content.file_name();

  std::error_code ec;
  const auto tp = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return 0;
  }
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(tp);
  const auto unix_secs =
      std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
  if (unix_secs <= 0) {
    return 0;
  }
  // 11644473600 seconds between 1601-01-01 and 1970-01-01.
  return (static_cast<uint64_t>(unix_secs) + 11644473600ull) * 10000000ull;
}

// One record per title the profile has played or has content for, newest first
// so Recent Games reads correctly.
//
// The profile's history comes first and is authoritative: it is where the
// achievement counts, the gamerscore and the real title names live. Installed
// content then fills in anything the history does not mention.
std::vector<X_TITLE_PLAYED> InstalledTitles();

std::vector<X_TITLE_PLAYED> InstalledTitles() {
  std::vector<X_TITLE_PLAYED> titles;

  for (const auto& played : nxe_profile::PlayedTitles()) {
    if (IsSystemTitle(played.title_id) || played.title_id == 0) {
      continue;
    }
    X_TITLE_PLAYED record{};
    record.title_id = played.title_id;
    record.achievements_possible = played.achievements_possible;
    record.achievements_earned = played.achievements_earned;
    record.gamerscore_total = played.gamerscore_total;
    record.gamerscore_earned = played.gamerscore_earned;
    record.last_played = played.last_played;

    const size_t count = std::min<size_t>(played.name.size(), 63);
    for (size_t i = 0; i < count; ++i) {
      record.title_name[i] = static_cast<uint16_t>(played.name[i]);
    }
    record.title_name[count] = 0;
    titles.push_back(record);
  }

  for (const auto& content : nxe_content::AllInstalledContent()) {
    const uint32_t title_id = content.title_id;
    if (IsSystemTitle(title_id) || title_id == 0) {
      continue;
    }
    if (std::any_of(titles.begin(), titles.end(),
                    [&](const X_TITLE_PLAYED& t) { return t.title_id == title_id; })) {
      continue;
    }

    X_TITLE_PLAYED record{};
    record.title_id = title_id;
    record.last_played = FileTimeFor(content);

    const auto name = content.display_name();
    const size_t count = std::min<size_t>(name.size(), 63);
    for (size_t i = 0; i < count; ++i) {
      record.title_name[i] = static_cast<uint16_t>(name[i]);
    }
    record.title_name[count] = 0;

    REXKRNL_INFO("Game library: title {:#010x} '{}' installed, no history entry", title_id,
                 rex::string::to_utf8(name));
    titles.push_back(record);
  }

  uint32_t earned = 0;
  uint32_t score = 0;
  for (const auto& title : titles) {
    earned += title.achievements_earned;
    score += title.gamerscore_earned;
  }
  REXKRNL_INFO("Game library: {} title(s), {} achievement(s), {} gamerscore", titles.size(),
               earned, score);

  std::sort(titles.begin(), titles.end(), [](const X_TITLE_PLAYED& a, const X_TITLE_PLAYED& b) {
    return uint64_t(a.last_played) > uint64_t(b.last_played);
  });
  return titles;
}

// Raw hook, because the marshaller cannot express this call.
//
// The guest passes a 64-bit XUID as the third argument, and it consumes an
// aligned register PAIR (r5 and r6), not one slot. A typed hook cannot say that:
// the runtime's ArgTranslator reads the low 32 bits of one register per
// parameter, so every argument after the XUID arrives one register early.
//
// That is not a guess. With the typed signature, the write of *buffer_size_out
// faulted at guest address 0x1 -- and 1 is the value of the count argument
// (SignalState) at guest 0x921FD370. The count was arriving in the parameter
// meant for the buffer-size pointer, which places the shift at exactly one slot
// and puts the XUID in r5:r6.
//
// So the guest call at 0x921FD370:
//
//     XamUserCreateTitlesPlayedEnumerator(0, v7, 0, SignalState, &v12, &v10, v8);
//
// lands as:
//
//     r3 user_index   r4 title_id   r5:r6 xuid   r7 count
//     r8 &buffer_size r9 &handle    r10 overlapped
// Asking for the played titles is not the Game Library opening.
//
// It looked like the one event unique to that screen, and it is not: the
// dashboard asks three times during boot to fill the Latest Games tile on the
// main blade, before any button has been pressed, all from the same call site
// as the later ones -- so neither the caller nor the arguments separate "the
// library opened" from "the shell is drawing its home screen". Reporting the
// library from here put "Browsing the Game Library" on screen at boot while the
// user was looking at the dashboard. The blades live inside one scene, so there
// is no navigation to hook either; the library needs a signal that has not been
// found yet, and inventing one from this call is worse than saying nothing.
REX_HOOK_RAW(__imp__XamUserCreateTitlesPlayedEnumerator) {

  const uint32_t user_index = ctx.r3.u32;
  uint32_t count = ctx.r7.u32;

  // The guest filters this call: r4 is a title id and r5:r6 a XUID (see the
  // register note above). Both are currently ignored -- every caller gets the
  // whole list -- so record what was actually asked for.
  const uint32_t filter_title = ctx.r4.u32;
  const uint64_t filter_xuid = (uint64_t(ctx.r5.u32) << 32) | ctx.r6.u32;
  // Two call sites ask for this list and they size it differently: 0x922F30A0
  // takes its count from its own object and defaults to 256, while 0x921FD370
  // takes it from the gamercard's played-title count. The link register says
  // which one is asking.
  REXKRNL_INFO("TitlesPlayedEnumerator: user {} title-filter {:#010x} xuid {:016X} count {} "
               "(caller lr={:#010x})",
               user_index, filter_title, filter_xuid, count, uint32_t(ctx.lr));
  const uint32_t buffer_size_ptr = ctx.r8.u32;
  const uint32_t handle_ptr = ctx.r9.u32;

  static bool s_logged_regs = false;
  if (!s_logged_regs) {
    REXKRNL_INFO(
        "TitlesPlayedEnumerator regs: r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} r8={:#x} "
        "r9={:#x} r10={:#x}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
        ctx.r10.u32);
  }

  if (!handle_ptr) {
    ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
    return;
  }
  if (count == 0 || count > kMaxTitles) {
    count = kMaxTitles;
  }

  auto e = rex::system::make_object<rex::system::XStaticEnumerator<X_TITLE_PLAYED>>(
      REX_KERNEL_STATE(), count);
  const auto result = e->Initialize(0xFE, 0xFE, 0x2000A, 0x20009, 0);
  if (XFAILED(result)) {
    REXKRNL_WARN("XamUserCreateTitlesPlayedEnumerator: Initialize failed {:#x}", result);
    ctx.r3.u64 = result;
    return;
  }

  const auto installed = InstalledTitles();
  for (const auto& title : installed) {
    if (auto* item = e->AppendItem()) {
      *item = title;
    }
  }

  if (buffer_size_ptr) {
    *reinterpret_cast<rex::be<uint32_t>*>(base + buffer_size_ptr) =
        static_cast<uint32_t>(sizeof(X_TITLE_PLAYED)) * count;
  }
  *reinterpret_cast<rex::be<uint32_t>*>(base + handle_ptr) = e->handle();

  REXKRNL_INFO("XamUserCreateTitlesPlayedEnumerator(user {}) -> {} title(s), handle {:#x}",
               user_index, installed.size(), e->handle());
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamIsSystemTitleId -- "is this title one of the console's own?"
//
// This ships as REX_EXPORT_STUB, and REX_STUB does not assign r3 at all: it logs
// and returns, leaving whatever the register already held. So the guest read an
// uninitialized value as the answer to a boolean question, and an uninitialized
// register is almost never zero.
//
// That is what emptied the Game Library. The list is built and drawn from the
// played-title enumerator -- which works; the guest reads both records and gets
// X_ERROR_NO_MORE_FILES to close the walk -- and then the library asks this
// predicate about each entry to drop the console's own titles from a list of
// games. Answering "yes" for everything removes every game a moment after it
// appears, which is exactly the reported behaviour: the games show for about a
// second and then vanish.
//
// It only shows up on that path. A boot that never leaves the home blade does
// not call it once; navigating into the library logs it immediately:
//
//     __imp__XamIsSystemTitleId STUB
//
// The real predicate is the same one the library list already uses above, so
// both now answer from one place.
u32 XamIsSystemTitleId_entry(u32 title_id) {
  return IsSystemTitle(title_id) ? 1 : 0;
}

// The name a title is known by, from the same two sources the library list uses.
//
// The profile's own history first -- it carries the name the console recorded --
// then the display name out of the installed package header.
std::u16string LookupTitleNameImpl(uint32_t title_id) {
  if (!title_id || IsSystemTitle(title_id)) {
    return {};
  }
  for (const auto& played : nxe_profile::PlayedTitles()) {
    if (played.title_id == title_id && !played.name.empty()) {
      return played.name;
    }
  }
  for (const auto& content : nxe_content::AllInstalledContent()) {
    if (uint32_t(content.title_id) == title_id) {
      auto name = content.display_name();
      if (!name.empty()) {
        return name;
      }
    }
  }
  return {};
}

// XamGetCachedTitleName -- the console's title-name cache.
//
// This is what names a row in the storage browser, and reporting a miss is why
// they all read "Unknown Game". The caller at guest 0x922F2B80 is:
//
//     v4 = 23;
//     if ( XamGetCachedTitleName(a1[11], a1 + 12, &v4) == 997 ) return E_PENDING;
//     else ... continue and draw ...
//
// so it takes whatever is left in the buffer as the name -- only 997 makes it
// wait. An empty buffer is an empty name, and the shell falls back to "Unknown
// Game". The size is 23 characters, the field at a1+12 being 24 wide.
//
// There is a real cache to answer from: the installed packages carry their
// display name in the content header ("Halo 3", "Ninja Gaiden II") and the
// profile's history carries the name the console recorded. Those are the same
// two sources the library list is built from, so a row and its list entry
// cannot disagree.
u32 XamGetCachedTitleName_entry(u32 title_id, mapped_void name_buffer, mapped_u32 size_chars) {
  auto* dst = name_buffer.as<rex::be<uint16_t>*>();
  if (dst == nullptr || !size_chars) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint32_t capacity = *size_chars;  // in characters, terminator included
  if (!capacity) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // Terminate first, so a miss renders as empty rather than as whatever the
  // buffer already contained.
  dst[0] = 0;

  const auto name = nxe_title::NameFor(title_id);
  if (name.empty()) {
    static uint32_t s_reported = 0;
    if (s_reported != title_id) {
      s_reported = title_id;
      REXKRNL_INFO("XamGetCachedTitleName({:#010x}) -> no cached name", title_id);
    }
    return X_ERROR_NOT_FOUND;
  }

  const size_t count = std::min<size_t>(name.size(), capacity - 1);
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<uint16_t>(name[i]);
  }
  dst[count] = 0;
  *size_chars = static_cast<uint32_t>(count);

  static uint32_t s_named = 0;
  if (s_named != title_id) {
    s_named = title_id;
    REXKRNL_INFO("XamGetCachedTitleName({:#010x}) -> '{}'", title_id,
                 rex::string::to_utf8(name));
  }
  return X_ERROR_SUCCESS;
}

}  // namespace

namespace nxe_title {

std::u16string NameFor(uint32_t title_id) { return LookupTitleNameImpl(title_id); }

}  // namespace nxe_title

REX_EXPORT(__imp__XamIsSystemTitleId, XamIsSystemTitleId_entry)
REX_EXPORT(__imp__XamGetCachedTitleName, XamGetCachedTitleName_entry)

namespace nxe_content {

uint32_t EnumeratedTitleCount() {
  // Not reachable until the kernel is up: InstalledTitles reads the content
  // manager and the signed-in profile. Before that the caller has to make do
  // with a filesystem estimate.
  if (!REX_KERNEL_STATE()) {
    return 0;
  }
  return static_cast<uint32_t>(InstalledTitles().size());
}

}  // namespace nxe_content
