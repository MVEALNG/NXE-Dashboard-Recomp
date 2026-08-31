// Profiles staged under the content root, for the sign-in list.
//
// The dashboard used to have exactly one profile and no way to change it. With
// more than one staged, the Sign In scene needs to name them all, and the
// avatar, settings, game library and gamertag all have to follow whichever is
// picked -- see profile_xuid in main.cpp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nxe_profile {

struct StagedProfile {
  std::filesystem::path directory;  // <content root>/<xuid>/FFFE07D1/00010000/<xuid>
  std::string xuid_text;            // the directory name, as written on disk
  uint64_t xuid = 0;                // parsed from it
  std::string gamertag;             // from the profile's own Account blob
  uint64_t online_xuid = 0;         // its own LIVE identity; 0 = offline account
  std::vector<uint8_t> account_info;  // 380-byte XAMACCOUNTINFO, straight off disk
};

// Every staged profile, ordered by XUID text so the list is stable between the
// sign-in scene's repeated polls.
std::vector<StagedProfile> StagedProfiles();

// The gamertag of one profile directory, decrypted on demand.
std::string GamerTagOf(const std::filesystem::path& profile_dir);

// The profile's own online XUID, 0 if it has never been on LIVE.
uint64_t OnlineXuidOf(const std::filesystem::path& profile_dir);

// The profile's XAMACCOUNTINFO as stored (380 bytes), or empty.
std::vector<uint8_t> AccountInfoOf(const std::filesystem::path& profile_dir);

// The profile currently signed in.
const std::filesystem::path& ProfileDirectory();
const std::string& GamerTag();

// Bumped whenever the signed-in profile changes. Everything derived from a
// profile is cached against this, so one increment invalidates all of it at
// once -- see ProfileScoped below.
uint32_t Generation();

// Sign in a staged profile, in place. Returns false if no staged profile has
// that offline XUID. Also used to change profiles: signing a second one in
// replaces the first.
bool SwitchTo(const std::string& xuid_text);

// Name the signed-in profile during startup, before the kernel exists.
void SetStartupProfile(const std::string& xuid_text);

// Re-read the signed-in profile's saved avatar into the profile setting the
// dashboard blade renders from. Call after the manifest changes on disk.
void RefreshAvatarSetting();

// Is anyone signed in? False from boot until something signs a profile in,
// which is the state a console starts in.
bool SignedIn();

// Sign the current profile out, leaving the console with no active user.
void SignOut();

// A pad button has been pressed at some point. Signing in is only honoured
// after this, because the dashboard also signs profiles in by itself and that
// is otherwise indistinguishable.
void NotePadInput();
bool PadInputSeen();

// Which scene the dashboard just navigated to. Leaving the chooser is what
// commits a pending sign-in.
void NoteSceneOpened(const std::string& container);
bool SigninSceneOpen();

// The profile the chooser is currently pointing at, recorded from the offers
// the dashboard makes while it is open. Committed on leaving.
void NoteProfileOffered(const std::string& xuid_text);

// A value derived from the signed-in profile, rebuilt when the profile changes.
//
// Same shape as the `static const x = [] { ... }()` these replace, and the
// returned reference has the same lifetime rules; the difference is that the
// value is recomputed after a switch instead of being frozen at first use.
// Each call site instantiates its own storage (the lambda type is unique), so
// this must be called from one place per cached value.
template <typename Fn>
const auto& ProfileScoped(Fn compute) {
  using T = decltype(compute());
  static std::mutex mutex;
  static T value{};
  static uint32_t built_for = 0;  // Generation() starts at 1, so 0 = never
  std::lock_guard<std::mutex> lock(mutex);
  const uint32_t now = Generation();
  if (built_for != now) {
    value = compute();
    built_for = now;
  }
  return value;
}

}  // namespace nxe_profile
