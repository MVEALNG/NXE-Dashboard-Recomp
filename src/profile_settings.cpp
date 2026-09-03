// Loading real profile settings out of the profile's GPD.
//
// xeXamUserReadProfileSettingsEx was erroring 14 times a run with
// "requested unimplemented setting 63E80044" -- the single most frequent error
// in this port. The runtime's implementation is not at fault; it is complete and
// correct, and simply has nothing to serve:
//
//     auto setting = user_profile->GetSetting(setting_id);
//     if (setting && setting->is_set) setting->Append(&out_setting->data, &out_stream);
//
// UserProfile is constructed with no settings at all, so every lookup misses and
// the whole call fails with X_ERROR_INVALID_PARAMETER. The fix is therefore not
// to reimplement the export -- which would mean redoing its buffer sizing and
// serialisation -- but to give UserProfile the settings it was always meant to
// have. AddSetting is public, and the typed Setting subclasses already know how
// to serialise themselves into the guest's buffer.
//
// Where they come from
// --------------------
// The profile staged for this port (a CON-signed STFS profile, content type
// 0x00010000, title FFFE07D1) carries a 261,441-byte FFFE07D1.gpd holding 81
// XDBF entries -- 35 settings, including 0x63E80044 as a 1000-byte binary blob,
// and 39 played-title records that src/played_titles.cpp reads. It is unpacked
// onto the storage device by tools/extract_stfs.py, under the layout the
// ContentManager already uses:
//
//     Content/<XUID>/FFFE07D1/00010000/<XUID>/FFFE07D1.gpd
//
// A note on 0x63E80044 specifically. Earlier in this port it was measured that
// answering that setting made the dashboard die within a second, while failing it
// let the dashboard run for minutes, and failing was adopted as correct. That
// held for the evidence available then, because the only possible answer was a
// fabricated empty blob. What was fatal was handing the dashboard a meaningless
// value, not answering at all. This serves the real 1000 bytes from the profile,
// which is a different thing entirely -- but it is the one setting to suspect
// first if the dashboard starts misbehaving after this change.
//
// XDBF layout, confirmed by decoding the file rather than assumed:
//
//     header   u32 magic 'XDBF', u32 version, u32 entry_table_len,
//              u32 entry_count, u32 free_table_len, u32 free_count      (24 bytes)
//     entry    u16 namespace, u64 id, u32 offset, u32 length            (18 bytes)
//     data     starts at 24 + entry_table_len*18 + free_table_len*8
//
//     setting  +0x00 u32 setting_id
//              +0x08 u8  type
//              +0x10 scalar value, or u32 payload length for string/binary
//              +0x18 payload
//
// Everything is big-endian, as the guest expects.

#include <cstdint>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

#include "installed_titles.h"
#include "profile_list.h"
#include "storage_device.h"

// Defined in main.cpp -- a cvar defined in a DLL registers after config
// parsing, so it has to live in the executable.
REXCVAR_DECLARE(bool, avatar_manifest);
REXCVAR_DECLARE(std::string, profile_xuid);
REXCVAR_DECLARE(bool, boot_signed_out);
REXCVAR_DECLARE(int32_t, profile_zone);
REXCVAR_DECLARE(double, profile_rep);
REXCVAR_DECLARE(int32_t, profile_gamerscore);
REXCVAR_DECLARE(std::string, profile_switch_notify_ids);

namespace nxe_profile {

namespace {

using rex::system::xam::UserProfile;

constexpr uint16_t kNamespaceSetting = 3;

// The avatar manifest, and the one setting deliberately NOT served.
//
// Its payload is 1000 bytes, exactly the size of a .Avatar manifest (the
// runtime's own avatar_manifest_path cvar loads a file of precisely that size),
// so answering it hands the dashboard a real avatar and it goes off to render
// one. That path is not viable here, and the reason is now traced rather than
// guessed at.
//
// What the guest does, from IDA on $flash_dash.xex:
//
//   sub_92471338   allocates 976 bytes and constructs the avatar object; keeps
//                  it only if its status reads E_PENDING (0x8000000A)
//   sub_92470938   the constructor -- asks XamAvatarGetAssetsResultSize for two
//                  buffer sizes, allocates buffer_a/buffer_b, calls
//                  XamAvatarGetAssets, and ends the success path with
//                      *(obj + 112) = 0x8000000A
//   sub_92471428   the per-frame tick; while the status is E_PENDING it calls
//   sub_92471038   which polls obj+52 (the OVERLAPPED given to GetAssets) and
//                  then walks what GetAssets should have written:
//                      v9 = **(uint32 ***)(obj + 28);
//
// An earlier version of this comment blamed mip-map generation at 0x92471038.
// That is wrong twice: 0x92471038 is the tick's continuation, not mip-mapping
// (XamAvatarGenerateMipMaps is only reached from it at 0x92471320, and only
// when obj+40 is non-zero), and the "null pointer chain" is that double
// dereference of buffer_a -- which the SDK's XamAvatarGetAssets never filled
// while claiming success. It now reports failure instead, which the guest
// handles cleanly (status E_FAIL, object destroyed, flagged at obj+1040 by
// sub_92483388).
//
// That alone does not make the setting safe to serve: measured, the dashboard
// still stalls with the manifest answered even when the asset load fails
// cleanly -- ~453 log lines against ~27,700, with the GPU thread never given
// work. So the wait is further downstream than the asset call.
//
// What would actually fix it. The dashboard's xam.xex is byte-identical to the
// one the avatar editor recompiles (md5 0d26260f...), and NXE already builds it
// as nxe_dash_xam, so retail xam's real avatar implementation is present at the
// same addresses. Routing XamAvatar* there is what the avatar editor does
// (src/hooks/xam_avatar_bridge.cpp). It is not a small wire-up: retail xam
// needs ~24 of its startup stages run by hand (CRT startup, pool allocator,
// string table, notify queues, timer state, task pools, content table, service
// object, service registration, eleven subsystem registrations), its
// unresolved variable imports repaired, and the content manager taught to
// resolve "AvatarAssetPack.toc" -- because the SDK does not populate the
// kernel state xam expects and its DllMain faults.
//
// Worth knowing before starting: NXE is further along one axis than the editor
// is. The editor gets the avatar service to 0x0 but then, per its
// docs/avatar-xam-status.md, "the title asks for nothing" -- GetAssetsResultSize
// and GetAssets are never called. The dashboard does call them. So the piece
// the editor is stuck on is the piece NXE already has.
//
// This is also the honest explanation of something measured much earlier in this
// port, when answering 0x63E80044 with a fabricated blob killed the dashboard in
// about a second while failing it let the dashboard run for minutes. The
// conclusion then was that failing the setting was correct. It was, but not for
// the reason assumed: the value was never the problem, the avatar renderer is.
// Supplying the genuine 1000 bytes from the profile crashes it just the same.
//
// So the other settings are served and this one is withheld until the avatar
// path works. Nothing here is faked -- a setting that cannot be honoured is
// simply left unanswered, which is what the runtime did before.
constexpr uint32_t kAvatarManifestSetting = 0x63E80044;
constexpr size_t kAvatarManifestSize = 1000;
constexpr size_t kAvatarOwnerXuidOffset = 0x380;

// XPROFILE_GAMERCARD_TITLES_PLAYED.
//
// This one number is what decides whether the Game Library can show anything at
// all, and the profile on disk stores it as 0.
//
// The dashboard's gamercard loader (guest 0x921FCD50) reads six settings, and
// keeps this one as the played-title count:
//
//     nData = v7->pSettings[3].data.nData;   // 0x10040004
//     *(a3 + 64) = nData;
//
// That count then gates the loader outright, at guest 0x921FD370:
//
//     SignalState = <the count>;
//     if ( SignalState ) { ... XamUserCreateTitlesPlayedEnumerator ... }
//
// With a zero count the enumerator is never created, so
// XamUserCreateTitlesPlayedEnumerator is never called -- which is exactly what
// the log showed while the library sat empty. No amount of installed content can
// change that, because nothing ever asks for it. The game list is not filtered
// down to nothing; it is never requested.
//
// So the count is reported as the number of titles actually installed on the
// storage device. That is a measured value, not a fabricated one -- it is the
// count of titles the library would have to show. The stored 0 is honoured when
// nothing is installed, and the profile's own figure is kept whenever it is
// already the larger of the two, so a profile with real history is never
// diminished.
constexpr uint32_t kTitlesPlayedSetting = 0x10040004;

// The rest of the gamercard family. Same namespace as titles-played above.
// The ids the gamercard actually asks for.
//
// Gamerscore was written to 0x10040013 and the zone to 0x10040096, and the card
// showed 0 and None because it never asks for either. Logging every id it
// requests settled it -- what it reads is
//
//     0x10040006  int32    gamerscore
//     0x402c0011  string   motto
//     0x10040012  int32    titles played
//     0x10040004  int32
//     0x63e80044  binary   avatar
//     0x5004000b  float    reputation
//
// and the type is carried in the id's top nibble (1 int32, 4 string, 5 float,
// 6 binary), which agrees: 0x10040006 is the int32 the Gamerscore row wants.
//
// The old gamerscore id is still written. It costs one setting, and a profile
// or title that does read it keeps working.
constexpr uint32_t kGamerscoreSetting = 0x10040006;      // int32, what the card reads
constexpr uint32_t kGamerscoreSettingAlt = 0x10040013;   // int32, the previous guess
constexpr uint32_t kReputationSetting = 0x5004000B;      // float, 0..5
constexpr uint32_t kZoneSetting = 0x10040096;            // int32, 1..4

// The count the Game Library actually sizes itself from.
//
// Not 0x10040004. The gamercard reader at guest 0x921FCD50 asks for six
// settings -- 10040006, 402C0011, 10040012, 10040004, 63E80044, 5004000B --
// and lays them into a 1152-byte block:
//
//     *(block + 124) = pSettings[2].data.nData;   // 0x10040012
//     *(block +  64) = pSettings[3].data.nData;   // 0x10040004
//
// Its caller at 0x921FD590 copies that block into the owning object at +16472,
// so block+124 lands at +0x40D4 -- and +0x40D4 is exactly what the list loader
// at 0x921FD370 reads as its count:
//
//     lwz  r29, 0x40D4(r31)        ; the count
//     ...  free the existing list ...
//     cmplwi r29, 0 ; beq -> done  ; nothing is rebuilt when it is zero
//     mulli r3, r29, 0xA8          ; else allocate 168 * count and enumerate
//
// That free happens before the test, so a count of zero frees the games and puts
// nothing back: they appear once and vanish on the next refresh. 0x40D4 has
// exactly one other writer in the whole image -- 0x921FD370 storing back however
// many records the enumerator returned -- so this setting is the only thing that
// can ever start the list.
//
// This profile's GPD carries a single setting, 0x10040004, and nothing for
// 0x10040012, so it was served as an unset record and read back as zero.
constexpr uint32_t kLibraryTitleCountSetting = 0x10040012;

// UserProfile::Setting::Type
enum class GpdType : uint8_t {
  kContent = 0,
  kInt32 = 1,
  kInt64 = 2,
  kDouble = 3,
  kWString = 4,
  kFloat = 5,
  kBinary = 6,
  kDateTime = 7,
};

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t Be64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | p[i];
  }
  return v;
}

uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  FILE* f = nullptr;
  if (fopen_s(&f, path.string().c_str(), "rb") != 0 || f == nullptr) {
    return data;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    data.resize(static_cast<size_t>(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
      data.clear();
    }
  }
  std::fclose(f);
  return data;
}

// Content/<XUID>/FFFE07D1/00010000/<name>/FFFE07D1.gpd -- whichever profile is
// staged, rather than a hardcoded XUID.
// The signed-in profile's offline XUID, empty when signed out. Everything
// derived from a profile is rebuilt when this changes, via the generation bump
// in SwitchTo/SignOut.
std::string g_active_xuid;

// Offline XUIDs are written as hex and compared as text, so case must not
// decide which profile loads.
bool EqualsNoCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::filesystem::path FindDashboardGpd() {
  std::error_code ec;
  const auto root = nxe_storage::ContentRoot();
  if (!std::filesystem::exists(root, ec)) {
    return {};
  }
  // Which profile is signed in.
  //
  // Empty until something signs one in -- a console boots with no active user,
  // and pretending otherwise is what had the dashboard endlessly re-picking a
  // profile it did not recognise. g_active_xuid is set by SwitchTo.
  std::string wanted = g_active_xuid;
  if (wanted.empty()) {
    if (REXCVAR_GET(boot_signed_out)) {
      return {};  // nobody signed in, and nobody assumed
    }
    // Otherwise a profile is always signed in: the one named on the command
    // line, or failing that whichever is staged.
    wanted = REXCVAR_GET(profile_xuid);
  }

  std::filesystem::path first;
  for (const auto& xuid_dir : std::filesystem::directory_iterator(root, ec)) {
    if (!xuid_dir.is_directory(ec)) continue;
    const auto profiles = xuid_dir.path() / "FFFE07D1" / "00010000";
    if (!std::filesystem::exists(profiles, ec)) continue;
    for (const auto& pkg : std::filesystem::directory_iterator(profiles, ec)) {
      if (!pkg.is_directory(ec)) continue;
      const auto gpd = pkg.path() / "FFFE07D1.gpd";
      if (!std::filesystem::exists(gpd, ec)) continue;

      if (!wanted.empty()) {
        // Match on either level: the outer directory and the package inside it
        // are both named for the offline XUID, but only the outer one is
        // guaranteed to be, so accept a hit on either.
        if (EqualsNoCase(pkg.path().filename().string(), wanted) ||
            EqualsNoCase(xuid_dir.path().filename().string(), wanted)) {
          return gpd;
        }
        continue;
      }
      if (first.empty()) {
        first = gpd;
      }
    }
  }
  if (!wanted.empty() && first.empty()) {
    REXLOG_WARN("Profile: no staged profile matches '{}'", wanted);
  }
  return first;  // fall back to whatever is staged
}

}  // namespace

// The directory the staged profile package lives in -- the same place a console
// keeps ThematicSkin, VisionEffect and the Account blob, alongside the GPDs.
//
// This is what XamProfileOpen mounts. It is found by scanning rather than built
// from the XUID on purpose: the directory is named for the profile's OFFLINE
// XUID (ECF094C2048FC0CD here) while the runtime's UserProfile reports a
// different one, so deriving the path from the XUID would miss.
// Every profile staged under the content root.
//
// Same shape the GPD search walks, but collecting instead of stopping at the
// first hit: <content root>/<xuid>/FFFE07D1/00010000/<pkg>/FFFE07D1.gpd. The
// offline XUID is the package directory's name.
std::vector<StagedProfile> StagedProfiles() {
  std::vector<StagedProfile> found;
  std::error_code ec;
  const auto root = nxe_storage::ContentRoot();
  if (!std::filesystem::exists(root, ec)) {
    return found;
  }
  for (const auto& xuid_dir : std::filesystem::directory_iterator(root, ec)) {
    if (!xuid_dir.is_directory(ec)) continue;
    const auto profiles = xuid_dir.path() / "FFFE07D1" / "00010000";
    if (!std::filesystem::exists(profiles, ec)) continue;
    for (const auto& pkg : std::filesystem::directory_iterator(profiles, ec)) {
      if (!pkg.is_directory(ec)) continue;
      if (!std::filesystem::exists(pkg.path() / "FFFE07D1.gpd", ec)) continue;

      StagedProfile entry;
      entry.directory = pkg.path();
      entry.xuid_text = pkg.path().filename().string();
      entry.xuid = std::strtoull(entry.xuid_text.c_str(), nullptr, 16);
      entry.gamertag = GamerTagOf(pkg.path());
      entry.account_info = AccountInfoOf(pkg.path());
      entry.online_xuid = OnlineXuidOf(pkg.path());
      if (entry.gamertag.empty()) {
        entry.gamertag = entry.xuid_text;  // nameless, but still selectable
      }
      found.push_back(std::move(entry));
    }
  }
  // Stable order so the list does not reshuffle between polls -- the sign-in
  // scene rebuilds the enumerator every redraw.
  std::sort(found.begin(), found.end(),
            [](const StagedProfile& a, const StagedProfile& b) {
              return a.xuid_text < b.xuid_text;
            });
  return found;
}

std::atomic<uint32_t> g_generation{1};

uint32_t Generation() { return g_generation.load(std::memory_order_acquire); }

// Signed out only when that is actually being modelled; otherwise the console
// always has a user, which is what every identity call answers for.
bool SignedIn() {
  if (!REXCVAR_GET(boot_signed_out)) {
    return !ProfileDirectory().empty();
  }
  return !g_active_xuid.empty();
}

// Name the signed-in profile before the kernel exists.
//
// SwitchTo cannot be used this early: it reloads the profile's settings into the
// runtime's UserProfile, and during OnPreSetup there is no kernel to hold one --
// calling it there segfaults the boot. Settings are loaded later anyway, by
// NxeDashApp once the kernel is up, so all that is needed here is to say who is
// signed in.
void SetStartupProfile(const std::string& xuid_text) {
  g_active_xuid = xuid_text;
  g_generation.fetch_add(1, std::memory_order_acq_rel);
}

std::atomic<bool> g_pad_input{false};

void NotePadInput() { g_pad_input.store(true, std::memory_order_release); }

bool PadInputSeen() { return g_pad_input.load(std::memory_order_acquire); }

std::atomic<bool> g_signin_scene_open{false};

std::mutex g_offer_mutex;
std::string g_offered_xuid;

void NoteProfileOffered(const std::string& xuid_text) {
  std::lock_guard<std::mutex> lock(g_offer_mutex);
  g_offered_xuid = xuid_text;
}

// Leaving the chooser commits whatever it was last pointing at.
//
// Signing in on each offer instead meant signing in many times a second, and a
// sign-in-changed broadcast with each -- the dashboard rebuilt its blade every
// time and the category list vanished.
void NoteSceneOpened(const std::string& container) {
  const bool signin = container.find("Signin") != std::string::npos;
  const bool was_open = g_signin_scene_open.exchange(signin);

  if (was_open && !signin) {
    std::string wanted;
    {
      std::lock_guard<std::mutex> lock(g_offer_mutex);
      wanted.swap(g_offered_xuid);
    }
    if (!wanted.empty() && wanted != g_active_xuid) {
      REXLOG_WARN("Sign-in: leaving the chooser on '{}'; signing in", wanted);
      SwitchTo(wanted);
    }
  }
}

bool SigninSceneOpen() { return g_signin_scene_open.load(std::memory_order_acquire); }

const std::filesystem::path& ProfileDirectory() {
  return ProfileScoped([] {
    const auto gpd = FindDashboardGpd();
    return gpd.empty() ? std::filesystem::path{} : gpd.parent_path();
  });
}

// Where this profile's avatar manifest lives (set in NxeDashApp::OnPreSetup).
std::filesystem::path AvatarManifestFile() {
  const std::string dir = rex::cvar::GetFlagByName("avatar_data_dir");
  if (dir.empty()) {
    return {};
  }
  return std::filesystem::path(dir) / "avatar_manifest.bin";
}

// A complete stock avatar to start a new account from.
//
// The *.Avatar files beside the asset packs are title preset manifests: whole
// valid avatars, which is what makes them a better starting point than a
// procedurally assembled one. Any of them will do; take the first.
std::vector<uint8_t> ReadAnyPreset() {
  const std::string pack_dir = rex::cvar::GetFlagByName("avatar_asset_pack_dir");
  if (pack_dir.empty()) {
    return {};
  }
  std::error_code ec;
  std::vector<std::filesystem::path> presets;
  for (const auto& entry : std::filesystem::directory_iterator(pack_dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".Avatar") {
      presets.push_back(entry.path());
    }
  }
  if (presets.empty()) {
    return {};
  }
  std::sort(presets.begin(), presets.end());
  return ReadFile(presets.front());
}

// Put a manifest into the profile setting the dashboard renders from, replacing
// whatever is there. Replace rather than add: this runs again on every avatar
// save, and a second AddSetting would leave the old value behind the new one.
void StoreAvatarSetting(UserProfile* profile, const std::vector<uint8_t>& manifest) {
  profile->AddSetting(
      std::make_unique<UserProfile::BinarySetting>(kAvatarManifestSetting, manifest));
}

// Hand a profile that has no avatar one of its own.
void SeedAvatarManifest(UserProfile* profile) {
  const auto path = AvatarManifestFile();
  if (path.empty()) {
    return;
  }

  // Already seeded on an earlier boot, or saved from the Avatar Editor.
  auto manifest = ReadFile(path);

  // Make sure it belongs to this profile.
  //
  // A manifest carries its owner's XUID, and everything downstream matches on
  // it -- GetManifestsByXuid serves the player's own avatar only when the owner
  // agrees, and the editor-save adoption in GetAssets refuses a manifest owned
  // by somebody else. A file sitting in this profile's folder is this profile's
  // avatar whatever it says inside, so correct it rather than let the mismatch
  // propagate.
  //
  // This is not hypothetical: the Avatar Editor used to be pointed at a shared
  // folder, so it would load one profile's avatar and, once it was pointed at
  // per-profile folders, save that avatar into a different profile's. The
  // result was MVEALNG's folder holding a manifest owned by REALmjoct.
  if (manifest.size() == kAvatarManifestSize) {
    const uint64_t xuid =
        std::strtoull(ProfileDirectory().filename().string().c_str(), nullptr, 16);
    uint64_t owner = 0;
    for (int i = 0; i < 8; ++i) {
      owner = (owner << 8) | manifest[kAvatarOwnerXuidOffset + i];
    }
    if (xuid && owner != xuid) {
      REXLOG_WARN("Profile: the avatar in this profile's folder is stamped {:016X}, not "
                  "{:016X}; re-stamping it as this profile's",
                  owner, xuid);
      for (int i = 0; i < 8; ++i) {
        manifest[kAvatarOwnerXuidOffset + i] = static_cast<uint8_t>(xuid >> (56 - i * 8));
      }
      std::FILE* f = std::fopen(path.string().c_str(), "wb");
      if (f) {
        std::fwrite(manifest.data(), 1, manifest.size(), f);
        std::fclose(f);
      }
    }
  }

  if (manifest.size() != kAvatarManifestSize) {
    manifest = ReadAnyPreset();
    if (manifest.size() != kAvatarManifestSize) {
      REXLOG_WARN("Profile: no avatar manifest and no preset to seed one from; the avatar will "
                  "not render. Put a *.Avatar preset beside the asset packs.");
      return;
    }
    // Stamp it as this profile's. The offline XUID is the name of the staged
    // profile's directory; the owner is what every later lookup matches on.
    const uint64_t xuid = std::strtoull(ProfileDirectory().filename().string().c_str(), nullptr, 16);
    for (int i = 0; i < 8; ++i) {
      manifest[kAvatarOwnerXuidOffset + i] = static_cast<uint8_t>(xuid >> (56 - i * 8));
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f) {
      std::fwrite(manifest.data(), 1, manifest.size(), f);
      std::fclose(f);
    }
    REXLOG_INFO("Profile: no avatar in this profile; seeded one from a preset for XUID {:016X}",
                xuid);
  }

  StoreAvatarSetting(profile, manifest);
}

// Re-read the signed-in profile's avatar and put it back into the setting the
// blade renders from.
//
// Called when the manifest file changes -- the Avatar Editor saving, mainly.
// Without this the setting keeps whatever was read at startup, so an edit shows
// up wherever the avatar is rebuilt from the manifest (the profile blade) and
// nowhere that renders from the setting (My Xbox).
void RefreshAvatarSetting() {
  auto* profile = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->user_profile() : nullptr;
  if (!profile) {
    return;
  }
  const auto path = AvatarManifestFile();
  if (path.empty()) {
    return;
  }
  auto manifest = ReadFile(path);
  if (manifest.size() != kAvatarManifestSize) {
    return;
  }
  REXLOG_WARN("Profile: avatar changed on disk; refreshing setting {:#010x} so the blade "
              "picks it up",
              kAvatarManifestSetting);
  StoreAvatarSetting(profile, manifest);
}

// Defined below, next to the settings it publishes.
void ApplyGamercardValues(UserProfile* profile);

void RepublishDerivedSettings() {
  auto* profile = REX_KERNEL_STATE()->user_profile();
  if (profile) {
    ApplyGamercardValues(profile);
  }
}

void InvalidateCaches() {
  g_generation.fetch_add(1, std::memory_order_acq_rel);
}

void SignOut() {
  if (g_active_xuid.empty()) {
    return;
  }
  REXLOG_WARN("Profile: signing out '{}'", g_active_xuid);
  g_active_xuid.clear();
  rex::cvar::SetFlagByName("profile_xuid", "");
  g_generation.fetch_add(1, std::memory_order_acq_rel);
}

// Replace by adding.
//
// AddSetting already replaces: it swaps the new setting into settings_[id] and
// swaps the old one out of the owning list. The Deserialize(Serialize()) pair
// that used to stand in for it here is a no-op for everything except a binary
// setting -- only BinarySetting overrides them, and the base class serializes to
// an empty vector and deserializes by ignoring its argument. So every int, float
// and datetime written through that pair was silently dropped and the value the
// UserProfile constructor pre-seeded stayed in place.
//
// That is what emptied the Game Library. The profile's played-title count
// (0x10040004) is pre-seeded to 0, so the parsed count never landed and the
// gamercard read at guest 0x921FCD50 served 0. The loader at 0x921FD370 frees
// the existing list at the top of every refresh and only rebuilds it
// "if (SignalState)", so a count of 0 freed the games and put nothing back --
// they appeared and then vanished a moment later.
//
// It is also why the gamercard's reputation, gamerscore and zone never showed:
// same pattern, same pre-seeded ids, same silent drop.
// Gamercard values that the profile itself does not carry.
//
// Replace rather than add, so a profile that does have one of these keeps a
// single value rather than the override sitting behind the original.
void ApplyGamercardValues(UserProfile* profile) {
  if (!profile) {
    return;
  }

  const auto set_int = [&](uint32_t id, int32_t value) {
    profile->AddSetting(std::make_unique<UserProfile::Int32Setting>(id, value));
  };

  // The played-title list is sized from this one; see the note on the constant.
  // It has to match what the enumerator will actually hand back, which is the
  // same list the Game Library builds -- installed titles plus history.
  const auto library_titles = static_cast<int32_t>(nxe_content::InstalledTitleCount());
  set_int(kLibraryTitleCountSetting, library_titles);
  REXLOG_INFO("Profile: game library sized for {} title(s) (setting {:#010x})", library_titles,
              kLibraryTitleCountSetting);

  if (const int32_t score = REXCVAR_GET(profile_gamerscore); score >= 0) {
    set_int(kGamerscoreSetting, score);
    set_int(kGamerscoreSettingAlt, score);
  }
  if (const int32_t zone = REXCVAR_GET(profile_zone); zone >= 0) {
    set_int(kZoneSetting, zone);
  }
  if (const double rep = REXCVAR_GET(profile_rep); rep >= 0.0) {
    profile->AddSetting(std::make_unique<UserProfile::FloatSetting>(kReputationSetting,
                                                                    static_cast<float>(rep)));
  }

  REXLOG_INFO("Profile: gamercard shows gamerscore {}, rep {:.1f}, zone {}",
              REXCVAR_GET(profile_gamerscore), REXCVAR_GET(profile_rep),
              REXCVAR_GET(profile_zone));
}

void LoadSettingsFromDisk();  // defined below; SwitchTo re-runs it

// Sign in another staged profile, in place.
//
// The order matters. profile_xuid decides what ProfileDirectory resolves to, so
// it is set first; bumping the generation then invalidates every profile-scoped
// cache at once, so the gamertag, Account blob, online XUID and played-titles
// list all rebuild against the new directory the next time they are asked.
// Only then is there any point re-reading the settings or telling the guest.
bool SwitchTo(const std::string& xuid_text) {
  bool known = false;
  std::string gamertag;
  for (const auto& entry : StagedProfiles()) {
    if (EqualsNoCase(entry.xuid_text, xuid_text)) {
      known = true;
      gamertag = entry.gamertag;
      break;
    }
  }
  if (!known) {
    REXLOG_WARN("Profile switch: no staged profile with XUID '{}'", xuid_text);
    return false;
  }

  g_active_xuid = xuid_text;
  rex::cvar::SetFlagByName("profile_xuid", xuid_text);
  g_generation.fetch_add(1, std::memory_order_acq_rel);

  // The avatar lives in a per-profile folder, and the runtime resolves it from
  // this cvar. Repointing it is also what makes the avatar switch: GetAssets
  // notices the manifest file it now resolves to has a different write time
  // from the one it last adopted and takes the new profile's avatar.
  const auto& dir = ProfileDirectory();
  if (!dir.empty()) {
    const auto avatars = nxe_storage::ContentRoot() / "avatars" / dir.filename();
    rex::cvar::SetFlagByName("avatar_data_dir", avatars.string());
  }

  LoadSettingsFromDisk();

  // Tell the guest its user changed, so it re-reads the profile and redraws.
  //
  // This lives here rather than at the call site because there are two ways in
  // -- the sign-in committed on leaving the chooser, and SwitchTo called
  // directly -- and when it sat in only one of them a sign-in would take
  // underneath a dashboard that never re-read, leaving the old gamertag and an
  // empty gamercard on screen.
  const std::string spec = REXCVAR_GET(profile_switch_notify_ids);
  size_t at = 0;
  while (at < spec.size()) {
    size_t end = spec.find(',', at);
    if (end == std::string::npos) {
      end = spec.size();
    }
    const std::string one = spec.substr(at, end - at);
    if (!one.empty()) {
      try {
        const auto id = static_cast<uint32_t>(std::stoul(one, nullptr, 0));
        // Data is a bitmask of which user slots changed, not a spare word.
        // Broadcasting 0 says "nobody changed", which is why raising this
        // rebuilt the blade without ever showing a signed-in user. The
        // runtime's own startup notifications use 1 for user 0
        // (KernelState::RegisterNotifyListener).
        constexpr uint32_t kUserZeroMask = 1;
        REXLOG_INFO("Profile switch: notifying the dashboard, id {:#x} data {:#x}", id,
                    kUserZeroMask);
        REX_KERNEL_STATE()->BroadcastNotification(id, kUserZeroMask);
      } catch (const std::exception&) {
        REXLOG_WARN("Profile switch: unparsable notification id '{}'", one);
      }
    }
    at = end + 1;
  }

  REXLOG_WARN("Profile switch: now signed in as '{}' ({})", gamertag, xuid_text);
  return true;
}

void LoadSettingsFromDisk() {
  const auto gpd_path = FindDashboardGpd();
  if (gpd_path.empty()) {
    REXLOG_INFO("Profile: no dashboard GPD staged; settings will be unavailable");
    return;
  }

  const auto data = ReadFile(gpd_path);
  if (data.size() < 24 || std::memcmp(data.data(), "XDBF", 4) != 0) {
    REXLOG_WARN("Profile: {} is not an XDBF file", gpd_path.string());
    return;
  }

  const uint32_t entry_table_len = Be32(&data[8]);
  const uint32_t entry_count = Be32(&data[12]);
  const uint32_t free_table_len = Be32(&data[16]);
  const size_t base = 24 + size_t(entry_table_len) * 18 + size_t(free_table_len) * 8;

  bool have_avatar = false;
  auto* profile = REX_KERNEL_STATE()->user_profile();
  if (profile == nullptr) {
    return;
  }

  uint32_t loaded = 0;
  size_t off = 24;
  for (uint32_t i = 0; i < entry_count && off + 18 <= data.size(); ++i, off += 18) {
    const uint16_t ns = Be16(&data[off]);
    const uint32_t entry_off = Be32(&data[off + 10]);
    const uint32_t entry_len = Be32(&data[off + 14]);
    if (ns != kNamespaceSetting) {
      continue;
    }
    if (base + entry_off + entry_len > data.size() || entry_len < 0x18) {
      continue;
    }

    const uint8_t* e = &data[base + entry_off];
    const uint32_t setting_id = Be32(e);
    const auto type = static_cast<GpdType>(e[8]);
    if (setting_id == 0) {
      continue;  // padding / malformed records carry a zero id
    }
    if (setting_id == kAvatarManifestSetting && !REXCVAR_GET(avatar_manifest)) {
      // Held back by default -- see the note above kAvatarManifestSetting.
      // The cvar exists so the renderer fault can be retested against the real
      // manifest now staged, without the default build depending on it.
      continue;
    }

    std::unique_ptr<UserProfile::Setting> setting;
    switch (type) {
      case GpdType::kInt32: {
        auto value = static_cast<int32_t>(Be32(e + 0x10));
        if (setting_id == kTitlesPlayedSetting) {
          const auto installed = static_cast<int32_t>(nxe_content::InstalledTitleCount());
          if (installed > value) {
            REXLOG_INFO("Profile: titles played {} -> {} (installed on the storage device)", value,
                        installed);
            value = installed;
          }
        }
        setting = std::make_unique<UserProfile::Int32Setting>(setting_id, value);
        break;
      }
      case GpdType::kInt64:
        setting = std::make_unique<UserProfile::Int64Setting>(
            setting_id, static_cast<int64_t>(Be64(e + 0x10)));
        break;
      case GpdType::kDateTime:
        setting = std::make_unique<UserProfile::DateTimeSetting>(
            setting_id, static_cast<int64_t>(Be64(e + 0x10)));
        break;
      case GpdType::kDouble: {
        const uint64_t bits = Be64(e + 0x10);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        setting = std::make_unique<UserProfile::DoubleSetting>(setting_id, value);
        break;
      }
      case GpdType::kFloat: {
        const uint32_t bits = Be32(e + 0x10);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        setting = std::make_unique<UserProfile::FloatSetting>(setting_id, value);
        break;
      }
      case GpdType::kWString: {
        const uint32_t bytes = Be32(e + 0x10);
        std::u16string value;
        for (uint32_t c = 0; c + 1 < bytes && 0x18 + c + 1 < entry_len; c += 2) {
          const auto ch = static_cast<char16_t>(Be16(e + 0x18 + c));
          if (ch == 0) break;
          value.push_back(ch);
        }
        setting = std::make_unique<UserProfile::UnicodeSetting>(setting_id, value);
        break;
      }
      case GpdType::kBinary: {
        const uint32_t bytes = Be32(e + 0x10);
        const uint32_t avail = entry_len - 0x18;
        const uint32_t count = bytes < avail ? bytes : avail;
        std::vector<uint8_t> value(e + 0x18, e + 0x18 + count);
        setting = std::make_unique<UserProfile::BinarySetting>(setting_id, value);
        break;
      }
      default:
        continue;  // CONTENT and anything unrecognised are left alone
    }

    if (setting_id == kAvatarManifestSetting) {
      have_avatar = true;
    }
    // This runs again on every profile switch; AddSetting replaces in place, so
    // the previous profile's value does not survive behind the new one.
    profile->AddSetting(std::move(setting));
    ++loaded;
  }

  REXLOG_INFO("Profile: loaded {} setting(s) from {}", loaded, gpd_path.string());

  if (!have_avatar && REXCVAR_GET(avatar_manifest)) {
    SeedAvatarManifest(profile);
  }

  ApplyGamercardValues(profile);
}

}  // namespace nxe_profile
