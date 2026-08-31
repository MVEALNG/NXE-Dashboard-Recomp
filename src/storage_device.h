// Host-backed storage device for the NXE dashboard.
#pragma once

#include <filesystem>

namespace nxe_storage {

// VFS mount for the emulated hard drive. Partition1 is already taken by
// game_data_root (Runtime::SetupVfs), so the storage volume gets its own
// device path, which is also what XamContentGetDeviceVolumePath reports.
inline constexpr char kHddMountPath[] = "\\Device\\Harddisk0\\Partition3";

// VFS mount for the signed-in profile's own directory, which XamProfileOpen
// then links to whatever name the guest asks for ("DASHUSER", "SkinRoot").
//
// Deliberately NOT under \Device\Harddisk0. The runtime registers a NullDevice
// for \Device\Harddisk0\{{Partition0,Cache0,Cache1}}, and that device matches on
// the \Device\Harddisk0 prefix, so it swallows every path beneath it that was
// registered after it -- kHddMountPath included:
//
//     Registered NullDevice for \Device\Harddisk0\{{Partition0,Cache0,Cache1}}
//     Mounted storage device A:/Xbox360Storage at \Device\Harddisk0\Partition3
//     ...
//     NullDevice::ResolvePath(\Partition3\Content\...\E000683F000088EB)
//
// Partition1 escapes it only because game_data_root is mounted before the
// NullDevice exists. \Device\Mass0 -- a real 360 device name, unused here --
// is outside that prefix entirely.
inline constexpr char kProfileMountPath[] = "\\Device\\Mass0";

// The optical drive. The device list already advertises this path for the
// Disc Drive; mounting a game there is what lets the dashboard read it.
inline constexpr char kOddMountPath[] = "\\Device\\Cdrom0";

// Host directory backing the drive: the volume root, holding Content/,
// Cache/, $SystemUpdate/ and Compatibility/ like a real 360 data partition.
const std::filesystem::path& Root();

// root/Content -- the directory rexglue's ContentManager treats as its
// content root, i.e. the one that directly contains {XUID:016X} folders.
const std::filesystem::path& ContentRoot();

}  // namespace nxe_storage
