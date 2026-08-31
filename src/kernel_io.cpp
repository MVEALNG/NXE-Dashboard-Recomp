// Kernel I/O overrides.
//
// NtDeviceIoControlFile
// ---------------------
// The SDK's version handles exactly two disk IOCTLs -- DISK_GET_DRIVE_GEOMETRY
// (0x70000) and DISK_GET_PARTITION_INFO (0x74004), both added for cache-mount
// code -- and ends its else branch with:
//
//     REXKRNL_DEBUG("NtDeviceIoControlFile(0x{:X}) - unhandled IOCTL!", ...);
//     assert_always();
//     return X_STATUS_INVALID_PARAMETER;
//
// rexruntimed is built against the debug CRT with asserts live (it imports
// _wassert), so assert_always() is a real abort(), not a no-op.
//
// The dashboard opens the raw device \Device\Harddisk0\Partition0 at guest
// 0x922A81A8 and issues IOCTL 0x70042 against it. That is neither of the two
// handled codes, so the process died with exit code 3 the moment that path ran.
//
// The guest handles failure perfectly well -- 0x922A81A8 only proceeds into its
// partition-parsing block when the call returns >= 0, and simply skips it
// otherwise. So return the same X_STATUS_INVALID_PARAMETER the SDK intended to
// return, minus the abort. Reporting "this runtime cannot service a raw-disk
// IOCTL" is the truthful answer here; there is no real partition table behind
// \Partition0, only the SDK's NullDevice.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

// X_STATUS_* expand through a cast to rex::X_RESULT.
using namespace rex;

namespace {

// The guest is big-endian and these buffers carry no alignment guarantee, so
// write them a byte at a time rather than through a be<> overlay.
void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

void StoreBe64(uint8_t* p, uint64_t v) {
  StoreBe32(p, static_cast<uint32_t>(v >> 32));
  StoreBe32(p + 4, static_cast<uint32_t>(v));
}

u32 DeviceIoControlFile_entry(u32 handle, u32 event_handle, u32 apc_routine, u32 apc_context,
                              u32 io_status_block, u32 io_control_code, mapped_void input_buffer,
                              u32 input_buffer_len, mapped_void output_buffer,
                              u32 output_buffer_len) {
  (void)handle;
  (void)event_handle;
  (void)apc_routine;
  (void)apc_context;
  (void)io_status_block;
  (void)input_buffer;
  (void)input_buffer_len;

  // Same values the SDK reports, so cache-mount code behaves identically.
  constexpr uint32_t kCacheSize = 0xFF000;
  constexpr uint32_t kIoctlDiskGetDriveGeometry = 0x70000;
  constexpr uint32_t kIoctlDiskGetPartitionInfo = 0x74004;

  auto* out = output_buffer.as<uint8_t*>();

  if (io_control_code == kIoctlDiskGetDriveGeometry) {
    if (output_buffer_len < 0x8) {
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    if (out == nullptr) {
      return X_STATUS_INVALID_PARAMETER;
    }
    StoreBe32(out, kCacheSize / 512);
    StoreBe32(out + 4, 512);
    return X_STATUS_SUCCESS;
  }

  if (io_control_code == kIoctlDiskGetPartitionInfo) {
    if (output_buffer_len < 0x10) {
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    if (out == nullptr) {
      return X_STATUS_INVALID_PARAMETER;
    }
    StoreBe64(out, 0);
    StoreBe64(out + 8, kCacheSize);
    return X_STATUS_SUCCESS;
  }

  // Report each distinct unhandled code once; the dashboard retries these.
  static uint32_t s_last_reported = 0;
  if (s_last_reported != io_control_code) {
    s_last_reported = io_control_code;
    REXKRNL_WARN("NtDeviceIoControlFile: unhandled IOCTL {:#x} - reporting failure", io_control_code);
  }
  return X_STATUS_INVALID_PARAMETER;
}

}  // namespace

REX_EXPORT(__imp__NtDeviceIoControlFile, DeviceIoControlFile_entry)
