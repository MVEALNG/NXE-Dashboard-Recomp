// Resolving a content record to the package behind it.
//
// This is the last step of applying a theme, and it was a bare
// REX_EXPORT_STUB -- so it logged a warning, never assigned r3, and the guest
// branched on whatever the register happened to hold.
//
// The apply chain is long and every other part of it now works. Picking a theme
// writes the chosen content record to the profile (SkinRoot:\ThematicSkin, 324
// bytes, verified on disk), the dashboard reads it back at guest 0x9213F430,
// and the applier at guest 0x92143E98 walks the storage devices looking for the
// package it names:
//
//     while ( !XamEnumerate(v8, 2u, v12, 0x320u, &v7, 0) )
//         ...
//         *(_DWORD *)a1 = *v6;              // try this device
//         if ( !sub_92143D98(a1) ) break;   // stop at the first that resolves
//
// and 0x92143D98 is where it died:
//
//     Internal = XamContentResolveInternal(a1, &v5, 0x104u, 0, 0, 0);
//     if ( !Internal )
//     {
//         Internal = XamContentCreateInternal(off_9279F64C, a1, 3, 0, 0, 0, 0, 0);
//         if ( !Internal ) { snprintf(v4, 0x10u, "%s:", off_9279F64C);
//                            sub_92143D10(v4); XamContentClose(off_9279F64C, 0); }
//     }
//
// With this stubbed the package was never mounted, so sub_92143D10 never ran,
// so no WallPaper1..8 was ever loaded and the dashboard kept the built-in look.
// That is the "it always picks the default theme" symptom: nothing failed
// visibly, the chain just stopped one call short of doing anything.
//
// What it does
// ------------
// Answers whether the content this record names actually exists, and where. The
// record is read straight out of guest memory rather than through a struct, so
// there is no layout assumption to get wrong -- the offsets below were read off
// a real ThematicSkin written by the dashboard itself:
//
//     +0x000 device_id   00 00 00 00
//     +0x004 type        00 03 00 00                     kTheme
//     +0x008 name        "Halo 3 Unite to Fight Theme"   UTF-16BE
//     +0x108 file_name   "5AF06FF870DEF3105B7BA419..."   ANSI
//     +0x138 xuid        0
//     +0x140 title_id    ff fe 07 d1
//
// The path is the same one the content enumerator and the installer already
// use, so a package that enumerates is a package that resolves:
//
//     Content/{xuid:016X}/{title:08X}/{type:08X}/{file_name}
//
// Only the hard drive can answer yes. The applier assigns each enumerated device
// id into the record in turn and takes the first that resolves, so claiming
// success for the disc drive would mount the wrong device; there is no content
// on the emulated ODD.
//
// The out buffer is filled in for honesty, not because anything reads it --
// neither call site (0x92143D98 or 0x922E7FC0) looks at it again, both only test
// the return value.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "storage_device.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace rex;

namespace {

// Offsets within the guest's content record.
constexpr uint32_t kOffDeviceId = 0x000;
constexpr uint32_t kOffContentType = 0x004;
constexpr uint32_t kOffFileName = 0x108;
constexpr uint32_t kOffXuid = 0x138;
constexpr uint32_t kOffTitleId = 0x140;
constexpr size_t kFileNameMax = 42;

// Must match storage_device.cpp: the drive is device 1.
constexpr uint32_t kDeviceIdHdd = 1;

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t Be64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | p[i];
  }
  return v;
}

std::string Ansi(const uint8_t* p, size_t max_len) {
  std::string out;
  for (size_t i = 0; i < max_len && p[i]; ++i) {
    out.push_back(static_cast<char>(p[i]));
  }
  return out;
}

// Content/{xuid:016X}/{title:08X}/{type:08X}/{file_name}
std::filesystem::path PackagePath(uint64_t xuid, uint32_t title_id, uint32_t content_type,
                                  const std::string& file_name) {
  char xuid_dir[17] = {};
  std::snprintf(xuid_dir, sizeof(xuid_dir), "%016llX", static_cast<unsigned long long>(xuid));
  char title_dir[9] = {};
  std::snprintf(title_dir, sizeof(title_dir), "%08X", title_id);
  char type_dir[9] = {};
  std::snprintf(type_dir, sizeof(type_dir), "%08X", content_type);
  return nxe_storage::ContentRoot() / xuid_dir / title_dir / type_dir / file_name;
}

// The package a record names, wherever it is staged.
std::filesystem::path FindPackage(uint8_t* base, uint32_t record) {
  const uint8_t* p = base + record;
  const uint32_t content_type = Be32(p + kOffContentType);
  const uint64_t xuid = Be64(p + kOffXuid);
  const uint32_t title_id = Be32(p + kOffTitleId);
  const std::string file_name = Ansi(p + kOffFileName, kFileNameMax);
  if (file_name.empty()) {
    return {};
  }

  std::error_code ec;
  auto path = PackagePath(xuid, title_id, content_type, file_name);
  if (!std::filesystem::exists(path, ec) && xuid != 0) {
    path = PackagePath(0, title_id, content_type, file_name);
  }
  return std::filesystem::exists(path, ec) ? path : std::filesystem::path{};
}

//===========================================================================
// Mounting a content package
//===========================================================================
//
// XamContentCreateInternal is the call after the resolve, and it is the one
// still failing. It is not a stub -- the runtime implements it -- but with the
// resolve now succeeding and the package present on disk, the theme still never
// appears, and the log shows why: after the resolve there is no VFS activity at
// all for the theme root, and no WallPaper is ever opened. sub_92143D10 loads
//
//     file://<root>:\WallPaper1 .. WallPaper8
//
// so if the root were mounted those lookups would appear whether they succeeded
// or not. Nothing does, which places the failure inside the mount itself.
//
// Rather than replace the runtime's implementation, this wraps it: the real one
// runs first and its result is used whenever it succeeds, so every content path
// that already works is untouched. Only when it fails does this mount the
// package directly -- the same way the profile is mounted in nxe_dash_app.h,
// which is proven to work: a host-backed device at its own path, with the
// guest's root name symlinked to it.
//
// The device path deliberately avoids \Device\Harddisk0. The runtime's
// NullDevice claims that whole prefix and shadows anything registered under it
// afterwards, which is what defeated the first attempt at the profile mount --
// see kProfileMountPath in storage_device.h.
using GuestFunc = void (*)(PPCContext&, uint8_t*);

GuestFunc RuntimeFunc(const char* name) {
  HMODULE module = GetModuleHandleA("rexruntimed.dll");
  if (module == nullptr) {
    module = GetModuleHandleA("rexruntime.dll");
  }
  if (module == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<GuestFunc>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// Roots this file mounted itself, so only those get torn down on close.
std::map<std::string, std::string>& OurMounts() {
  static std::map<std::string, std::string> mounts;
  return mounts;
}

bool MountPackageAs(const std::string& root_name, const std::filesystem::path& package) {
  auto* fs = REX_KERNEL_FS();
  if (fs == nullptr) {
    return false;
  }

  static uint32_t s_next = 0;
  char device_path[64] = {};
  std::snprintf(device_path, sizeof(device_path), "\\Device\\ContentPkg%u", s_next++);

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(device_path, package,
                                                                  /*read_only=*/true);
  if (!device->Initialize() || !fs->RegisterDevice(std::move(device))) {
    REXKRNL_WARN("XamContentCreateInternal: could not mount {} at {}", package.string(),
                 device_path);
    return false;
  }

  const std::string link = root_name + ":";
  fs->UnregisterSymbolicLink(link);
  fs->RegisterSymbolicLink(link, device_path);
  OurMounts()[root_name] = device_path;

  REXKRNL_INFO("XamContentCreateInternal: mounted '{}' -> {} ({})", link, device_path,
               package.filename().string());
  return true;
}

}  // namespace

REX_HOOK_RAW(__imp__XamContentResolveInternal) {
  const uint32_t record = ctx.r3.u32;
  const uint32_t out_addr = ctx.r4.u32;
  const uint32_t out_chars = ctx.r5.u32;

  if (!record) {
    ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
    return;
  }

  const uint8_t* p = base + record;
  const uint32_t device_id = Be32(p + kOffDeviceId);
  const uint32_t content_type = Be32(p + kOffContentType);
  const uint64_t xuid = Be64(p + kOffXuid);
  const uint32_t title_id = Be32(p + kOffTitleId);
  const std::string file_name = Ansi(p + kOffFileName, kFileNameMax);

  if (file_name.empty()) {
    ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
    return;
  }

  // Only the drive holds content here; see the note above about the applier
  // trying each device in turn.
  if (device_id != kDeviceIdHdd && device_id != 0 && device_id != 0xFFFFFFFFu) {
    ctx.r3.u64 = X_ERROR_PATH_NOT_FOUND;
    return;
  }

  // Content staged for nobody in particular lives under XUID 0, which is where
  // the installed themes are; content staged for the signed-in user lives under
  // theirs. Try what the record says first, then the common tree.
  std::error_code ec;
  auto path = PackagePath(xuid, title_id, content_type, file_name);
  if (!std::filesystem::exists(path, ec) && xuid != 0) {
    path = PackagePath(0, title_id, content_type, file_name);
  }
  if (!std::filesystem::exists(path, ec)) {
    static uint32_t s_missing = 0;
    if (++s_missing <= 4) {
      REXKRNL_WARN("XamContentResolveInternal: no package at {}", path.string());
    }
    ctx.r3.u64 = X_ERROR_PATH_NOT_FOUND;
    return;
  }

  // The guest-visible form of the same location. Written so the buffer holds
  // something true; nothing reads it back.
  if (out_addr && out_chars) {
    char guest_path[260] = {};
    std::snprintf(guest_path, sizeof(guest_path), "%s\\Content\\%016llX\\%08X\\%08X\\%s",
                  nxe_storage::kHddMountPath, static_cast<unsigned long long>(xuid), title_id,
                  content_type, file_name.c_str());
    const size_t room = out_chars < sizeof(guest_path) ? out_chars : sizeof(guest_path);
    const size_t len = std::strlen(guest_path);
    const size_t copied = len < room - 1 ? len : room - 1;
    std::memset(base + out_addr, 0, room);
    std::memcpy(base + out_addr, guest_path, copied);
  }

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamContentResolveInternal: type {:#010x} title {:#010x} '{}' -> {}", content_type,
                 title_id, file_name, path.string());
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamContentCreateInternal(root_name, record, flags, disposition, license, ...)
//
// The runtime's implementation runs first and wins whenever it succeeds. Only
// its failure is taken over, and the failure is reported once so the takeover is
// visible rather than silent.
REX_HOOK_RAW(__imp__XamContentCreateInternal) {
  const uint32_t root_addr = ctx.r3.u32;
  const uint32_t record = ctx.r4.u32;
  const uint32_t flags = ctx.r5.u32;
  const uint32_t disposition_ptr = ctx.r6.u32;
  const std::string root_name = root_addr ? Ansi(base + root_addr, 64) : std::string();

  if (auto* forward = RuntimeFunc("__imp__XamContentCreateInternal")) {
    PPCContext saved = ctx;
    forward(ctx, base);
    const uint32_t runtime_result = ctx.r3.u32;

    // Success is exactly zero, which is the test the guest itself applies:
    //
    //     Internal = XamContentCreateInternal(off_9279F64C, a1, 3, 0, 0, 0, 0, 0);
    //     if ( !Internal ) { ... the mount is usable ... }
    //
    // This call reports Win32 error codes as small positive numbers rather than
    // HRESULTs, so "not negative" is not success. An earlier version of this
    // wrapper tested it that way, and the runtime returns 3 -- ERROR_PATH_NOT_FOUND
    // -- which it read as success and handed straight back, so the fallback below
    // never ran and the theme failed to mount exactly as it had before.
    const bool ok = runtime_result == 0;

    // Logged either way. Which of these two lines appears is the difference
    // between "the package was never mounted" and "it was mounted and something
    // later failed", and guessing at that from an absence of log lines has
    // already cost a round trip.
    static uint32_t s_reported = 0;
    if (++s_reported <= 6) {
      REXKRNL_INFO("XamContentCreateInternal('{}', flags {:#x}) -> runtime {:#x}{}", root_name,
                   flags, runtime_result, ok ? "" : ", mounting the package directly");
    }
    if (ok) {
      return;  // the runtime handled it
    }
    ctx = saved;
  }

  if (root_name.empty() || !record) {
    ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
    return;
  }

  const auto package = FindPackage(base, record);
  if (package.empty() || !MountPackageAs(root_name, package)) {
    ctx.r3.u64 = X_ERROR_FILE_NOT_FOUND;
    return;
  }

  // "Opened an existing package", which is what happened.
  if (disposition_ptr) {
    uint8_t* d = base + disposition_ptr;
    d[0] = 0;
    d[1] = 0;
    d[2] = 0;
    d[3] = 2;
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamContentClose(root_name, overlapped) -- tear down only what this file
// mounted, then let the runtime do whatever it would have done anyway.
REX_HOOK_RAW(__imp__XamContentClose) {
  const uint32_t root_addr = ctx.r3.u32;
  const std::string root_name = root_addr ? Ansi(base + root_addr, 64) : std::string();

  auto& mounts = OurMounts();
  auto it = mounts.find(root_name);
  if (it != mounts.end()) {
    if (auto* fs = REX_KERNEL_FS()) {
      fs->UnregisterSymbolicLink(root_name + ":");
      fs->UnregisterDevice(it->second);
    }
    mounts.erase(it);
  }

  if (auto* forward = RuntimeFunc("__imp__XamContentClose")) {
    forward(ctx, base);
    return;
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
