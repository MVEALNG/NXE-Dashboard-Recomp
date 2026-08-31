// Storage devices for the NXE dashboard.
//
// The "Storage Devices" screen reported "No storage devices found."
//
// The root cause was not the enumerator. The SDK ships a working one
// (xam_content_device.cpp) handing out a dummy HDD (id 1, type 1) and ODD
// (id 2, type 4), which is exactly the shape the dashboard's HDD probe at guest
// 0x922C0000 expects -- it loops XamEnumerate until it sees device_type == 1.
//
// The dashboard simply never called it. It does not poll for storage; it is
// told about it, and nothing in the runtime ever raised the event. See
// "Announcing the drive" below -- that is the actual fix. Everything else here
// exists because the paths behind that event had never run before and were
// broken in their own right.
//
// The device ids below MUST keep matching the SDK's, since the dashboard takes
// an id from XamEnumerate and passes it back into the functions in this file.
//
// Three of the device entry points the dashboard imports ship as bare
// REX_EXPORT_STUB, which logs a warning and never assigns r3, so the guest
// reads an undefined result and branches on garbage:
//
//   XamContentGetDefaultDevice      guest 0x92272EB8 treats non-zero as failure
//                                   and takes an error path; on "success" it
//                                   stores the *uninitialised* out-param into a
//                                   lwarx/stwcx. slot.
//   XamContentGetDeviceVolumePath   guest 0x922B8930 opens the returned path
//                                   and checks the volume for "FATW".
//   XamContentGetDeviceSerialNumber guest 0x922C1A68 copies 20 bytes into its
//                                   device record.
//
// Signatures were read off those three call sites rather than guessed.
//
// GetDeviceData and the enumerator are also taken over -- not because the SDK's
// are broken, but so the drive reports the real geometry of the host volume
// backing it rather than a hardcoded "Dummy HDD" of 20GB/3GB, and so the
// enumerated records and direct queries can never disagree about it.
//
// XamContentGetDeviceState is left to the SDK: it is already correct, and
// replacing it would only add log lines.

#include <cstring>
#include <functional>
#include <system_error>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "install_paths.h"
#include "storage_device.h"

// The X_ERROR_* macros expand through X_RESULT_FROM_WIN32 to a cast to
// rex::X_RESULT, so they only compile with rex in scope.
using namespace rex;

// The console's storage tree.
//
// Was "A:/Xbox360Storage", which is one machine's second drive. Relative now, so
// a checkout finds it beside the executable; set storage_root to an absolute
// path to keep it anywhere else.
REXCVAR_DEFINE_STRING(storage_root, "storage", "Paths",
                      "Xbox 360 storage tree holding Content/ and xconfig.bin.");

namespace nxe_storage {

const std::filesystem::path& Root() {
  // The console's storage tree: Content/, xconfig.bin and the rest.
  static const std::filesystem::path kRoot = nxe_paths::Resolve(REXCVAR_GET(storage_root));
  return kRoot;
}

const std::filesystem::path& ContentRoot() {
  static const std::filesystem::path kContent = Root() / "Content";
  return kContent;
}

}  // namespace nxe_storage

namespace {

// Layout of the 0x50-byte record XamContentGetDeviceData fills and that
// XamEnumerate streams one-at-a-time out of the device enumerator.
struct X_CONTENT_DEVICE_DATA {
  rex::be<uint32_t> device_id;
  rex::be<uint32_t> device_type;
  rex::be<uint64_t> total_bytes;
  rex::be<uint64_t> free_bytes;
  rex::be<uint16_t> name[28];
};
static_assert(sizeof(X_CONTENT_DEVICE_DATA) == 0x50,
              "X_CONTENT_DEVICE_DATA must match the guest's 0x50-byte record");

// Must stay in step with the SDK's DummyDeviceId / DeviceType.
constexpr uint32_t kDeviceIdHdd = 1;
constexpr uint32_t kDeviceIdOdd = 2;
constexpr uint32_t kDeviceTypeHdd = 1;
constexpr uint32_t kDeviceTypeOdd = 4;

constexpr uint64_t kGb = 1024ull * 1024ull * 1024ull;

// The largest hard drive Microsoft shipped for the 360. The host volume is far
// larger than any real 360 drive, so the emulated drive is presented at a size
// the dashboard could actually have seen, with the free figure derived from
// what is genuinely on disk (and clamped by the host's real free space).
constexpr uint64_t kHddCapacity = 320ull * kGb;

// A read-only disc: no free space.
constexpr uint64_t kOddCapacity = 7ull * kGb;

struct DeviceInfo {
  uint32_t id;
  uint32_t type;
  const char* name;
  const char* volume_path;
};

constexpr DeviceInfo kDevices[] = {
    {kDeviceIdHdd, kDeviceTypeHdd, "Hard Drive", nxe_storage::kHddMountPath},
    {kDeviceIdOdd, kDeviceTypeOdd, "Disc Drive", "\\Device\\Cdrom0"},
};

const DeviceInfo* Lookup(uint32_t device_id) {
  for (const auto& d : kDevices) {
    if (d.id == device_id) return &d;
  }
  return nullptr;
}

// Bytes occupied by the storage tree, computed once. Used to derive a free
// figure that reflects what is actually staged rather than a round number.
uint64_t UsedBytes() {
  static const uint64_t kUsed = [] {
    uint64_t total = 0;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        nxe_storage::Root(), std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
      return total;
    }
    for (const auto& entry : it) {
      std::error_code file_ec;
      if (entry.is_regular_file(file_ec) && !file_ec) {
        const auto size = entry.file_size(file_ec);
        if (!file_ec) {
          total += size;
        }
      }
    }
    return total;
  }();
  return kUsed;
}

void DeviceCapacity(const DeviceInfo& device, uint64_t& total, uint64_t& free_bytes) {
  if (device.type != kDeviceTypeHdd) {
    total = kOddCapacity;
    free_bytes = 0;
    return;
  }

  total = kHddCapacity;
  const uint64_t used = UsedBytes();
  free_bytes = used >= total ? 0 : total - used;

  // Never claim more free space than the host can actually provide.
  std::error_code ec;
  const auto space = std::filesystem::space(nxe_storage::Root(), ec);
  if (!ec && static_cast<uint64_t>(space.available) < free_bytes) {
    free_bytes = static_cast<uint64_t>(space.available);
  }
}

// The guest's name field is big-endian UTF-16, NUL-terminated.
void WriteName(rex::be<uint16_t>* dst, size_t capacity, const char* ascii) {
  size_t i = 0;
  for (; ascii[i] != 0 && i + 1 < capacity; ++i) {
    dst[i] = static_cast<uint16_t>(static_cast<unsigned char>(ascii[i]));
  }
  dst[i] = 0;
}

// Writes a NUL-terminated ANSI string, reporting whether it fit.
bool WriteAnsi(mapped_void buffer, uint32_t capacity, const char* text) {
  char* dst = buffer.as<char*>();
  if (dst == nullptr || capacity == 0) {
    return false;
  }
  const size_t len = std::strlen(text);
  if (len + 1 > capacity) {
    return false;
  }
  std::memcpy(dst, text, len + 1);
  return true;
}

// Fills one 0x50-byte record from a device description. Shared by the direct
// query and the enumerator so the two can never disagree about the drive.
void FillDeviceData(X_CONTENT_DEVICE_DATA* data, const DeviceInfo& device) {
  uint64_t total = 0;
  uint64_t free_bytes = 0;
  DeviceCapacity(device, total, free_bytes);

  std::memset(data, 0, sizeof(*data));
  data->device_id = device.id;
  data->device_type = device.type;
  data->total_bytes = total;
  data->free_bytes = free_bytes;
  WriteName(data->name, 28, device.name);
}

//=============================================================================
// Entry points
//=============================================================================

u32 GetDeviceData_entry(u32 device_id, mapped_void device_data) {
  const DeviceInfo* device = Lookup(device_id);
  if (device == nullptr) {
    REXKRNL_WARN("XamContentGetDeviceData({:#x}) - no such device", device_id);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  auto* data = device_data.as<X_CONTENT_DEVICE_DATA*>();
  if (data == nullptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  FillDeviceData(data, *device);

  REXKRNL_INFO("XamContentGetDeviceData({:#x}) -> '{}' type={} total={}MB free={}MB", device_id,
               device->name, device->type, uint64_t(data->total_bytes) / (1024 * 1024),
               uint64_t(data->free_bytes) / (1024 * 1024));
  return X_ERROR_SUCCESS;
}

u32 GetDefaultDevice_entry(mapped_u32 device_id_out) {
  if (!device_id_out) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // The hard drive is the console's default storage target.
  *device_id_out = kDeviceIdHdd;
  REXKRNL_INFO("XamContentGetDefaultDevice() -> {:#x}", kDeviceIdHdd);
  return X_ERROR_SUCCESS;
}

u32 GetDeviceVolumePath_entry(u32 device_id, mapped_void path_buffer,
                                   u32 path_capacity, u32 overlapped_ptr) {
  const DeviceInfo* device = Lookup(device_id);
  if (device == nullptr) {
    REXKRNL_WARN("XamContentGetDeviceVolumePath({:#x}) - no such device", device_id);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!WriteAnsi(path_buffer, path_capacity, device->volume_path)) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  REXKRNL_INFO("XamContentGetDeviceVolumePath({:#x}) -> '{}'", device_id, device->volume_path);
  (void)overlapped_ptr;
  return X_ERROR_SUCCESS;
}

u32 GetDeviceSerialNumber_entry(u32 device_id, mapped_void serial_buffer,
                                     u32 serial_capacity) {
  const DeviceInfo* device = Lookup(device_id);
  if (device == nullptr) {
    REXKRNL_WARN("XamContentGetDeviceSerialNumber({:#x}) - no such device", device_id);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // Console storage serials are fixed-length alphanumeric strings; the caller at
  // guest 0x922C1A68 asks for 20 bytes. These are stable across runs so anything
  // that keys off them stays consistent, and are plainly synthetic.
  const char* serial =
      device->type == kDeviceTypeHdd ? "REXGLUEHDD0000000001" : "REXGLUEODD0000000001";
  if (!WriteAnsi(serial_buffer, serial_capacity, serial)) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  REXKRNL_INFO("XamContentGetDeviceSerialNumber({:#x}) -> '{}'", device_id, serial);
  return X_ERROR_SUCCESS;
}

// Taken over from the SDK so the enumerated records describe the same drive as
// GetDeviceData above. The storage list is built from what XamEnumerate streams
// out of here, so leaving the SDK's version in place would have shown the drive
// as "Dummy HDD" at 20GB while every direct query reported the real volume.
//
// Behaviour is otherwise identical to the SDK's: same Initialize arguments,
// same buffer-size contract, and both devices appended in the same order, which
// matters because the dashboard's HDD probe at guest 0x922C0000 walks the list
// one record at a time until it sees device_type == 1.
u32 CreateDeviceEnumerator_entry(u32 content_type, u32 content_flags, u32 max_count,
                                 mapped_u32 buffer_size_ptr, mapped_u32 handle_out) {
  if (!handle_out) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<uint32_t>(sizeof(X_CONTENT_DEVICE_DATA)) * max_count;
  }

  auto e = rex::system::make_object<rex::system::XStaticEnumerator<X_CONTENT_DEVICE_DATA>>(
      REX_KERNEL_STATE(), max_count);
  const auto result = e->Initialize(0xFE, 0xFE, 0x2000A, 0x20009, 0);
  if (XFAILED(result)) {
    REXKRNL_WARN("XamContentCreateDeviceEnumerator: Initialize failed {:#x}", result);
    return result;
  }

  for (const auto& device : kDevices) {
    auto* data = e->AppendItem();
    if (data == nullptr) {
      break;
    }
    FillDeviceData(data, device);
  }

  *handle_out = e->handle();
  REXKRNL_INFO(
      "XamContentCreateDeviceEnumerator(type={:#x} flags={:#x} max={}) -> {} device(s), handle={:#x}",
      content_type, content_flags, max_count, std::size(kDevices), e->handle());
  return X_ERROR_SUCCESS;
}

//=============================================================================
// XamEnumerate
//=============================================================================
//
// The SDK's xeXamEnumerate opens with assert_true(flags == 0), and rexruntimed
// is built against the debug CRT with asserts live: it imports _wassert, and the
// expression "flags == 0" together with "xam_enum.cpp" is present in the image
// as UTF-16.
//
// Every storage path in the dashboard passes flags = 2 -- the list builder at
// guest 0x92201E60, the HDD-id cache at 0x922C8690, the volume walk at
// 0x922C8768 and the theme scan at 0x922E80A8 all call XamEnumerate(h, 2, ...).
// So the moment the storage screen became reachable at all, the first enumerate
// killed the process: exit code 3, abort() inside ucrtbased called from
// rexruntimed, entered from 0x92201E60.
//
// Reimplemented here without that assert. The flag does not otherwise change
// anything -- the SDK ignores it, and WriteItems pages through the enumerator
// identically either way -- so record it once and carry on. The rest is a
// faithful copy of the SDK's behaviour, overlapped path included, because this
// override is global: content, profile and device enumeration all come through
// here, not just storage.
u32 Enumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length,
                    mapped_u32 items_returned, u32 overlapped_ptr) {
  // buffer_length cannot be trusted and is deliberately unused.
  //
  // IDA types it as cbBuffer from the XDK prototype, but the guest does not use
  // it that way consistently. The device enumerator at guest 0x922E80A8 passes
  // 0x320 for a 848-byte buffer of 80-byte records -- a byte count. The content
  // manager at guest 0x92201DA0 passes 1 for a 560-byte stack buffer it fills
  // one 512-byte record at a time -- not a byte count by any reading:
  //
  //     li   r6, 1                      ; "cbBuffer"
  //     addi r5, r1, 0x290+var_230      ; pvBuffer, 560 bytes of frame
  //     li   r4, 0x61E                  ; dwFlags
  //     bl   XamEnumerate
  //
  // Sizing anything off it is therefore unsafe in both directions, and clearing
  // the buffer with it was tried and does not work: against that call it zeroes
  // one byte. Records are instead written at their full size by the enumerators
  // themselves, which is where the guest's 512-byte record is honoured -- see
  // content_enum.cpp.
  (void)buffer_length;

  if (flags != 0) {
    static uint32_t s_reported = 0;
    if (s_reported != flags) {
      s_reported = flags;
      REXKRNL_INFO("XamEnumerate: flags={:#x} (SDK asserts on this; ignoring as it ignores it)",
                   flags);
    }
  }

  auto e = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [e, buffer, handle, flags](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    if (!buffer) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->WriteItems(buffer.guest_address(), buffer.as<uint8_t*>(), &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;

    // The Game Library reads through the overlapped form, and this lambda is
    // the only place that path writes anything -- the logging further down runs
    // after the early return for overlapped calls, so async enumeration was
    // invisible. The guest reads this item count straight out of the overlapped
    // (XGetOverlappedResult at guest 0x9214A948 hands back overlapped[1]), and
    // the library accumulates the list from it at 0x922F3150, so this number is
    // exactly what the list ends up holding.
    static uint32_t s_async_logged = 0;
    if (s_async_logged < 48) {
      ++s_async_logged;
      REXKRNL_INFO("XamEnumerate[async]: handle {:#x} flags {:#x} -> result {:#x}, {} item(s)",
                   handle, flags, result, item_count);
    }
    return result;
  };

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }

  uint32_t extended_error = 0;
  uint32_t item_count = 0;
  const X_RESULT result = run(extended_error, item_count);
  if (items_returned) {
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
  }

  // Temporary: shows whether the guest actually drains an enumerator it was
  // given, which is the difference between "the list never reached the UI" and
  // "the UI received it and dropped it".
  {
    static uint32_t s_logged = 0;
    if (s_logged < 96) {
      ++s_logged;
      REXKRNL_INFO("XamEnumerate: handle {:#x} flags {:#x} -> result {:#x}, {} item(s)", handle,
                   flags, result, item_count);
    }
  }
  return result;
}

//=============================================================================
// Announcing the drive
//=============================================================================
//
// Enumerating devices correctly is not enough on its own: the dashboard does
// not poll for storage. It learns about it from XN_SYS_STORAGEDEVICESCHANGED
// (0x0B), handled by its notification pump at guest 0x921422C0:
//
//     case 0x9:  ...                 XN_SYS_UI
//     case 0xA:  sub_92141DA0(a1);   XN_SYS_SIGNINCHANGED
//     case 0xB:  sub_9213F5A8(a1); dash_293f(v4); sub_92140FE0(a1);
//     case 0xE:  sub_922A8478();     XN_SYS_PROFILESETTINGCHANGED
//
// and which builds the device objects through the handler at guest 0x922B8868.
//
// Nothing in the runtime ever raises 0x0B. KernelState::RegisterNotifyListener
// seeds a startup burst to the first listener -- 0x09, 0x0A, and 0x12/0x13 for
// input devices -- and BroadcastNotification is only ever called for 0x09,
// 0x0A and XMP messages. That list is inherited from Xenia and is tuned for
// games, which do not care; the dashboard's Storage Devices screen is built
// entirely from this event, so without it the screen never enumerates at all.
// That is why none of the entry points above were reached even once during a
// 45-second run with logging on every one of them.
//
// A console raises this whenever storage is attached or removed. Here the drive
// is present from boot and never changes, so there is exactly one true event:
// it appeared, once, while the dashboard was starting. It is announced to the
// first system-area listener, and to nothing afterwards.
//
// It used to be announced to EVERY system-area listener as that listener was
// created, on the reasoning that delivering per listener is what a broadcast
// does and that it saved having to work out which listener the storage screen
// pumps. That was wrong twice over, and it cost the Change Theme screen.
//
// Wrong about who needs it. The listener that matters is not a guess: the pump
// at guest 0x921422C0 is the only thing in the image that handles 0x0B --
//
//     while ( XNotifyGetNext(*(HANDLE *)(a1 + 340), 0, &v12, &v11) )
//         case 0xBu: v4 = sub_9213F5A8(a1); dash_293f(v4); sub_92140FE0(a1);
//
// and a1+340 is filled in exactly once, by the dashboard's startup at guest
// 0x92141EF0:
//
//     *(_DWORD *)(a1 + 340) = XamNotifyCreateListenerInternal(0x2300000001uLL, 5, v9);
//
// which is the first listener created on boot. So one announcement to the first
// system-area listener reaches the storage handler, and every later listener was
// only ever being told something untrue.
//
// Wrong about the harm. A scene that creates a listener is not asking "what is
// the state of the world", it is asking "tell me when something changes", and
// roughly twenty scenes in this image do exactly that. Announcing per listener
// told every screen in the dashboard that a drive had just been plugged in or
// pulled out, the moment that screen opened.
//
// The Change Theme screen is the one that could not survive it. Its picker
// creates a listener and schedules a tick, at guest 0x922E8BB0:
//
//     ListenerInternal = XamNotifyCreateListenerInternal(0x300000001uLL, 5, v9);
//     *(_DWORD *)(a1 + 24) = ListenerInternal;
//     dash_2736(*(_DWORD *)(a1 + 4), 1104, 300);      // tick every 300ms
//
// and that tick, at guest 0x922E8CB0, closes the screen if anything it depends
// on has changed:
//
//     while ( XNotifyGetNext(*(HANDLE *)(a1 + 24), 0, &v18, v19) )
//         if ( v18 == 11 || v18 == 10 || v18 == 33554433 || v18 == 14 )
//             v6 = 1;
//     if ( v6 ) { ... dash_280f(v7, v8, 255); }        // navigate back
//
// 11 is this notification. So the theme screen loaded, initialised and pushed
// perfectly -- every step returned 0 on nine consecutive presses -- and then
// tore itself down within 300ms because of this line. From outside it looked
// like the button did nothing.
//
// Anything else that quietly refreshed or backed out on open was doing so for
// the same reason.
constexpr uint32_t kXnSysStorageDevicesChanged = 0x0000000B;

// The one announcement. Guarded so it happens once per boot, which is the same
// discipline the runtime's own startup burst uses (has_notified_startup_).
bool g_announced_storage = false;

// A note on the arguments, because they are easy to get wrong from the
// decompiler alone.
//
// IDA types the first parameter as a qword and renders the call sites as
// XamNotifyCreateListenerInternal(0x2300000001uLL, 5, v9). That reconstruction
// is wrong -- it is pairing r3 with r4 as though this were a 32-bit ABI. The
// recompiled call sites show the mask is entirely in r3:
//
//     li r5, 5        ; max_version
//     li r4, 1        ; is_system
//     li r3, 35       ; mask -- 0x23, the whole thing
//     bl XamNotifyCreateListenerInternal
//
// so the masks in play are 0x23 (dashboard startup), 0x1 (the overlapped pump at
// 0x92141980) and 0x3 (the theme picker), and each bit is a notification area:
// bit 0 system, bit 1 LIVE. That reading is what makes the guest's own code
// consistent -- the theme picker tests for 0x02000001, a LIVE-area id, which it
// could only ever receive with bit 1 set. Under the qword reading its mask would
// be 0x300000001, whose bit 1 is clear, making that test dead code.
//
// These are raw hooks only so the register-to-parameter mapping is written out
// explicitly rather than left to the marshaller.
u32 CreateNotifyListener(uint64_t mask, uint32_t is_system, uint32_t max_version,
                         const char* who) {
  if (max_version > 10) {
    max_version = 10;
  }

  auto listener = rex::system::object_ref<rex::system::XNotifyListener>(
      new rex::system::XNotifyListener(REX_KERNEL_STATE()));
  listener->Initialize(mask, max_version);
  const uint32_t handle = listener->handle();

  // Bit 0 selects the system notification area -- the same test
  // RegisterNotifyListener applies before seeding its own startup burst.
  const bool system_area = (mask & 0x1) != 0;
  if (system_area && !g_announced_storage) {
    g_announced_storage = true;
    listener->EnqueueNotification(kXnSysStorageDevicesChanged, 0);
    REXKRNL_INFO("{}(mask={:#x}, is_system={}, version={}) -> handle={:#x}; announced the drive "
                 "to this listener, the one the storage pump at 0x921422C0 reads",
                 who, mask, is_system, max_version, handle);
  } else {
    REXKRNL_DEBUG("{}(mask={:#x}, is_system={}, version={}) -> handle={:#x}", who, mask, is_system,
                  max_version, handle);
  }
  return handle;
}

}  // namespace

// XamNotifyCreateListener(mask, max_version) -- two arguments; guest 0x9249B868
// tail-calls it with "li r4,3" for the version and the mask already in r3.
REX_HOOK_RAW(__imp__XamNotifyCreateListener) {
  ctx.r3.u64 = CreateNotifyListener(ctx.r3.u32, 0, ctx.r4.u32, "XamNotifyCreateListener");
}

REX_HOOK_RAW(__imp__XamNotifyCreateListenerInternal) {
  ctx.r3.u64 =
      CreateNotifyListener(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, "XamNotifyCreateListenerInternal");
}

// XNotifyBroadcast(id, data) -- another bare REX_EXPORT_STUB, and the reason a
// change the dashboard makes to itself never reaches the rest of the dashboard.
//
// The guest raises this whenever it changes something other screens depend on.
// Applying a theme ends with exactly that, at guest 0x922E7AD0:
//
//     sub_9249A4C0(v4, a1, 324, &v6, 0);       // write ThematicSkin
//     ...
//     return XNotifyBroadcast(0x8000000F, (PVOID)(1 << v2));
//
// and 0x8000000F is handled by the main pump at guest 0x921422C0:
//
//     if ( v12 == -2147483633 )                // 0x8000000F
//         sub_92140B88(a1, 1);                 // re-read ThematicSkin, apply it
//
// With the broadcast going nowhere the theme was written and then nobody was
// told, so the dashboard carried on with the look it already had.
//
// KernelState::BroadcastNotification already does the right thing -- it delivers
// to every registered listener whose mask covers the notification's area, which
// is what a broadcast is. This just stops throwing the call away.
REX_HOOK_RAW(__imp__XNotifyBroadcast) {
  const uint32_t id = ctx.r3.u32;
  const uint32_t data = ctx.r4.u32;

  REX_KERNEL_STATE()->BroadcastNotification(id, data);

  static uint32_t s_logged = 0;
  if (++s_logged <= 8) {
    REXKRNL_INFO("XNotifyBroadcast({:#010x}, {:#x})", id, data);
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

REX_EXPORT(__imp__XamEnumerate, Enumerate_entry)
REX_EXPORT(__imp__XamContentCreateDeviceEnumerator, CreateDeviceEnumerator_entry)
REX_EXPORT(__imp__XamContentGetDeviceData, GetDeviceData_entry)
REX_EXPORT(__imp__XamContentGetDefaultDevice, GetDefaultDevice_entry)
REX_EXPORT(__imp__XamContentGetDeviceVolumePath, GetDeviceVolumePath_entry)
REX_EXPORT(__imp__XamContentGetDeviceSerialNumber, GetDeviceSerialNumber_entry)
