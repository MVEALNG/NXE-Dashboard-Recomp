// Profile enumeration.
//
// The My Xbox blade showed no profile because the dashboard could not find one:
// every profile entry point in the runtime is a bare REX_EXPORT_STUB --
// XamProfileCreateEnumerator, XamProfileEnumerate, XamProfileOpen and
// XamProfileClose. The dashboard enumerates at guest 0x922E3BA0:
//
//     XamProfileCreateEnumerator(0, &handle);
//     result = XamProfileEnumerate(handle, 2, &enum_result, 0);
//     if (!result) { ... count this profile ... }
//     else if (result == 5) { wait for notification 11, re-enumerate }
//
// so 0 means "here is a profile", and anything else ends the walk. With the
// stub returning an undefined r3 the count was whatever the register held.
//
// Notification 11 is 0x0B, XN_SYS_STORAGEDEVICESCHANGED -- the same event that
// had to be raised before the storage screen would populate. The dashboard
// re-enumerates profiles when storage appears, which is consistent with
// profiles living on a storage device.
//
// Structures come from the binary's own type information rather than guesswork:
//
//     _PROFILEENUMRESULT (392)      XAMACCOUNTINFO (380)
//       +0x000 xuidOffline            +0x00 dwReserved
//       +0x008 xai ----------------->  +0x04 dwLiveFlags
//       +0x184 DeviceID               +0x08 szGamerTag  WCHAR[16]
//                                     +0x28 xuidOnline
//                                     +0x30 dwCachedUserFlags
//
// What is reported, and why
// -------------------------
// The identity comes from the runtime's own UserProfile rather than being
// invented here, so XamUserGetXUID, XamUserGetName and this enumeration cannot
// disagree with each other. UserProfile keeps xuid_ and name_ private with no
// setters, so aligning it with an on-disk profile would mean overriding the
// whole user API surface -- a wider change than this needs.
//
// xuidOnline is left zero: the guest treats a non-zero value as "this profile
// is an Xbox LIVE account" (0x922E3BA0 counts online and offline profiles
// separately off exactly that field), and there is no LIVE account here.
//
// A note on the staged profile. This used to describe an empty Xenia profile
// whose FFFE07D1.gpd held a single 24-byte setting of zeros, with no way to
// read the gamertag out of the encrypted Account blob. Both halves have since
// changed: src/account_decrypt.cpp recovers the gamertag, and the profile
// staged now is a real one -- ECF094C2048FC0CD, gamertag REALmjoct, 35
// settings and 39 played-title records.
//
// The XUID is still the runtime's rather than that directory's. UserProfile
// keeps xuid_ private with no setter, so aligning them would mean overriding
// the whole user API surface, and nothing so far needs them to agree.

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/filesystem/vfs.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include <atomic>
#include <mutex>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "profile_list.h"
#include "storage_device.h"

namespace nxe_live {
uint64_t OnlineXuid();
}

namespace nxe_profile {
// Recovered from the profile's encrypted Account blob; empty if unavailable.
const std::string& GamerTag();
// Host path of the staged profile package directory.
const std::filesystem::path& ProfileDirectory();
}

using namespace rex;

REXCVAR_DECLARE(bool, profile_switch_restart);
REXCVAR_DECLARE(std::string, profile_xuid);

// Report the staged profile's own XUID as the signed-in identity.
//
// Off, because it is not finished. What it fixes is real: the runtime answers
// XamUserGetXUID with UserProfile::xuid(), the hardcoded 0xB13EBABEBABEBABE,
// while the profile enumerator lists the staged profile's E030000000A8C189. The
// guest is told two different things about one person, and anything resolving
// one against the other decides they are strangers -- which is exactly what the
// gamer blade does. dash_2a57 (guest 0x921F6D58) fetches the signed-in XUID and
// hands it to GamerRootScene; the scene cannot find that XUID among the
// profiles, so it draws the panel a stranger gets: 0 G, Zone None, Offline, and
// Add Friend / Compare Games. Every value it asked for was served correctly --
// gamerscore 1000, rep 4.5 -- and it drew zeros anyway, because it never
// believed the card was yours.
//
// It took two goes to land. The identity has to come from the profile_xuid cvar
// rather than ProfileDirectory(), which would drive profile enumeration and
// Account decryption from inside a kernel export before the guest has opened
// anything; and this must answer X_E_* rather than X_ERROR_*, because
// X_ERROR_NO_SUCH_USER is positive and dash_2a57 tests `>= 0`, so every failure
// read as success and the guest carried on with a zero XUID. XamProfileOpen below
// also had to compare against this identity rather than the placeholder, or the
// start-up open looked like a request for somebody else and never mounted.
//
// On by default: DASHUSER: mounts, start-up is clean, and the avatar system comes
// to life with it -- a run went from one 'Applied blend shape' line to thirty-five,
// because the avatar is stored per profile and the profile can finally be found.
REXCVAR_DEFINE_BOOL(profile_report_staged_xuid, true, "Profile",
                    "Report the staged profile's own XUID from XamUserGetXUID and the profile "
                    "enumerator, instead of the runtime's placeholder. Fixes the gamer blade "
                    "showing a stranger's card.");
REXCVAR_DECLARE(bool, signin_profiles_offline);
REXCVAR_DECLARE(std::string, profile_switch_notify_ids);
REXCVAR_DECLARE(int32_t, profile_switch_grace_seconds);

namespace {

// Defined below; used by both selection paths (XamProfileOpen and
// XamUserLogon), which sit either side of its definition.
// XUIDs are hex text; case must not decide whether a profile is pinned.
inline bool EqualsNoCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

void RestartAsProfile(const std::string& xuid_text);

// _PROFILEENUMRESULT, laid out as the guest declares it.
#pragma pack(push, 1)
struct X_PROFILEENUMRESULT {
  rex::be<uint64_t> xuid_offline;  // +0x000
  uint8_t account_info[380];       // +0x008  XAMACCOUNTINFO
  rex::be<uint32_t> device_id;     // +0x184
};
#pragma pack(pop)
static_assert(sizeof(X_PROFILEENUMRESULT) == 392,
              "_PROFILEENUMRESULT must match the guest's 392-byte record");

// Offsets within XAMACCOUNTINFO.
constexpr size_t kAccountLiveFlags = 0x04;
constexpr size_t kAccountGamerTag = 0x08;  // WCHAR[16]
constexpr size_t kAccountXuidOnline = 0x28;
constexpr size_t kAccountCachedFlags = 0x30;

constexpr size_t kGamerTagChars = 16;

// The storage device the profile is reported as living on -- device id 1, the
// hard drive presented in storage_device.cpp.
constexpr uint32_t kDeviceIdHdd = 1;

// Ends the enumeration walk, and the exact value matters.
//
// The caller of the list builder tests it and populates nothing unless it is
// 18 (sub_922E3D78, on the message that rebuilds the sign-in list):
//
//     sub_922E3BA0(this)          ; walk the profiles, return the last result
//     cmpwi  cr6, r3, 18          ; must be ERROR_NO_MORE_FILES exactly
//     bne    cr6 -> loc_922E3FA0  ; anything else: bail, build no rows
//     ...                          ; == 18: read the bucket counters, build the UI
//
// This was 0x103, labelled ERROR_NO_MORE_FILES but actually ERROR_NO_MORE_ITEMS
// (259). So the walk ran, both profiles were enumerated, their entries were
// constructed and linked -- and then the caller discarded the lot and drew an
// empty Sign In screen. XStaticEnumerator::WriteItems already returns 18 when
// it runs out; the old value was overriding a correct answer with a wrong one.
//
// Still deliberately not 5: the guest treats 5 as "storage is not ready yet,
// wait for the device notification and try again", which would have it spin
// waiting for a device that is already there.
constexpr uint32_t kNoMoreProfiles = 18;  // ERROR_NO_MORE_FILES

void StoreBe64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>(v >> (56 - i * 8));
  }
}

// szGamerTag is big-endian UTF-16, NUL-terminated.
void WriteGamerTag(uint8_t* dst, const std::string& text) {
  size_t i = 0;
  for (; i < text.size() && i + 1 < kGamerTagChars; ++i) {
    const auto ch = static_cast<uint16_t>(static_cast<unsigned char>(text[i]));
    dst[i * 2] = static_cast<uint8_t>(ch >> 8);
    dst[i * 2 + 1] = static_cast<uint8_t>(ch);
  }
  dst[i * 2] = 0;
  dst[i * 2 + 1] = 0;
}

// One record, for a profile that named itself off disk.
void FillProfileFrom(X_PROFILEENUMRESULT* out, const nxe_profile::StagedProfile& staged) {
  std::memset(out, 0, sizeof(*out));

  out->xuid_offline = staged.xuid;
  out->device_id = kDeviceIdHdd;

  uint8_t* xai = out->account_info;
  if (staged.account_info.size() == sizeof(out->account_info)) {
    // The console's own record, verbatim: gamertag, live flags, online XUID and
    // cached user flags all as stored. Rebuilding a few fields and zeroing the
    // rest left the guest sorting profiles on values that were never real.
    std::memcpy(xai, staged.account_info.data(), sizeof(out->account_info));
  } else {
    WriteGamerTag(xai + kAccountGamerTag, staged.gamertag);
    StoreBe64(xai + kAccountXuidOnline, staged.online_xuid);
  }
  if (REXCVAR_GET(signin_profiles_offline)) {
    StoreBe64(xai + kAccountXuidOnline, 0);
  }
}

// The offline XUID of the profile actually signed in.
//
// UserProfile::xuid() is a hardcoded placeholder -- 0xB13EBABEBABEBABE, set in
// the SDK's own user_profile.cpp -- and matches nothing on disk. The gamertag
// beside it is already taken from the staged profile for exactly that reason;
// this is the other half of the same fix.
//
// Leaving them apart tells the guest two different things about one person: the
// profile enumerator lists E030000000A8C189 while XamUserGetXUID answers the
// placeholder, so a XUID that came from one cannot be found by the other.
// Anything that resolves the two against each other then concludes it is
// looking at somebody else -- which is what the gamer blade did. dash_2a57
// fetches the signed-in XUID and hands it to GamerRootScene, the scene looks it
// up, finds no such profile, and draws the panel a stranger gets: 0 G, Zone
// None, Offline, Add Friend / Compare Games. Every value it needed was being
// served correctly; it just did not believe the card was yours.
// Read from the cvar, not from ProfileDirectory().
//
// This is called from inside XamUserGetXUID, which the guest asks before it has
// opened anything -- the very first thing it does with the answer is
// XamProfileOpen(xuid, "DASHUSER"). ProfileDirectory() drives the whole profile
// subsystem to answer: staged-profile enumeration, Account blob decryption and
// the generation-scoped caches around them. Reaching all of that from a kernel
// export that early stopped the guest before it ever reached XamProfileOpen, so
// DASHUSER: was never mounted and everything reading through it followed it
// down. profile_xuid is the same value as a plain string and costs nothing.
uint64_t ActiveXuidImpl() {
  if (!REXCVAR_GET(profile_report_staged_xuid)) {
    const auto& runtime = REX_KERNEL_STATE()->user_profile();
    return runtime ? runtime->xuid() : 0;
  }
  const std::string& text = REXCVAR_GET(profile_xuid);
  if (text.size() == 16) {
    char* end = nullptr;
    const uint64_t xuid = std::strtoull(text.c_str(), &end, 16);
    if (end && *end == 0 && xuid) return xuid;
  }
  const auto& profile = REX_KERNEL_STATE()->user_profile();
  return profile ? profile->xuid() : 0;
}

void FillProfile(X_PROFILEENUMRESULT* out) {
  std::memset(out, 0, sizeof(*out));

  const auto& profile = REX_KERNEL_STATE()->user_profile();
  const uint64_t xuid = ActiveXuidImpl();
  // Prefer the gamertag decrypted out of the Account blob; UserProfile's own
  // name is a hardcoded placeholder.
  std::string name = nxe_profile::GamerTag();
  if (name.empty()) {
    name = profile ? profile->name() : std::string("User");
  }

  out->xuid_offline = xuid;
  out->device_id = kDeviceIdHdd;

  uint8_t* xai = out->account_info;
  WriteGamerTag(xai + kAccountGamerTag, name);
  // A non-zero online XUID is what marks this as a LIVE account: guest
  // 0x922E3BA0 sorts profiles into online and offline buckets on exactly this
  // field. See live_signin.cpp for where the value comes from.
  StoreBe64(xai + kAccountXuidOnline, nxe_live::OnlineXuid());
  std::memset(xai + kAccountLiveFlags, 0, 4);
  std::memset(xai + kAccountCachedFlags, 0, 4);
}

u32 XamProfileCreateEnumerator_entry(u32 flags, mapped_u32 handle_out) {
  (void)flags;
  if (!handle_out) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // Every staged profile, so the Sign In scene can actually offer a choice.
  //
  // This used to emit a single record built from the runtime's own UserProfile,
  // which meant the sign-in list could only ever show the profile already
  // signed in -- nothing to switch to. Each profile names itself from its own
  // Account blob, so the list reads as it would on a console.
  const auto staged = nxe_profile::StagedProfiles();
  if (staged.empty()) {
    REXKRNL_WARN("XamProfileCreateEnumerator: no profiles staged under the content root");
  }

  // One record per XamProfileEnumerate call, which is how the guest reads them.
  //
  // The constructor argument is items_per_enumerate -- how many records get
  // written into the caller's buffer on each Enumerate -- not a capacity.
  // Passing the profile count here writes two 392-byte records into a buffer
  // the guest sized for one, which walks off the end of it and takes the
  // process down. The number of profiles is expressed by appending, below.
  auto e = rex::system::make_object<rex::system::XStaticEnumerator<X_PROFILEENUMRESULT>>(
      REX_KERNEL_STATE(), 1);
  const auto result = e->Initialize(0xFE, 0xFE, 0x2000A, 0x20009, 0);
  if (XFAILED(result)) {
    REXKRNL_WARN("XamProfileCreateEnumerator: Initialize failed {:#x}", result);
    return result;
  }

  if (staged.empty()) {
    if (auto* record = e->AppendItem()) {
      FillProfile(record);
    }
  } else {
    for (const auto& entry : staged) {
      if (auto* record = e->AppendItem()) {
        FillProfileFrom(record, entry);
      }
    }
  }

  *handle_out = e->handle();
  static size_t s_last_count = SIZE_MAX;
  if (staged.size() != s_last_count) {
    s_last_count = staged.size();
    for (const auto& entry : staged) {
      REXKRNL_INFO("XamProfileCreateEnumerator:   {} '{}' online_xuid={:016X} ({}, {} byte record)",
                   entry.xuid_text, entry.gamertag, entry.online_xuid,
                   entry.online_xuid ? "LIVE account" : "offline account",
                   entry.account_info.size());
    }
  }
  return X_ERROR_SUCCESS;
}

u32 XamProfileEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 overlapped) {
  (void)overlapped;

  auto e = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }
  auto* dst = buffer.as<uint8_t*>();
  if (dst == nullptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t written = 0;
  const uint32_t result = e->WriteItems(buffer.guest_address(), dst, &written);
  REXKRNL_INFO("XamProfileEnumerate(handle={:#x}, flags={:#x}) -> result {:#x}, {} written",
               handle, flags, result, written);
  if (result != X_ERROR_SUCCESS || written == 0) {
    return kNoMoreProfiles;
  }
  return X_ERROR_SUCCESS;
}

// XamProfileOpen(XUID xuid, "DASHUSER", 0, 0) -- guest 0x92141790 passes the
// XUID straight from XamUserGetXUID by value.
//
// This has to be a raw hook. The typed-hook marshaller cannot carry a 64-bit
// argument at all: ArgTranslator::GetIntegerArgumentValue reads ctx.r3.u32 and
// friends, taking the low 32 bits of every register regardless of the declared
// parameter type. Declaring the parameter u64 changed nothing -- the XUID still
// arrived as 0xBABEBABE, the bottom half of 0xB13EBABEBABEBABE -- so the guest
// was being asked about an identity that does not exist.
//
// That truncation is what made the long-standing intermittent segfault
// reproducible. Answering "success" to open a profile that was never matched
// left the dashboard building objects around a profile it did not have, and the
// destructor at guest 0x9227C750 then made a virtual call through a vtable
// pointer of 1 -- faulting at vm_base + 5, which is the 0x100000005 this port
// has been crashing on since the start.
namespace {

// The second argument is a mount name, and answering "success" without mounting
// anything is why applying a theme did nothing.
//
// XamProfileOpen(xuid, name, 0, 0) makes the profile's contents reachable as
// "name:". The dashboard uses two names for the same profile: guest 0x92141790
// mounts it as "DASHUSER" at startup, and the theme commit at guest 0x922E7AD0
// mounts it as "SkinRoot" to write the chosen skin --
//
//     XamProfileOpen(v7, off_927F2960[0], 0, 0);          // "SkinRoot"
//     snprintf(v8, 0x104u, "%s:\\ThematicSkin", off_927F2960[0]);
//     v4 = sub_92146308(v8, 0x40000000, 0, 0, 2, 0, 0);   // create for writing
//     if ( v4 != -1 ) { sub_9249A4C0(v4, a1, 324, &v6, 0); ... }
//
// -- and the file is read back through the other name, at DASHUSER:\ThematicSkin.
// With nothing mounted the create failed and the write was skipped silently,
// because that -1 is the only thing the guest checks:
//
//     VFS: 'SkinRoot:\' -> [no device]
//     [NtCreateFile] FAILED: path='SkinRoot:\ThematicSkin' -> 0xc000000f
//
// three times, once per theme applied. Selecting a theme did visibly change the
// preview, because the preview is a separate path -- sub_922E7D00 just loads
// file://<theme>\WallPaper1 into the on-screen element -- so the screen looked
// like it was working while nothing was ever persisted.
//
// The link points at the profile's own device (kProfileMountPath), mounted at
// startup in nxe_dash_app.h.
//
// Linking straight into the storage volume instead --
// \Device\Harddisk0\Partition3\Content\<xuid>\FFFE07D1\00010000\<pkg> -- was
// tried first and does not work. The link registers, and then resolution walks
// into the NullDevice the runtime registers for
// \Device\Harddisk0\{{Partition0,Cache0,Cache1}}, which claims the whole
// \Device\Harddisk0 prefix and shadows anything mounted under it afterwards:
//
//     Registered symbolic link: SkinRoot: => \Device\Harddisk0\Partition3\Content\...
//     NullDevice::ResolvePath(\Partition3\Content\...)
//     [NtCreateFile] FAILED: path='SkinRoot:\ThematicSkin' -> 0xc000000f
//
// So the target has to be a device root outside that prefix. Registering the
// same name twice would fail, so any previous link is dropped first -- the
// dashboard opens the same profile repeatedly, under both of its names.
bool MountProfileAs(const std::string& name) {
  auto* fs = REX_KERNEL_FS();
  if (fs == nullptr) {
    return false;
  }
  if (nxe_profile::ProfileDirectory().empty()) {
    REXKRNL_WARN("XamProfileOpen: no profile staged, cannot mount '{}:'", name);
    return false;
  }

  const std::string link = name + ":";
  fs->UnregisterSymbolicLink(link);
  if (!fs->RegisterSymbolicLink(link, nxe_storage::kProfileMountPath)) {
    REXKRNL_WARN("XamProfileOpen: could not mount '{}' at {}", link,
                 nxe_storage::kProfileMountPath);
    return false;
  }

  static std::string s_last;
  if (s_last != link) {
    s_last = link;
    REXKRNL_INFO("XamProfileOpen: mounted '{}' -> {}", link, nxe_storage::kProfileMountPath);
  }
  return true;
}

// The mount name, an ASCII string in guest memory.
std::string GuestAnsi(uint8_t* base, uint32_t addr, size_t max_len = 64) {
  std::string out;
  if (!addr) {
    return out;
  }
  const char* p = reinterpret_cast<const char*>(base + addr);
  for (size_t i = 0; i < max_len && p[i]; ++i) {
    out.push_back(p[i]);
  }
  return out;
}

}  // namespace

// XamUserGetXUID(user_index, type_mask, out) -- who is signed in.
//
// Replaced rather than left to the runtime, which answers UserProfile::xuid():
// the placeholder, which is not any profile on this disk. See ActiveXuid above
// for what that cost.
//
// The shape follows the runtime's own export so nothing else changes: only user
// 0 is signed in, an index past three is a bad argument, and the online XUID is
// preferred when asked for, because a non-zero one is what marks the account as
// a LIVE one. type_mask 1 is the offline XUID, which is what the gamer blade
// asks for -- dash_2a57(1) at guest 0x921F6D58.
REX_HOOK_RAW(__imp__XamUserGetXUID) {
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t type_mask = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;

  if (!out) {
    ctx.r3.u64 = X_E_INVALIDARG;
    return;
  }

  // Deliberately the runtime's own shape, with one value changed.
  //
  // An earlier attempt rewrote the logic as well and answered the derived LIVE
  // XUID whenever the mask asked for an online one. That is not what the runtime
  // does -- UserProfile::type() is the constant 1 | 2, so both of its branches
  // hand back the same XUID and online and offline are never told apart -- and
  // the startup call, whose mask has bit 2 set, came back 0009BABEBABEBABE:
  // nxe_live::OnlineXuid() derives that from the placeholder when the profile has
  // no LIVE identity of its own. The guest was handed an identity that had never
  // existed and crashed shortly after.
  // HRESULTs, not Win32 codes. X_ERROR_NO_SUCH_USER is 0x459 and positive, so a
  // caller testing `>= 0` -- which dash_2a57 does -- reads every failure as
  // success and carries on with the zero XUID this writes out. That is what
  // XamUserGetOnlineCountryFromXUID(0x0) in the log was, and the guest did not
  // survive much past it. The runtime's own export returns X_E_*; so does this.
  uint32_t result = X_E_NO_SUCH_USER;
  uint64_t xuid = 0;
  if (user_index >= 4) {
    result = X_E_INVALIDARG;
  } else if (user_index == 0) {
    const auto& profile = REX_KERNEL_STATE()->user_profile();
    const uint32_t type = (profile ? profile->type() : 0u) & type_mask;
    if (type & (2 | 4 | 1)) {
      xuid = ActiveXuidImpl();
      result = X_ERROR_SUCCESS;
    }
  }
  StoreBe64(base + out, xuid);
  ctx.r3.u64 = result;

  static uint64_t s_said = 0;
  if (xuid && s_said != xuid) {
    s_said = xuid;
    REXKRNL_INFO("XamUserGetXUID: the signed-in user is {:016X}", xuid);
  }
}

REX_HOOK_RAW(__imp__XamProfileOpen) {
  const uint64_t xuid = ctx.r3.u64;
  const std::string name = GuestAnsi(base, ctx.r4.u32);

  const auto& profile = REX_KERNEL_STATE()->user_profile();
  // ActiveXuidImpl(), not profile->xuid(): whatever identity is being reported is the
  // one an open of the signed-in profile arrives with. Comparing against the
  // runtime's placeholder while reporting the staged XUID made the start-up open
  // of DASHUSER look like a request for some other profile, so it took the branch
  // below -- which answers success and deliberately does not mount, because it was
  // written for reading a profile from the Sign In list. Nothing was mounted, and
  // every read through DASHUSER: afterwards failed.
  const uint64_t expected = ActiveXuidImpl();

  REXKRNL_INFO("XamProfileOpen({:016X}, '{}')", xuid, name);

  // Opening a staged profile by its offline XUID.
  //
  // The check below compares against the runtime's UserProfile::xuid(), which
  // is never any staged profile's offline XUID -- so picking a profile in the
  // Sign In list arrived here and was turned away as "no such profile". A XUID
  // that names something actually on disk is a real request: either the profile
  // already signed in, which proceeds, or a different one, which is a switch.
  if (xuid != expected) {
    for (const auto& entry : nxe_profile::StagedProfiles()) {
      if (entry.xuid != xuid) {
        continue;
      }
      const auto& active_dir = nxe_profile::ProfileDirectory();
      const std::string active =
          active_dir.empty() ? std::string() : active_dir.filename().string();
      // Not a sign-in: this call mounts a profile for reading, and in practice
      // only ever arrives with the runtime's own placeholder XUID. Accept a
      // staged one so it cannot fail, but leave switching to XamUserLogon.
      (void)active;
      ctx.r3.u64 = X_ERROR_SUCCESS;
      return;
    }
  }

  if (xuid != expected) {
    REXKRNL_WARN("XamProfileOpen({:#x}): no such profile (this runtime has {:#x})", xuid, expected);
    ctx.r3.u64 = X_ERROR_NO_SUCH_USER;
    return;
  }

  if (!name.empty() && !MountProfileAs(name)) {
    // Nothing behind the name means every path through it would fail later, so
    // say so here rather than let the caller write into a device that is not
    // there.
    ctx.r3.u64 = X_ERROR_FUNCTION_FAILED;
    return;
  }

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamProfileOpen({:#x}) -> ok", xuid);
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// The handle argument is the mount name the open was given.
REX_HOOK_RAW(__imp__XamProfileClose) {
  const std::string name = GuestAnsi(base, ctx.r3.u32);
  if (!name.empty()) {
    if (auto* fs = REX_KERNEL_FS()) {
      fs->UnregisterSymbolicLink(name + ":");
    }
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// UserProfile stores name_ privately with no setter, so the recovered gamertag
// can only reach the guest by answering these directly. Both mirror the
// runtime's own behaviour apart from the string they report.
const std::string& DisplayName() {
  return nxe_profile::ProfileScoped([]() -> std::string {
    const std::string tag = nxe_profile::GamerTag();
    if (!tag.empty()) return tag;
    auto* p = REX_KERNEL_STATE()->user_profile();
    return p ? p->name() : std::string("User");
  });
}

u32 XamUserGetName_entry(u32 user_index, mapped_void buffer, u32 buffer_len) {
  if (user_index >= 4) return X_ERROR_INVALID_PARAMETER;
  if (user_index != 0) return X_ERROR_NO_SUCH_USER;
  // Nobody signed in means nobody to name. Answering with the runtime profile's
  // placeholder ("User") had the dashboard open that non-existent player's
  // gamercard instead of the profile chooser.
  if (!nxe_profile::SignedIn()) return X_ERROR_NO_SUCH_USER;
  auto* dst = buffer.as<char*>();
  if (dst == nullptr || buffer_len == 0) return X_ERROR_INVALID_PARAMETER;

  const std::string& name = DisplayName();
  static bool s_logged = false;
  if (!s_logged) { s_logged = true; REXKRNL_INFO("XamUserGetName -> '{}'", name); }
  const uint32_t limit = buffer_len < 16 ? buffer_len : 16;
  uint32_t i = 0;
  for (; i + 1 < limit && i < name.size(); ++i) dst[i] = name[i];
  dst[i] = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserGetGamerTag_entry(u32 user_index, mapped_void buffer, u32 buffer_len) {
  if (user_index >= 4) return X_ERROR_INVALID_PARAMETER;
  if (user_index != 0) return X_ERROR_NO_SUCH_USER;
  if (!nxe_profile::SignedIn()) return X_ERROR_NO_SUCH_USER;
  auto* dst = buffer.as<uint8_t*>();
  if (dst == nullptr || buffer_len == 0) return X_ERROR_INVALID_PARAMETER;

  // Big-endian UTF-16, NUL terminated; buffer_len counts characters.
  const std::string& name = DisplayName();
  static bool s_logged = false;
  if (!s_logged) { s_logged = true; REXKRNL_INFO("XamUserGetGamerTag -> '{}'", name); }
  const uint32_t limit = buffer_len < 16 ? buffer_len : 16;
  uint32_t i = 0;
  for (; i + 1 < limit && i < name.size(); ++i) {
    dst[i * 2] = 0;
    dst[i * 2 + 1] = static_cast<uint8_t>(name[i]);
  }
  dst[i * 2] = 0;
  dst[i * 2 + 1] = 0;
  return X_ERROR_SUCCESS;
}

// XamUserLogon(XUID* xuids, DWORD flags, XOVERLAPPED* overlapped)
//
// The sign-in flow at guest 0x922E37B8 calls this, waits while the overlapped
// reports 997 (ERROR_IO_PENDING), and only then decides whether a user is
// signed in -- falling through to XamShowSigninUIp if not. As a bare
// REX_EXPORT_STUB it returned an undefined r3, so whether the console believed
// anyone was signed in came down to a stale register.
//
// The overlapped MUST be completed rather than left pending: that loop pumps
// until InternalLow stops being 997, so an uncompleted request would spin
// forever. Completing it immediately is what the runtime does elsewhere for
// requests it can satisfy without blocking, and this one it can -- the profile
// is local and already open.
// Relaunch as another profile.
//
// Spawned from a detached thread rather than here: this runs inside a guest
// call, and the guest should see its logon complete normally rather than have
// the process torn down underneath it.
// This process's own top-level window, so it can be hidden before the
// replacement appears. (avatar_editor_launch.cpp has an equivalent for the
// editor; it is file-local there, and this is three lines.)
BOOL CALLBACK FindOwnWindow(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
    return TRUE;
  }
  *reinterpret_cast<HWND*>(param) = hwnd;
  return FALSE;
}

HWND OwnMainWindow() {
  HWND found = nullptr;
  EnumWindows(FindOwnWindow, reinterpret_cast<LPARAM>(&found));
  return found;
}

void RestartAsProfile(const std::string& xuid_text) {
  // Switch in place. This used to relaunch the process, which worked but meant
  // the dashboard visibly closed and came back.
  //
  // Everything derived from the signed-in profile is cached against
  // nxe_profile::Generation(), so SwitchTo can invalidate all of it with one
  // increment rather than this code having to know each cache by name -- which
  // is the part that would otherwise rot into one profile wearing another's
  // gamerscore.
  //
  // Still guarded to one switch at a time: a single selection arrives here
  // twice, from XamProfileOpen and again from XamUserLogon.
  static std::atomic<bool> switching{false};
  if (switching.exchange(true)) {
    return;
  }

  if (!nxe_profile::SwitchTo(xuid_text)) {
    switching.store(false);
    return;
  }

  switching.store(false);
}

// Was a different staged profile asked for?
//
// Deliberately strict. The guest signs in at startup too, and a XUID that is
// not exactly one of the staged profiles' offline XUIDs -- zero, or the
// runtime's own, which is a different value entirely -- must not be read as a
// switch request, or the dashboard restarts forever.
void MaybeSwitchProfile(mapped_void xuids) {
  if (!xuids || !REXCVAR_GET(profile_switch_restart)) {
    REXKRNL_WARN("Sign-in: logon ignored (no xuid array, or profile_switch_restart is off)");
    return;
  }
  const auto* wanted = xuids.as<const rex::be<uint64_t>*>();
  if (!wanted) {
    return;
  }

  const auto& active_dir = nxe_profile::ProfileDirectory();
  const std::string active = active_dir.empty() ? std::string() : active_dir.filename().string();
  const auto staged = nxe_profile::StagedProfiles();

  for (int slot = 0; slot < 4; ++slot) {
    const uint64_t xuid = static_cast<uint64_t>(wanted[slot]);
    if (!xuid) {
      continue;
    }
    for (const auto& entry : staged) {
      if (entry.xuid != xuid) {
        continue;
      }
      if (entry.xuid_text == active) {
        return;  // already signed in as this one; the startup logon lands here
      }
      // The dashboard signs a profile in for itself while booting -- it opens
      // the Sign In screen on its own and logs on the user it last had, which
      // arms the flag below and then immediately consumes it. That only became
      // visible once the passcode dialog was answerable: before that the boot
      // sign-in stalled on an undismissable prompt and never got this far.
      //
      // A person cannot have chosen a profile in the first seconds of a cold
      // boot, so anything arriving that early is the dashboard's own doing.
      using clock = std::chrono::steady_clock;
      static const auto started = clock::now();
      const auto age = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - started);
      if (age.count() < REXCVAR_GET(profile_switch_grace_seconds)) {
        static std::once_flag boot_once;
        std::call_once(boot_once, [&] {
          REXKRNL_INFO("Profile switch: '{}' requested {}s into startup; that is the dashboard "
                       "signing in for itself, not a selection",
                       entry.gamertag, age.count());
        });
        return;
      }
      // A profile named on the command line is pinned.
      //
      // Left unpinned the dashboard drifts: it signs the chosen profile in at
      // boot and then, twenty seconds later, asks for the other one and gets
      // it. Naming a profile is a statement about who should be signed in, so
      // requests for anyone else are refused.
      if (const std::string pinned = REXCVAR_GET(profile_xuid);
          !pinned.empty() && !EqualsNoCase(pinned, entry.xuid_text)) {
        static std::string last_refused;
        if (last_refused != entry.xuid_text) {
          last_refused = entry.xuid_text;
          REXKRNL_INFO("Sign-in: '{}' requested but '{}' is pinned by profile_xuid; ignoring",
                       entry.gamertag, pinned);
        }
        return;
      }

      // Honour it: sign in whoever the dashboard asked for.
      //
      // The dashboard will not leave the Sign In screen until a logon it made
      // is honoured, so this is what gets it to the blade at all -- refusing
      // these left it in the chooser with no gamertag, library or avatar.
      //
      // Deliberately simple. Recording offers and committing the last one on
      // leaving the chooser was tried, to make the *user's* pick decide; it
      // works for that but leaves a plain boot stuck, because a sign-in that
      // never commits never satisfies the dashboard. Choosing a profile from
      // the chooser is unfinished and off by default (see profile_xuid).
      REXKRNL_WARN("Sign-in: '{}' ({}) requested", entry.gamertag, entry.xuid_text);
      RestartAsProfile(entry.xuid_text);
      return;
    }
  }
}

u32 XamUserLogon_entry(mapped_void xuids, u32 flags, u32 overlapped) {
  if (const auto* w = xuids ? xuids.as<const rex::be<uint64_t>*>() : nullptr) {
    REXKRNL_INFO("XamUserLogon(flags={:#x}) xuids=[{:016X} {:016X} {:016X} {:016X}]", flags,
                 static_cast<uint64_t>(w[0]), static_cast<uint64_t>(w[1]),
                 static_cast<uint64_t>(w[2]), static_cast<uint64_t>(w[3]));
  } else {
    REXKRNL_INFO("XamUserLogon(flags={:#x}) with no xuid array", flags);
  }

  MaybeSwitchProfile(xuids);

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

}  // namespace

REX_EXPORT(__imp__XamUserLogon, XamUserLogon_entry)
REX_EXPORT(__imp__XamUserGetName, XamUserGetName_entry)
REX_EXPORT(__imp__XamUserGetGamerTag, XamUserGetGamerTag_entry)
REX_EXPORT(__imp__XamProfileCreateEnumerator, XamProfileCreateEnumerator_entry)
REX_EXPORT(__imp__XamProfileEnumerate, XamProfileEnumerate_entry)
static rex::ppc::detail::PPCFuncRegistrar _reg_xam_profile_open(
    "__imp__XamProfileOpen", &__imp__XamProfileOpen);

// Shared with profile_read.cpp, which stamps each returned settings record with
// the XUID it belongs to. That has to be the same identity reported everywhere
// else, or the gamercard discards its own values -- see the note there.
namespace nxe_profile {
uint64_t ActiveXuid() { return ActiveXuidImpl(); }
}  // namespace nxe_profile
