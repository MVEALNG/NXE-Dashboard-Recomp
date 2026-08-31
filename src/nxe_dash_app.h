// nxe_dash - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/filesystem/vfs.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/system/kernel_state.h>

#include <filesystem>

REXCVAR_DECLARE(std::string, disc_title);

#include "install_paths.h"
#include "discord_presence.h"
#include "game_launch.h"
#include "storage_device.h"

// Defined by the SDK's avatar pipeline (kernel/xam/xam_avatar.cpp). It looks
// for AvatarAssetPack.toc here, falling back to the game_data_root *cvar* --
// which this app never sets, because it assigns paths.game_data_root directly
// in OnConfigurePaths. So point this at the same directory explicitly.
REXCVAR_DECLARE(std::string, avatar_asset_pack_dir);
REXCVAR_DECLARE(bool, dash_vsync);

namespace nxe_boot {
// Play the startup animation over the loading dashboard.
void StartBootVideo();
}  // namespace nxe_boot

namespace nxe_profile {
// Host path of the signed-in profile's package directory; empty when signed out.
const std::filesystem::path& ProfileDirectory();
// Name the signed-in profile during startup, before the kernel exists.
void SetStartupProfile(const std::string& xuid_text);
}  // namespace nxe_profile

namespace nxe_guide {
// Loads and registers the recompiled XAM Guide. No-op unless guide_enable.
void Preload();
}

namespace nxe_profile {
void LoadSettingsFromDisk();

// Host path of the staged profile package directory. XamProfileOpen mounts this
// under whatever name the guest asks for -- "DASHUSER" and "SkinRoot" are both
// used. See user_profile.cpp.
const std::filesystem::path& ProfileDirectory();
}

class NxeDashApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    // "NXE" is the application's name: it becomes the window title, the log file
    // prefix, and the name of the config file the SDK reads beside the
    // executable (NXE.toml).
    return std::unique_ptr<NxeDashApp>(new NxeDashApp(ctx, "NXE",
        PPCImageConfig));
  }

  // The dashboard loads its UI from media:\<pkg>.xzp and reads fonts from
  // file://media:/Xenon*Latin.xtt. On a console `media:` is a symbolic link the
  // title creates via NtCreateSymbolicLinkObject -- which this SDK exports only
  // as a bare stub, so the link is never registered and every lookup died at
  // "VFS: 'media:\' -> [no device]" before a filename was even considered.
  //
  // Register it here instead. OnPostSetup runs after the runtime has mounted
  // game_data_root at \Device\Harddisk0\Partition1, so pointing media: at that
  // same device makes the staged .xzp packages resolve. dvdmedia: is the disc
  // variant the same code paths fall back to.
  // Rich presence starts here rather than at construction: it reads its
  // application id from a cvar, and cvars are not settled until setup has run.
  // With no id configured this returns immediately and costs nothing.
  void OnShutdown() override { nxe_discord::Stop(); }

  void OnPostSetup() override {
    nxe_discord::Start();

    // Vertical sync, applied here because this is the first point at which the
    // GPU plugin's cvars exist.
    //
    // rexgpu-xenos defines `vsync` (default on) and is loaded after the config
    // file has already been parsed, so anything that file said about `vsync`
    // was applied to a cvar that did not exist yet and was dropped -- which is
    // why turning it off never survived a restart. dash_vsync lives in this
    // executable, so config and command line reach it normally, and it is
    // copied across here.
    {
      const bool want = REXCVAR_GET(dash_vsync);
      rex::cvar::SetFlagByName("vsync", want ? "true" : "false");
      REXLOG_INFO("GPU: vsync {}", want ? "enabled" : "disabled");
    }

    auto* fs = REX_KERNEL_FS();
    if (!fs) return;
    const char* kPartition = "\\Device\\Harddisk0\\Partition1";
    for (const char* link : {"media:", "dvdmedia:"}) {
      fs->RegisterSymbolicLink(link, kPartition);
    }

    // DASHUSER: was pinned here too, at the game directory. That was wrong: it
    // is the profile mount, and the dashboard mounts it itself at guest
    // 0x92141790 with XamProfileOpen(xuid, "DASHUSER", 0, 0). Pointing it at
    // the game directory sent every profile-local file the dashboard reads or
    // writes -- ThematicSkin and VisionEffect among them -- somewhere they
    // could never be. XamProfileOpen registers it now, and SkinRoot: with it,
    // against the real profile directory.

    MountStorageDevice(fs);
    MountProfileDevice(fs);
    MountDiscDevice(fs);

    // The runtime builds UserProfile with no settings, so every
    // XamUserReadProfileSettings lookup missed and the whole call failed.
    // Populate it from the staged profile's GPD now that storage is up.
    nxe_profile::LoadSettingsFromDisk();

    // Before the title thread starts. See guide_bridge.cpp.
    nxe_guide::Preload();
  }

  // Mount the emulated hard drive.
  //
  // Partition1 is already taken by game_data_root, so the storage volume gets
  // its own device path -- the one XamContentGetDeviceVolumePath reports, and
  // the one the dashboard then opens to probe the volume (guest 0x922B8930).
  // Without a device registered there that probe resolves to "[no device]".
  //
  // This is the volume root, holding Content/, Cache/, $SystemUpdate/ and
  // Compatibility/. The content tree underneath it is what OnConfigurePaths
  // hands to the ContentManager below.
  static void MountStorageDevice(rex::filesystem::VirtualFileSystem* fs) {
    const auto& root = nxe_storage::Root();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
      REXLOG_WARN("Storage device root missing, drive will not mount: {}", root.string());
      return;
    }

    auto device = std::make_unique<rex::filesystem::HostPathDevice>(
        nxe_storage::kHddMountPath, root, /*read_only=*/false);
    if (!device->Initialize() || !fs->RegisterDevice(std::move(device))) {
      REXLOG_WARN("Failed to mount storage device at {}", nxe_storage::kHddMountPath);
      return;
    }
    REXLOG_INFO("Mounted storage device {} at {}", root.string(), nxe_storage::kHddMountPath);
  }

  // Mount the game presented as the disc in the tray.
  //
  // The dashboard reads the disc itself rather than being told about it: with
  // media type 1, guest 0x921FF300 opens \Device\Cdrom0\default.xex, parses
  // the XEX and takes the title information from it, which is what puts the
  // game's own name on the tile. So the whole job here is to put a disc in the
  // drive -- the staged package for disc_title, read-only, at the device path
  // the storage list already advertises for the Disc Drive.
  static void MountDiscDevice(rex::filesystem::VirtualFileSystem* fs) {
    const std::string text = REXCVAR_GET(disc_title);
    if (text.empty()) {
      return;
    }
    const auto title_id = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
    const auto root = nxe_game::PackagePathForTitle(title_id);
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root / "default.xex", ec)) {
      REXLOG_INFO("No disc mounted: nothing staged for title {:08X}", title_id);
      return;
    }

    auto device = std::make_unique<rex::filesystem::HostPathDevice>(
        nxe_storage::kOddMountPath, root, /*read_only=*/true);
    if (!device->Initialize() || !fs->RegisterDevice(std::move(device))) {
      REXLOG_WARN("Failed to mount the disc at {}", nxe_storage::kOddMountPath);
      return;
    }
    REXLOG_INFO("Mounted disc {} at {}", root.string(), nxe_storage::kOddMountPath);
  }

  // Mount the signed-in profile's directory on its own device.
  //
  // XamProfileOpen only links a name to it; the device has to exist first, and
  // it has to be its own device rather than a path inside the storage volume.
  // A symbolic link straight to
  // \Device\Harddisk0\Partition3\Content\<xuid>\FFFE07D1\00010000\<pkg> is
  // registered happily and then resolves into the NullDevice, because that
  // device claims the whole \Device\Harddisk0 prefix -- see kProfileMountPath.
  // Writable: this is where the dashboard puts ThematicSkin when a theme is
  // applied.
  static void MountProfileDevice(rex::filesystem::VirtualFileSystem* fs) {
    const auto& dir = nxe_profile::ProfileDirectory();
    if (dir.empty()) {
      REXLOG_INFO("No profile staged; profile device will not mount");
      return;
    }

    auto device = std::make_unique<rex::filesystem::HostPathDevice>(
        nxe_storage::kProfileMountPath, dir, /*read_only=*/false);
    if (!device->Initialize() || !fs->RegisterDevice(std::move(device))) {
      REXLOG_WARN("Failed to mount profile at {}", nxe_storage::kProfileMountPath);
      return;
    }
    REXLOG_INFO("Mounted profile {} at {}", dir.string(), nxe_storage::kProfileMountPath);
  }

  // Make the executable launchable on its own.
  //
  // Everything below was previously supplied as --game_data_root / --gpu_plugin
  // on the command line, so double-clicking the exe failed with
  // "--game_data_root was not provided." These fill in the same values as
  // DEFAULTS: PathConfig/RuntimeConfig arrive already populated from the CLI and
  // cvars, so an explicit flag or a config-file entry still wins over these.
  // Start the boot animation before anything else is set up, so it covers the
  // whole load. It runs on its own thread and takes its own window down when
  // the video ends -- by which point the dashboard is up behind it.
  void OnPostInitLogging() override { nxe_boot::StartBootVideo(); }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.game_data_root.empty()) {
      // Staged game directory: default.xex, the XUI .xzp packages and the
      // Xenon*Latin.xtt fonts, with media:/dvdmedia: pointed here by
      // OnPostSetup above. Forward slashes avoid backslash-escape mistakes.
      paths.game_data_root = nxe_paths::Resolve("gamedir").string();
    }

    // The ContentManager's root is user_data_root, and when that is left empty
    // the Runtime falls back to game_data_root -- so saves, profiles and
    // installed content were being resolved against the read-only staged game
    // directory, which has no content tree at all.
    //
    // Point it at the storage device instead. This must be the directory that
    // directly contains {XUID:016X} folders, because ContentManager resolves
    // packages as content_root/xuid/title_id/content_type/.
    //
    // Unlike game_data_root this one is never empty here: rex_app resolves it
    // to GetUserFolder()/<app name> when neither --user_data_root nor the cvar
    // is set, and it does that before calling this hook. (The .toml cannot be
    // used either -- LoadConfig runs after path resolution.) So compare against
    // that computed default and treat it as "unset"; an explicit override on
    // the command line still wins.
    const auto default_user_root = rex::filesystem::GetUserFolder() / GetName();
    if (paths.user_data_root.empty() || paths.user_data_root == default_user_root) {
      paths.user_data_root = nxe_storage::ContentRoot();
    }

    // Fullscreen, which is the SDK's own default (rexglue-sdk/src/ui/window.cpp)
    // and what a console does. This used to be forced to false here to keep the
    // dashboard in a window during development; --fullscreen=false on the
    // command line still gives you that when you want it.
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Without a GPU plugin the runtime comes up in "native rendering mode" and
    // every Vd* ring-buffer call is ignored, so nothing is ever drawn.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }

    // Tell the avatar pipeline where the asset packs are.
    //
    // It resolves them from avatar_asset_pack_dir, or failing that the
    // game_data_root *cvar*. This app sets paths.game_data_root directly rather
    // than through that cvar, so the fallback finds nothing and the pipeline
    // reports "neither avatar_asset_pack_dir nor game_data_root is set" and
    // refuses to build an avatar. Naming the directory here is the fix.
    if (REXCVAR_GET(avatar_asset_pack_dir).empty()) {
      REXCVAR_SET(avatar_asset_pack_dir, nxe_paths::Resolve("gamedir").string());
    }

    // A profile named on the command line is signed in at startup; without one
    // the console boots signed out, like a console does.
    if (const std::string wanted = rex::cvar::GetFlagByName("profile_xuid"); !wanted.empty()) {
      nxe_profile::SetStartupProfile(wanted);
    }

    // Point the dashboard at the same closet the Avatar Editor uses.
    //
    // Imported marketplace items live there, not in AvatarAssetPack.toc, and
    // the runtime resolves them through avatar_closet_dir (defaulting to a
    // closet/ beside the asset pack, which here is empty). Without this an
    // avatar wearing an imported item renders in the editor and comes up
    // missing those pieces on the blade -- the editor would find them and the
    // dashboard would not.
    if (rex::cvar::GetFlagByName("avatar_closet_dir").empty()) {
      const std::string closet = nxe_paths::Resolve("assets/closet").string();
      std::error_code ec;
      if (std::filesystem::is_directory(closet, ec)) {
        rex::cvar::SetFlagByName("avatar_closet_dir", closet);
        REXLOG_INFO("Avatar: closet at {}", closet);
      }
    }

    // Keep each profile's saved avatar to itself.
    //
    // AvatarManifestPath() falls back to <user_data_root>/avatars, which is one
    // file for the whole machine. With a single profile staged that was
    // indistinguishable from correct; with two it means whoever saves last wins,
    // and a profile with no avatar of its own -- a freshly made account has no
    // 0x63E80044 setting -- silently inherits the other one's.
    //
    // Keyed on the staged profile's directory name, which is its *offline*
    // XUID. Deliberately not the runtime's UserProfile::xuid(), which is a
    // fixed value that does not change with the profile staged (see the note in
    // user_profile.cpp) and so would give both profiles the same folder.
    //
    // Set by name: avatar_data_dir is defined inside the runtime DLL, so its
    // storage symbol is not linkable from here the way a cvar defined in this
    // binary would be.
    if (rex::cvar::GetFlagByName("avatar_data_dir").empty()) {
      const auto& profile = nxe_profile::ProfileDirectory();
      if (!profile.empty()) {
        const auto dir = nxe_storage::ContentRoot() / "avatars" / profile.filename();
        rex::cvar::SetFlagByName("avatar_data_dir", dir.string());
        REXLOG_INFO("Avatar: this profile's avatar lives in {}", dir.string());
      } else {
        REXLOG_WARN("Avatar: no profile staged at setup; avatar_data_dir left unset, so the "
                    "shared folder is used and every profile shares one avatar");
      }
    }
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
