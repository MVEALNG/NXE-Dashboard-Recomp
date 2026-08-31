// XamContentCreateEnumerator -- the non-aggregate content enumerator.
//
// The aggregate enumerator was fixed in content_enum.cpp and the Game Library
// still reported "You don't have any games in your library". The played-title
// record was correct by then and the dashboard was reading it back, so the list
// was being built and then filtered down to nothing.
//
// The filter is at guest 0x922E0AA0. For each played title it searches the
// content list for a record with the same title id and a game content type:
//
//     if ( v9[79] == v17[0] )                   // content title_id == played title_id
//     {
//         v10 = *v9;                            // content_type
//         if ( v10 == 0x0D0000 || v10 == 0x7000 || v10 == 0x4000 || v10 == 0x5000 )
//             accept;
//     }
//
// -- and it never runs at all when that list is empty:
//
//     v7 = v6[2];                               // number of content records
//     if ( v7 ) { ...search... }
//
// So a played title is only shown if matching content is installed. That is
// reasonable behaviour, and the content genuinely is installed; the list handed
// to it was empty because this enumerator, unlike the aggregate one, is still
// the runtime's:
//
//     ListContent(HDD, xuid, XContentType(content_type));
//
// ListContent's title_id parameter is defaulted, so it searches whichever title
// is running -- FFFE07D1, the dashboard. Ninja Gaiden II is staged under its own
// title id (544307D5, read from its default.xex), so it could never be found. The
// same call also formats the content type straight into the path, so the
// wildcard 0xFFFFFFFF becomes a literal search in a directory that cannot exist.
//
// Both are the defects already fixed in the aggregate enumerator, in the entry
// point that was left alone. This applies the same two corrections and nothing
// else: the sizing, the ODD branch, the XCONTENT_DATA record type and the return
// values are the runtime's, unchanged.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "installed_titles.h"

using namespace rex;
using namespace rex::system;
using namespace rex::system::xam;

namespace {

constexpr uint32_t kContentTypeAny = 0xFFFFFFFFu;

u32 ContentCreateEnumerator_entry(u32 user_index, u32 device_id, u32 content_type,
                                  u32 content_flags, u32 items_per_enumerate,
                                  mapped_u32 buffer_size_ptr, mapped_u32 handle_out) {
  (void)user_index;
  (void)content_flags;

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    if (buffer_size_ptr) {
      *buffer_size_ptr = 0;
    }
    return X_E_INVALIDARG;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(XCONTENT_DATA) * items_per_enumerate;
  }

  auto e = make_object<XStaticEnumerator<XCONTENT_DATA>>(REX_KERNEL_STATE(), items_per_enumerate);
  const auto result = e->Initialize(0xFF, 0xFE, 0x20005, 0x20007, 0);
  if (XFAILED(result)) {
    return result;
  }

  if (!device_info || device_info->device_id == DummyDeviceId::HDD) {
    // Every package staged on the drive, across all titles and all types, which
    // is what AllInstalledContent already works out for the aggregate path.
    for (const auto& content_data : nxe_content::AllInstalledContent()) {
      if (content_type != kContentTypeAny &&
          static_cast<uint32_t>(XContentType(content_data.content_type)) != content_type) {
        continue;
      }
      if (auto* item = e->AppendItem()) {
        *item = content_data;
      }
    }
  }

  if (!device_info || device_info->device_id == DummyDeviceId::ODD) {
    // Disc drive content is not modelled, as in the runtime.
  }

  REXKRNL_INFO("ContentCreateEnumerator(type {:#010x}, device {}) -> {} item(s)", content_type,
               device_id, e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}

}  // namespace

REX_EXPORT(__imp__XamContentCreateEnumerator, ContentCreateEnumerator_entry)
