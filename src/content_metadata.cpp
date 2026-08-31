// Package metadata: the name and size shown against an installed item.
//
// With the library populating, the All Games list finally had a row in it -- and
// the row was wrong: an unreadable name, a size of 2147483647 GB, and a warning
// triangle where the icon belongs.
//
// None of that came from the content record, which is correct by this point. It
// came from XamContentGetMetaDataInternal, which ships as a bare REX_EXPORT_STUB
// -- so it returned an undefined r3 and, more to the point, never wrote the
// output buffer at all. The list rendered whatever that memory already held.
// 2147483647 is INT_MAX, which is what a size field reads as when nobody sets it.
//
// The contract comes from guest 0x922C0580, which sets its argument up in full
// before calling:
//
//     memset(v3, 0, 512);
//     v3[1] = 0x40000;                        // content_type   (+0x04)
//     v3[0] = v2[0];                          // device_id      (+0x00)
//     sub_9214C530(v4, "IptvClient", 42);     // file_name      (+0x108)
//     v5 = -129057;                           // title_id 0xFFFE07DF (+0x140)
//     XamContentGetMetaDataInternal(v3, &unk_92814FC0, 0);
//
// Those offsets are XCONTENT_AGGREGATE_DATA exactly, so the first argument is a
// content record, the second is the metadata to fill, and the third is zero. The
// callers test the result as "non-zero is failure":
//
//     if ( XamContentGetMetaDataInternal(...) ) return -2147467259;   // E_FAIL
//
// What gets reported
// ------------------
// Everything here is read off the package on disk; nothing is invented.
//
//   content_size   the real recursive byte count of the package directory
//   display_name   the name from the content record (English slot)
//   title_name     the same name -- for an installed title they are the same
//   content_type   echoed back from the record
//
// Thumbnails are reported as zero-length rather than filled with something
// plausible. There is no artwork in an extracted package, and a zero size is how
// the format says "no image" -- the dashboard then draws its own placeholder
// instead of decoding whatever bytes happened to be there.
//
// The struct is 0x93D6 bytes and is zeroed in full before the fields above are
// written, which is what makes the untouched fields read as empty rather than as
// leftover memory.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/filesystem/devices/stfs_xbox.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "cover_art.h"
#include "game_icon.h"
#include "storage_device.h"

using namespace rex;
using namespace rex::system;
using namespace rex::system::xam;
using rex::filesystem::XContentMetadata;

namespace {

// English occupies the first language slot: the accessors index with
// uint32_t(XLanguage::kEnglish) - 1, and kEnglish is 1.
constexpr size_t kEnglishSlot = 0;

// Total bytes under a directory, following subdirectories. An installed title is
// a directory tree, not a single file, so its size is the sum of its parts.
uint64_t DirectorySize(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return 0;
  }
  if (std::filesystem::is_regular_file(path, ec)) {
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
  }

  uint64_t total = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(
           path, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec)) {
      const auto size = it->file_size(ec);
      if (!ec) total += size;
    }
  }
  return total;
}

// <root>/<xuid:016X>/<title:08X>/<type:08X>/<file_name>, the layout the content
// resolver writes and the enumerator reads back.
std::filesystem::path PackagePath(const XCONTENT_AGGREGATE_DATA& content) {
  char xuid_dir[17] = {};
  std::snprintf(xuid_dir, sizeof(xuid_dir), "%016llX",
                static_cast<unsigned long long>(uint64_t(content.xuid)));
  char title_dir[9] = {};
  std::snprintf(title_dir, sizeof(title_dir), "%08X", uint32_t(content.title_id));
  char type_dir[9] = {};
  std::snprintf(type_dir, sizeof(type_dir), "%08X",
                static_cast<uint32_t>(XContentType(content.content_type)));
  return nxe_storage::ContentRoot() / xuid_dir / title_dir / type_dir / content.file_name();
}

void StoreName(be<uint16_t>* dst, size_t capacity, const std::u16string& name) {
  const size_t count = name.size() < capacity - 1 ? name.size() : capacity - 1;
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<uint16_t>(name[i]);
  }
  dst[count] = 0;
}

u32 GetMetaDataInternal_entry(mapped_void content_ptr, mapped_void metadata_ptr, u32 flags) {
  (void)flags;

  const auto* content = content_ptr.as<const XCONTENT_AGGREGATE_DATA*>();
  auto* metadata = metadata_ptr.as<XContentMetadata*>();
  if (content == nullptr || metadata == nullptr) {
    REXKRNL_WARN("XamContentGetMetaDataInternal: null argument (content={} metadata={})",
                 fmt::ptr(content), fmt::ptr(metadata));
    return X_ERROR_INVALID_PARAMETER;
  }

  const auto path = PackagePath(*content);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // Report the miss rather than filling the record with a guess; the callers
    // treat any non-zero result as failure and skip the item.
    REXKRNL_WARN("XamContentGetMetaDataInternal: no package at {}", path.string());
    return X_ERROR_FILE_NOT_FOUND;
  }

  std::memset(metadata, 0, sizeof(XContentMetadata));
  metadata->content_type = XContentType(content->content_type);
  metadata->metadata_version = 2;
  metadata->content_size = DirectorySize(path);

  const auto name = content->display_name();
  StoreName(metadata->display_name_raw.uint[kEnglishSlot], 128, name);
  StoreName(metadata->title_name_raw.uint, 64, name);

  // Cover art: read, sized, and deliberately not reported.
  //
  // The picture itself is fine -- a fitted 13,118-byte PNG that fits the 15,616
  // byte field, loaded and cached by nxe_art::CoverFor. Reporting a non-zero
  // thumbnail_size is what breaks: the dashboard crashes reading guest address
  // 0x138, a null object dereference, and it does so BEFORE any thumbnail is
  // loaded -- the only memory:// request in the log is the gamer picture. So it
  // is not the image loader refusing the art; something consuming this record
  // takes a different path the moment a thumbnail is declared, and falls over.
  //
  // Twice attempted, twice the same crash, so the size stays zero until that
  // path is understood. Zero is the format's own "no image", and the loader
  // hook substitutes a placeholder so the shell does not retry forever.
  metadata->thumbnail_size = 0;
  metadata->title_thumbnail_size = 0;

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamContentGetMetaDataInternal({}) -> {} bytes", rex::string::to_utf8(name),
                 uint64_t(metadata->content_size));
  }
  return X_ERROR_SUCCESS;
}

}  // namespace

REX_EXPORT(__imp__XamContentGetMetaDataInternal, GetMetaDataInternal_entry)
