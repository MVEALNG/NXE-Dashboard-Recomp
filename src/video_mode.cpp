// Screen resolution for the NXE dashboard.
//
// The Screen Resolution screen came up with an empty list, and its "Current
// Setting" line still showed the previous screen's text (a timezone) because
// nothing ever replaced it.
//
// The list does not come from VdEnumerateVideoModes. The dashboard carries its
// own mode tables and chooses between them based on how the display is
// attached, at guest 0x92218420:
//
//     v4 = 35;
//     if (!VdGetDisplayDiscoveryData(&v4)) goto keep_existing;
//     if      (type == 1) sel = 4;
//     else if (type == 2) sel = (~flags >> 7) & 1;
//     else if (type == 3) sel = ((~flags >> 7) & 1) | 2;
//     ...
//     if (sel == 4)         { memcpy(modes, table_10, 80); count = 10; }
//     if (sel == 2 || !sel) { memcpy(modes, table_4,  32); count = 4;  }
//
// table_10 at guest 0x92017F98 is the VGA/HDMI list -- 640x480, 848x480,
// 1024x768, 1280x720, 1280x768, 1280x1024, 1360x768, 1440x900, 1680x1050 and
// 1920x1080. table_4 at 0x92017F3C is the component list: 480p, 720p, 1080i,
// 1080p. Each entry is 8 bytes: width, height, label string id, flags, with
// 1080p stored as height -1080 to mark it progressive.
//
// VdGetDisplayDiscoveryData ships as a bare REX_EXPORT_STUB, which logs and
// never assigns r3. So the success test above read a garbage return value and
// the connection type was read straight off uninitialised stack. Whichever
// table got loaded, if any, was an accident -- hence the empty list.
//
// The contract below was read from the disassembly of 0x92218420 rather than
// guessed, because two parts of it are easy to get wrong:
//
//   r3        the buffer; the caller has ALREADY stored the struct size (35)
//             at +0 before the call
//   return    NON-ZERO means success -- "cmplwi r3, 0 / beq keep_existing" --
//             which is the opposite of the usual NTSTATUS convention
//   +0x1D     read with lwz -- a 32-bit big-endian load spanning 0x1D..0x20,
//             so it takes in four separate byte fields at once
//   +0x04     flags, whose bit 7 is consulted only for types 2 and 3
//
// The field layout was later cross-checked against xenia-canary PR #1021, which
// implements this call independently. That confirmed the 0x23 size and the
// non-zero-is-success convention, and corrected an assumption made here first
// time round: 0x1D is a one-byte video_output field, and the byte that actually
// lands in the low bits of the guest's 32-bit read is video_standard at 0x20.
// Setting that to 1 (NTSC) is what selects the 10-entry VGA/HDMI table. See the
// note on the struct below.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/xvideo.h>
#include <rex/types.h>

using namespace rex;

namespace {

// DisplayDiscoveryData, 0x23 bytes. Field names and values cross-checked
// against xenia-canary PR #1021 ("Added support for VdEnumerateVideoModes and
// VdGetDisplayDiscoveryData"), which implements the same call independently.
//
//   +0x00 u32  size                  +0x16 u8 physical_height_cm
//   +0x04 u32  flags                 +0x17 u8 unknown4
//   +0x08 u8   unknown               +0x18 u8 unknown5
//   +0x09 u8   gamma                 +0x19 u8 unknown6
//   +0x15 u8   physical_width_cm     +0x1A u8 unknown7
//   +0x1D u8   video_output          +0x20 u8 video_standard
//
// video_output is documented there as 1-VGA, 2-DVI, 3-HDMI, other-TV.
//
// Worth being precise about which byte the dashboard actually consults, because
// it is not the obvious one. Guest 0x92218420 does "lwz r10, +0x1D", a 32-bit
// big-endian load spanning 0x1D..0x20 -- so the value it compares against 1, 2
// and 3 is dominated by video_standard at 0x20, not by video_output at 0x1D.
// A video_standard of 1 (NTSC) is what makes it select the 10-entry VGA/HDMI
// mode table, which is the behaviour the Screen Resolution screen needs and
// what PR #1021 also produces.
constexpr uint32_t kDiscoveryDataSize = 0x23;

constexpr size_t kOffFlags = 0x04;
constexpr size_t kOffUnknown = 0x08;
constexpr size_t kOffGamma = 0x09;
constexpr size_t kOffPhysicalWidthCm = 0x15;
constexpr size_t kOffPhysicalHeightCm = 0x16;
constexpr size_t kOffUnknown4 = 0x17;
constexpr size_t kOffUnknown5 = 0x18;
constexpr size_t kOffUnknown6 = 0x19;
constexpr size_t kOffUnknown7 = 0x1A;
constexpr size_t kOffVideoOutput = 0x1D;
constexpr size_t kOffVideoStandard = 0x20;

// HDMI audio capability bits.
constexpr uint32_t kDiscoveryFlags = 0x19E;
constexpr uint8_t kGamma22 = 0x78;    // 2.2
constexpr uint8_t kVideoOutputTv = 0;
constexpr uint8_t kVideoStandardNtsc = 1;

void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

uint32_t LoadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

u32 GetDisplayDiscoveryData_entry(mapped_void data) {
  auto* p = data.as<uint8_t*>();
  if (p == nullptr) {
    return 0;  // zero is failure here
  }

  // The caller stores the struct size before calling; anything else is not a
  // record this can fill.
  if (LoadBe32(p) != kDiscoveryDataSize) {
    REXKRNL_WARN("VdGetDisplayDiscoveryData: buffer declares {} bytes, expected {}", LoadBe32(p),
                 kDiscoveryDataSize);
    return 0;
  }

  std::memset(p, 0, kDiscoveryDataSize);
  StoreBe32(p, kDiscoveryDataSize);
  StoreBe32(p + kOffFlags, kDiscoveryFlags);
  p[kOffUnknown] = 0x30;
  p[kOffGamma] = kGamma22;
  p[kOffPhysicalWidthCm] = 0x50;
  p[kOffPhysicalHeightCm] = 0x22;
  p[kOffUnknown4] = 1;
  p[kOffUnknown5] = 3;
  p[kOffUnknown6] = 3;
  p[kOffUnknown7] = 2;
  p[kOffVideoOutput] = kVideoOutputTv;
  p[kOffVideoStandard] = kVideoStandardNtsc;

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("VdGetDisplayDiscoveryData -> standard {} (selects the VGA/HDMI mode table)",
                 kVideoStandardNtsc);
  }
  return 1;  // non-zero is success
}

// The mode the display is actually running.
//
// Also a bare REX_EXPORT_STUB, so it never wrote the caller's buffer at all --
// guest 0x92219000 reads width/height/interlaced straight back out of it to
// find which row of the table is the current one, and 0x92217548 stores it in
// its display record. Both were reading uninitialised stack, which is why no
// row came up selected.
//
// There is only one output mode in this runtime, so the "real" mode and the
// mode the guest renders at are the same thing; report what VdQueryVideoMode
// reports rather than inventing a second answer that could disagree with it.
void QueryRealVideoMode_entry(mapped_void video_mode) {
  auto* mode = video_mode.as<rex::system::X_VIDEO_MODE*>();
  if (mode == nullptr) {
    return;
  }
  rex::kernel::xboxkrnl::VdQueryVideoMode(mode);
}

// Which AV connector the console thinks it has.
//
// Once XConfig writes actually persisted, the dashboard began committing a
// display configuration and then reconfiguring the output around it -- and the
// GPU ring buffer blew up immediately after, every run:
//
//     XConfig: set 0x0015 = 0x028001e0   (640x480, packed width<<16|height)
//     XConfig: set 0x000A = 0x00040000   (VIDEO_FLAGS)
//     HalGetCurrentAVPack STUB
//     [gpu] ExecutePacketType0 overflow (read count 28, packet count 0000FE00)
//     [gpu] **** INDIRECT RINGBUFFER: Failed to execute packet.
//
// HalGetCurrentAVPack is another bare REX_EXPORT_STUB, so the branch that
// decides how to drive the display was taken on an undefined r3.
//
// The value here is not a choice. The xam-level XGetAVPack answers the same
// physical question and the SDK already implements it as 6 (VGA); the Display
// screen visibly agrees, printing "VGA" beneath 1280 x 720. A HAL-level query
// reporting something different would be describing a second, non-existent
// connector.
constexpr uint32_t kAvPackVga = 6;

u32 GetCurrentAVPack_entry() { return kAvPackVga; }

}  // namespace

REX_EXPORT(__imp__HalGetCurrentAVPack, GetCurrentAVPack_entry)
REX_EXPORT(__imp__VdGetDisplayDiscoveryData, GetDisplayDiscoveryData_entry)
REX_EXPORT(__imp__VdQueryRealVideoMode, QueryRealVideoMode_entry)
