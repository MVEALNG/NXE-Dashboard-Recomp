// Content enumeration, instrumented.
//
// The Game Library comes up empty even with a package staged and a valid header
// beside it. The runtime's own debug line says only
//
//     XamContentAggregateCreateEnumerator: added 0 items to enumerator
//
// which confirms nothing matched but not why -- it never records which content
// type was asked for, which XUID, or which directory it looked in. Those three
// facts are the whole question, so this is the runtime's implementation with the
// logic left alone and the missing detail logged.
//
// The search itself is unchanged from xam_content_aggregate.cpp:
//
//     title_ids = { kCurrentlyRunningTitleId } + the XEX's alternate title ids
//     for each: ListContent(HDD, xuid, type, title)              // user content
//               if (userxuid) ListContent(HDD, 0, type, title)   // common content
//
// kCurrentlyRunningTitleId resolves to the running title, which for the
// dashboard is FFFE07D1 -- so the common-content pass looks in
// Content/0000000000000000/FFFE07D1/<type>/, exactly where the imported game
// sits. If a type other than 00004000 is being requested, that is what the log
// below will show.

#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include <filesystem>
#include <string>

#include "installed_titles.h"
#include "played_titles.h"
#include "storage_device.h"

using namespace rex;
using namespace rex::system;
using namespace rex::system::xam;

// Show only games staged from the ROMs folder in the Game Library.
//
// Without this the library is every content package ever staged, which on a
// machine that has been experimented with means games nobody put there. The
// ROMs folder is a folder somebody manages: adding a game adds it, deleting one
// removes it, and the shelf matches what is on disk.
//
// Only game packages are filtered. Themes, gamerpics and saves are content too,
// and hiding those would break parts of the dashboard that have nothing to do
// with which games are installed.
REXCVAR_DEFINE_BOOL(library_roms_only, true, "Dashboard",
                    "Show only games staged from roms_dir in the Game Library. Off lists "
                    "every game package staged under the storage root.");

// List the account's games in All Games even with no package staged for them.
//
// Off, because the guest does not survive it. The entries reach the list --
// the enumeration goes from 881 items to 927 -- and the library then opens the
// package behind each one to read its header. There is no package, the open
// returns nothing, and the guest dereferences it without checking: a read of
// guest address 0 inside sub_92147C78, straight after the enumeration.
//
// Listing a title this way therefore needs a package actually staged for it,
// not merely an entry describing one. Kept as a setting because the entries
// themselves are right, and become useful the moment there is something behind
// them to open.
REXCVAR_DEFINE_BOOL(library_synthesize_content, false, "Dashboard",
                    "Show games from the profile history in the Game Library even when no "
                    "package is staged for them. They have no package behind them, so "
                    "anything that opens one will fail.");

// Content type 0x4000 -- the directory name both installed games use here:
//
//     Content/0000000000000000/4D5307E6/00004000/Halo 3
//
// so a synthesised entry claims the same type as a real one, and the guest's
// grouping treats it identically.
constexpr uint32_t kInstalledGameType = 0x4000;
constexpr uint32_t kHardDriveDeviceId = 1;
namespace {

// The dashboard asks for content_type 0xFFFFFFFF, meaning "any type". The
// runtime does not implement that: ResolvePackageRoot formats the type straight
// into the path --
//
//     root / {xuid:016X} / {title:08X} / {type:08X}
//
// so a wildcard becomes a literal search in .../FFFE07D1/FFFFFFFF/, a directory
// that cannot exist. That is why the Game Library stayed empty no matter what
// was staged, and why the runtime reported "added 0 items" without complaint.
//
// Expand it here instead: list the title's directory and enumerate each child
// that names a real content type. Directory names are the same {:08X} the
// resolver writes, so a valid one is exactly eight hex digits -- which also
// excludes the sibling "Headers" tree that holds the .header files.
constexpr uint32_t kContentTypeAny = 0xFFFFFFFFu;

// The dashboard's content record is 512 bytes, not 328.
//
// XCONTENT_AGGREGATE_DATA is 328 and covers every field this port fills --
// device id, content type, display name, file name, xuid, title id. The
// dashboard's record is larger, and the extra 184 bytes are not padding it
// ignores: the theme picker's filter at guest 0x922E88C0 reads one of them.
//
//     if ( *(_DWORD *)(v6 + 4) == 196608 )         // 0x30000, kTheme -- passed
//     {
//         v7 = *(_WORD *)(v6 + 506);
//         if ( (v7 & 1) == 0 && (v7 & 2) == 0 )    // rejected
//
// Writing only 328 bytes left +506 holding whatever the guest's buffer already
// had, which read 0xBEBE every time -- so all four installed themes were
// discarded while their type, name and owning profile were all correct.
//
// 512 is not a guess; three independent places in the image agree:
//
//     0x922E88C0   sub_92144098(512); sub_9214BC00(v9, v6, 512)   per record
//     0x922C0448   enumerates one record into a 512-byte buffer
//     0x92201BE8   (v8 << 9) + *(_DWORD *)a1                      512 stride
//
// Fixing it at the enumerator rather than by clearing the caller's buffer in
// XamEnumerate is deliberate: that was tried first and cannot work, because the
// length the guest passes there is not a byte count on this path -- see the
// note in storage_device.cpp.
//
// The tail is zero-filled rather than invented. Zero is what a record with
// nothing set looks like, and it is the truth here: nothing in this port hides
// or marks content.
#pragma pack(push, 1)
struct XCONTENT_AGGREGATE_RECORD {
  XCONTENT_AGGREGATE_DATA data;
  uint8_t reserved[512 - sizeof(XCONTENT_AGGREGATE_DATA)];
};
#pragma pack(pop)
static_assert(sizeof(XCONTENT_AGGREGATE_RECORD) == 512,
              "the dashboard indexes content records with a 512-byte stride");

bool IsContentTypeDirName(const std::string& name) {
  if (name.size() != 8) return false;
  for (char c : name) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

// Every title that actually has content staged for this XUID.
//
// The runtime only ever searches the running title:
//
//     std::vector<uint32_t> title_ids{kCurrentlyRunningTitleId};
//
// which resolves to whatever is executing -- FFFE07D1 for the dashboard. That is
// right for a game asking about its own saves, and wrong for the dashboard,
// whose whole job here is to list content belonging to *other* titles. Installed
// games live under their own title id (Ninja Gaiden II is 544307D5, read from
// the title id field of its own default.xex), so no game could ever appear.
//
// Discover them from the filesystem instead, using the same {:08X} directory
// names the resolver writes.
std::vector<uint32_t> TitlesWithContent(uint64_t xuid) {
  std::vector<uint32_t> titles;
  char xuid_dir[17] = {};
  std::snprintf(xuid_dir, sizeof(xuid_dir), "%016llX", static_cast<unsigned long long>(xuid));

  std::error_code ec;
  const auto root = nxe_storage::ContentRoot() / xuid_dir;
  if (!std::filesystem::exists(root, ec)) {
    return titles;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (!entry.is_directory(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (!IsContentTypeDirName(name)) continue;  // title ids share the 8-hex form
    titles.push_back(static_cast<uint32_t>(std::stoul(name, nullptr, 16)));
  }
  return titles;
}

std::vector<XCONTENT_AGGREGATE_DATA> ListContentAnyType(uint64_t xuid, uint32_t title_id,
                                                        uint32_t resolved_title) {
  std::vector<XCONTENT_AGGREGATE_DATA> all;
  char xuid_dir[17] = {};
  std::snprintf(xuid_dir, sizeof(xuid_dir), "%016llX", static_cast<unsigned long long>(xuid));
  char title_dir[9] = {};
  std::snprintf(title_dir, sizeof(title_dir), "%08X", resolved_title);

  std::error_code ec;
  const auto title_path = nxe_storage::ContentRoot() / xuid_dir / title_dir;
  if (!std::filesystem::exists(title_path, ec)) {
    return all;
  }

  for (const auto& entry : std::filesystem::directory_iterator(title_path, ec)) {
    if (!entry.is_directory(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (!IsContentTypeDirName(name)) continue;  // skips "Headers"

    const auto type = static_cast<uint32_t>(std::stoul(name, nullptr, 16));
    auto found = REX_KERNEL_STATE()->content_manager()->ListContent(
        static_cast<uint32_t>(DummyDeviceId::HDD), xuid, XContentType(type), resolved_title);
    if (!found.empty()) {
      REXKRNL_INFO("ContentEnum:   type {} under title {:#010x} -> {} item(s)", name,
                   resolved_title, found.size());
      all.insert(all.end(), found.begin(), found.end());
    }
  }
  return all;
}

// Is this a game package that did not come from the ROMs folder?
//
// rom.txt is the marker rom_library.cpp leaves in everything it stages, so its
// absence in a game package means the game was put there some other way.
bool SkipAsNotARom(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  if (!REXCVAR_GET(library_roms_only)) {
    return false;
  }
  if (static_cast<uint32_t>(XContentType(data.content_type)) != kInstalledGameType) {
    return false;  // themes, saves, gamerpics: none of this applies
  }

  char xuid_dir[17] = {};
  std::snprintf(xuid_dir, sizeof(xuid_dir), "%016llX",
                static_cast<unsigned long long>(xuid));
  char title_dir[9] = {};
  std::snprintf(title_dir, sizeof(title_dir), "%08X", uint32_t(data.title_id));

  std::error_code ec;
  const auto package = nxe_storage::ContentRoot() / xuid_dir / title_dir / "00004000" /
                       data.file_name();
  // Either marker means the dashboard put it there: rom.txt for an Xbox 360
  // game from the ROMs folder, pcgame.txt for an installed PC game.
  return !std::filesystem::exists(package / "rom.txt", ec) &&
         !std::filesystem::exists(package / "pcgame.txt", ec);
}

u32 ContentAggregateCreateEnumerator_entry(u64 xuid, u32 device_id, u32 content_type, u32 unk3,
                                           mapped_u32 handle_out) {
  (void)unk3;

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    return X_E_INVALIDARG;
  }

  auto e = make_object<XStaticEnumerator<XCONTENT_AGGREGATE_RECORD>>(REX_KERNEL_STATE(), 1);
  X_KENUMERATOR_CONTENT_AGGREGATE* extra = nullptr;
  const auto result = e->Initialize(0xFF, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) {
    return result;
  }
  extra->magic = kXObjSignature;
  extra->handle = e->handle();

  const auto content_type_enum = XContentType(uint32_t(content_type));
  const uint64_t userxuid = REX_KERNEL_STATE()->user_profile()->xuid();

  if (!device_info || device_info->device_type == DeviceType::HDD) {
    std::vector<uint32_t> title_ids{kCurrentlyRunningTitleId};
    auto exe_module = REX_KERNEL_STATE()->GetExecutableModule();
    if (exe_module && exe_module->xex_module()) {
      const auto& alt_ids = exe_module->xex_module()->opt_alternate_title_ids();
      std::copy(alt_ids.cbegin(), alt_ids.cend(), std::back_inserter(title_ids));
    }

    // Add every title that actually has content staged, for both the signed-in
    // user and the common (xuid 0) tree.
    //
    // Dedup against the *resolved* running title, not the sentinel: title_ids
    // starts with kCurrentlyRunningTitleId (0xFFFFFFFF), so comparing discovered
    // ids against the raw list would never match FFFE07D1 and would enumerate
    // the dashboard's own content twice.
    if (content_type == kContentTypeAny) {
      const uint32_t running = REX_KERNEL_STATE()->title_id();
      for (uint64_t who : {xuid, userxuid}) {
        for (uint32_t t : TitlesWithContent(who)) {
          if (t == running) continue;
          if (std::find(title_ids.begin(), title_ids.end(), t) == title_ids.end()) {
            title_ids.push_back(t);
          }
        }
      }
    }

    for (auto& title_id : title_ids) {
      const auto resolved = title_id == kCurrentlyRunningTitleId
                                ? REX_KERNEL_STATE()->title_id()
                                : title_id;

      auto content_datas =
          content_type == kContentTypeAny
              ? ListContentAnyType(xuid, title_id, resolved)
              : REX_KERNEL_STATE()->content_manager()->ListContent(
                    static_cast<uint32_t>(DummyDeviceId::HDD), xuid, content_type_enum, title_id);
      REXKRNL_INFO("ContentEnum: type {:#010x} title {:#010x} xuid {:#018x} -> {} item(s)",
                   content_type, resolved, xuid, content_datas.size());
      for (const auto& content_data : content_datas) {
        if (SkipAsNotARom(xuid, content_data)) {
          continue;
        }
        if (auto* item = e->AppendItem()) {
          std::memset(item, 0, sizeof(*item));
          item->data = content_data;
          REXKRNL_INFO("ContentEnum:   -> device {} type {:#010x} title {:#010x} '{}' file '{}'",
                       uint32_t(item->data.device_id),
                       static_cast<uint32_t>(XContentType(item->data.content_type)),
                       uint32_t(item->data.title_id),
                       rex::string::to_utf8(item->data.display_name()), item->data.file_name());
        }
      }

      if (userxuid != 0 && xuid != 0) {
        auto common_datas =
            content_type == kContentTypeAny
                ? ListContentAnyType(0, title_id, resolved)
                : REX_KERNEL_STATE()->content_manager()->ListContent(
                      static_cast<uint32_t>(DummyDeviceId::HDD), 0, content_type_enum, title_id);
        REXKRNL_INFO("ContentEnum: type {:#010x} title {:#010x} common       -> {} item(s)",
                     content_type, resolved, common_datas.size());
        for (const auto& content_data : common_datas) {
          if (SkipAsNotARom(0, content_data)) {
            continue;
          }
          if (auto* item = e->AppendItem()) {
            std::memset(item, 0, sizeof(*item));
            item->data = content_data;
          }
        }
      }
    }

  // The account's own games, which have no package on this console.
  //
  // All Games is built from this enumeration grouped by title id, so a title
  // with no package never reaches it however much the played-titles list knows
  // about it -- which is why the library showed two games while the profile knew
  // forty-seven.
  //
  // Only for an unfiltered enumeration of the hard drive. A caller asking for one
  // content type, or for a specific device, is asking about packages, and these
  // are not packages: nothing is written to the storage device and there is no
  // file behind them. Anything that opens one will fail, which is the trade for
  // having the list be right. library_synthesize_content = false turns it off.
  if (REXCVAR_GET(library_synthesize_content) && content_type == kContentTypeAny &&
      (!device_info || device_info->device_type == DeviceType::HDD)) {
    size_t added = 0;
    for (const auto& played : nxe_profile::PlayedTitles()) {
      if (nxe_content::IsSystemTitleId(played.title_id) || played.name.empty()) {
        continue;
      }
      if (std::find(title_ids.begin(), title_ids.end(), played.title_id) !=
          title_ids.end()) {
        continue;  // it has real content; that entry is the better one
      }
      auto* item = e->AppendItem();
      if (!item) {
        break;  // the enumerator is full
      }
      std::memset(item, 0, sizeof(*item));
      item->data.device_id = kHardDriveDeviceId;
      item->data.content_type = static_cast<XContentType>(kInstalledGameType);
      item->data.title_id = played.title_id;
      item->data.xuid = 0;
      item->data.set_display_name(played.name);
      // file_name locates a package on disk and there is no package. The title
      // id is unique, and makes a failed lookup name the title it was for.
      char file[16] = {};
      std::snprintf(file, sizeof(file), "%08X", played.title_id);
      item->data.set_file_name(file);
      ++added;
    }
    if (added) {
      REXKRNL_INFO("ContentEnum: {} title(s) from the profile history, no package staged",
                   added);
    }
  }
  }

  *handle_out = e->handle();
  REXKRNL_INFO("ContentAggregateCreateEnumerator(type {:#010x}, device {}) -> {} item(s)",
               content_type, device_id, e->item_count());
  return X_ERROR_SUCCESS;
}

}  // namespace



namespace nxe_content {

std::vector<XCONTENT_AGGREGATE_DATA> AllInstalledContent() {
  std::vector<XCONTENT_AGGREGATE_DATA> all;
  const uint32_t running = REX_KERNEL_STATE()->title_id();
  const uint64_t userxuid = REX_KERNEL_STATE()->user_profile()->xuid();

  std::vector<uint32_t> titles;
  for (uint64_t who : {uint64_t(0), userxuid}) {
    for (uint32_t t : TitlesWithContent(who)) {
      if (std::find(titles.begin(), titles.end(), t) == titles.end()) {
        titles.push_back(t);
      }
    }
  }
  for (uint32_t t : titles) {
    for (uint64_t who : {uint64_t(0), userxuid}) {
      auto found = ListContentAnyType(who, t, t);
      all.insert(all.end(), found.begin(), found.end());
      if (userxuid == 0) break;  // both passes would scan the same directory
    }
  }
  (void)running;
  return all;
}


uint32_t InstalledTitleCount() {
  // Ask the list, when there is one.
  //
  // Two implementations of "how many titles" drifted apart three times today:
  // 48-against-49, then 71-against-70, then 26-against-54. Each time the fix
  // was to make this walk match the other one more closely, and each time a
  // later change moved them apart again. The only number that cannot disagree
  // with the list is the list's own size.
  //
  // The walk below survives as the answer for the one moment it cannot be
  // asked -- during profile load, before the kernel exists -- and is corrected
  // by RepublishDerivedSettings once it can.
  if (const uint32_t exact = EnumeratedTitleCount()) {
    return exact;
  }

  // The same disk, seen the same way the enumerator sees it.
  //
  // This used to walk every XUID directory with its own copy of the rule, while
  // AllInstalledContent -- what the enumerator is built from -- looks only at
  // XUID 0 and the signed-in user. That agreed for as long as the other
  // directories held nothing but the dashboard's own content, and stopped
  // agreeing the moment there were more games: 71 counted against 70
  // enumerated.
  //
  // One off is not a cosmetic difference. The dashboard sizes the library from
  // this number and fills it from the enumerator, and a walk that comes up
  // short makes it discard the whole list -- see the note in installed_titles.h.
  // So the count asks TitlesWithContent, exactly as the enumerator does.
  //
  // The signed-in XUID comes from the cvar rather than the kernel because this
  // runs while the profile is still loading, long before there is a kernel to
  // ask.
  std::vector<uint32_t> seen;
  uint64_t user = 0;
  {
    const std::string text = rex::cvar::GetFlagByName("profile_xuid");
    if (text.size() == 16) {
      user = std::strtoull(text.c_str(), nullptr, 16);
    }
  }
  for (uint64_t who : {uint64_t(0), user}) {
    for (uint32_t title_id : TitlesWithContent(who)) {
      if (IsSystemTitleId(title_id)) continue;
      if (std::find(seen.begin(), seen.end(), title_id) != seen.end()) continue;
      seen.push_back(title_id);
    }
    if (!user) break;  // both passes would scan the same directory
  }

  // And the profile's history, which is the half the enumerator puts first.
  //
  // InstalledTitles() is history-then-content, so a title played on this
  // account but never installed here is in the list and has to be in the count.
  // Leaving it out is how this went from 71-against-70 to 26-against-54.
  for (const auto& played : nxe_profile::PlayedTitles()) {
    if (IsSystemTitleId(played.title_id)) continue;
    if (std::find(seen.begin(), seen.end(), played.title_id) != seen.end()) continue;
    seen.push_back(played.title_id);
  }
  return static_cast<uint32_t>(seen.size());
}

}  // namespace nxe_content

//=============================================================================
// How big a piece of content is
//=============================================================================
//
// Every size on the Hard Drive screen read 0 KB -- the categories and the rows
// inside them alike -- while the free space above them was right. Free space
// comes from XamContentGetDeviceData, which this port implements; the per-item
// sizes come from XamContentGetAttributesInternal, which shipped as a bare
// REX_EXPORT_STUB. REX_STUB does not assign r3, so the guest read an
// uninitialized register as the result, and the reader at guest 0x92272C98
// zeroes both size fields on any non-zero return:
//
//     if ( XamContentGetAttributesInternal(a1 + 40, &attrs, 0) )
//     { *(a1 + 368) = 0; *(a1 + 376) = 0; }        // failed -> no size
//     else
//     { *(a1 + 368) = attrs+0x1C; *(a1 + 376) = attrs+0x04; }
//
// The layout is read straight off that function rather than guessed:
//
//     addi r4, r1, 0x50            ; the out buffer starts here
//     ...
//     lwz  r11, 0x70(r1)           ; buffer +0x20  low half
//     lwz  r10, 0x6C(r1)           ; buffer +0x1C  high half
//     insrdi r11, r10, 32,0        ; -> one 64-bit value at +0x1C
//     std  r11, 0x170(r31)
//     ld   r9,  0x54(r1)           ; buffer +0x04, 64-bit
//     std  r9,  0x178(r31)
//
// so two 64-bit sizes, at +0x04 and +0x1C. Both are filled with the real number
// of bytes the content occupies: these are the size and the size-on-disk of the
// same thing, and for content staged as a directory tree they are the same
// quantity. Nothing else in the block is invented -- it is zeroed.
//
// It matters beyond the size column. The category builder at 0x9227A748 gives
// up entirely when this call fails:
//
//     if ( ... || XamContentGetAttributesInternal(obj[90], obj[90] + 38364, 0) || ... )
//         return -2147467259;      // E_FAIL, before the per-type handler runs
//
// -- and 38364 is 0x95DC, which is exactly the r4 = r3 + 0x95DC seen in the
// register trace of the stub.
namespace {

#pragma pack(push, 1)
struct X_CONTENT_ATTRIBUTES {
  be<uint32_t> unknown0;             // +0x00
  be<uint64_t> size_bytes;           // +0x04  -> object +0x178
  uint8_t reserved[0x1C - 0x0C];     // +0x0C
  be<uint64_t> size_on_disk;         // +0x1C  -> object +0x170
  uint8_t tail[0x30 - 0x24];         // +0x24, out to the 48 bytes the
};                                   //        smallest caller provides
#pragma pack(pop)
static_assert(sizeof(X_CONTENT_ATTRIBUTES) == 0x30,
              "the attributes block is the 48 bytes guest 0x922C0448 allocates");

// Bytes under a staged content directory.
//
// Symlinks are followed on purpose: a game can be staged as a junction back to
// where it actually lives rather than copied onto the drive, and its size is
// still its size.
uint64_t BytesUnder(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec)) {
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
  }
  if (!std::filesystem::is_directory(path, ec)) {
    return 0;
  }

  uint64_t total = 0;
  const auto options = std::filesystem::directory_options::follow_directory_symlink |
                       std::filesystem::directory_options::skip_permission_denied;
  for (auto it = std::filesystem::recursive_directory_iterator(path, options, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    std::error_code file_ec;
    if (it->is_regular_file(file_ec)) {
      const auto size = it->file_size(file_ec);
      if (!file_ec) {
        total += size;
      }
    }
  }
  return total;
}

// Walking a five-gigabyte install is not something to repeat thirty times while
// one screen draws, and staged content does not change underneath a running
// dashboard.
uint64_t CachedBytesUnder(const std::filesystem::path& path) {
  static std::mutex lock;
  static std::map<std::string, uint64_t> cache;

  const auto key = path.string();
  std::lock_guard<std::mutex> guard(lock);
  const auto found = cache.find(key);
  if (found != cache.end()) {
    return found->second;
  }
  const uint64_t size = BytesUnder(path);
  cache.emplace(key, size);
  REXKRNL_INFO("Content size: {} -> {} byte(s)", key, size);
  return size;
}

u32 ContentGetAttributesInternal_entry(mapped_void content, mapped_void attributes, u32 flags) {
  auto* record = content.as<XCONTENT_AGGREGATE_DATA*>();
  auto* out = attributes.as<X_CONTENT_ATTRIBUTES*>();
  if (!record || !out) {
    return X_ERROR_INVALID_PARAMETER;
  }
  (void)flags;

  std::memset(out, 0, sizeof(*out));

  // Same layout the content resolver writes:
  //   <root>/<xuid:016X>/<title:08X>/<type:08X>/<file name>
  char xuid_dir[17] = {};
  std::snprintf(xuid_dir, sizeof(xuid_dir), "%016llX",
                static_cast<unsigned long long>(uint64_t(record->xuid)));
  char title_dir[9] = {};
  std::snprintf(title_dir, sizeof(title_dir), "%08X", uint32_t(record->title_id));
  char type_dir[9] = {};
  std::snprintf(type_dir, sizeof(type_dir), "%08X",
                static_cast<uint32_t>(XContentType(record->content_type)));

  const auto path =
      nxe_storage::ContentRoot() / xuid_dir / title_dir / type_dir / record->file_name();

  const uint64_t size = CachedBytesUnder(path);
  out->size_bytes = size;
  out->size_on_disk = size;

  // Bounded per-call trace.
  //
  // This is the call the launch gate turns on: guest 0x922C8768 walks the device
  // enumerator, skips device_type 1, and needs this to succeed on one of the
  // rest before 0x922C8858 will report the title as playable at all. Seeing the
  // record it is asked about -- and whether the path exists -- is the difference
  // between "the gate never asked" and "the gate asked and we said no".
  static uint32_t s_traced = 0;
  if (s_traced < 24) {
    ++s_traced;
    std::error_code exists_ec;
    REXKRNL_INFO("ContentAttributes: device {} title {:#010x} type {:#x} name '{}' -> {} ({} bytes)",
                 uint32_t(record->device_id), uint32_t(record->title_id),
                 static_cast<uint32_t>(XContentType(record->content_type)), record->file_name(),
                 std::filesystem::exists(path, exists_ec) ? "found" : "MISSING", size);
  }
  return X_ERROR_SUCCESS;
}

}  // namespace

REX_EXPORT(__imp__XamContentAggregateCreateEnumerator, ContentAggregateCreateEnumerator_entry)

REX_EXPORT(__imp__XamContentGetAttributesInternal, ContentGetAttributesInternal_entry)
