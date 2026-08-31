// Signed in to Xbox LIVE, locally.
//
// The dashboard was reporting "Disconnected / Xbox LIVE" on the gamercard and
// throwing "Can't connect to Xbox LIVE" from the account screen. Answering the
// reachability query in xlivebase.cpp was not enough on its own, and the reason
// is worth stating: those are two different questions.
//
//   0x58003            "can this console reach the LIVE service?"
//   signin_state       "is this user signed in, and how?"
//
// Saying yes to the first while the second still said "signed in locally" left
// the dashboard in a state no console is ever in -- a reachable service and an
// account that never logged into it. Several screens read both and disagreed
// with each other, and the account screen walked into LIVE code holding a
// profile that, by its own reckoning, was offline.
//
// XUSER_SIGNIN_STATE has three values: 0 not signed in, 1 signed in locally,
// 2 signed in to Xbox LIVE. The runtime hardcodes 1:
//
//     uint32_t signin_state() const { return 1; }
//
// and UserProfile exposes no setter, so both entry points that read it are
// overridden here instead: XamUserGetSigninState and XamUserGetSigninInfo. They
// are kept consistent with each other on purpose -- the previous inconsistency
// is the thing being fixed.
//
// The online XUID
// ---------------
// A profile only counts as a LIVE account when its online XUID is non-zero --
// guest 0x922E3BA0 sorts profiles into online and offline buckets on exactly
// that field:
//
//     if ( LODWORD(v9.xai.xuidOnline) ) ++online; else ++offline;
//
// The real Account record is preferred: it is decrypted from the profile on the
// storage device and carries an xuidOnline at +0x28. If that field is zero --
// which it will be for a profile that never signed in to LIVE -- one is derived
// from the offline XUID instead, keeping the low bits so it stays stable across
// runs and tagging it with the 0x0009 prefix that real LIVE XUIDs carry.
//
// That derived value is a fabrication, and the only one here. Everything else --
// gamertag, offline XUID, the account record itself -- is read off disk. There
// is no Xbox LIVE being reached and nothing is authenticated; this is a local
// simulation, made deliberately at the user's direction, the same trade already
// taken for the NCSI probe and the reachability query.
//
// Why this is a switch
// --------------------
// Reporting LIVE changes which panels the dashboard fills its blades with, so it
// is left switchable rather than baked in.
//
// A correction is recorded here deliberately, because the first reading of this
// was wrong and the wrong version was briefly written into this file. Dumping
// every scene the dashboard loads shows that five Offline* slot scenes stop
// loading once LIVE is reported:
//
//     signin_state = 2 (LIVE)     0 of 5 Offline*SlotScene
//     signin_state = 1 (local)    5 of 5 Offline*SlotScene
//
// From that it was concluded that reporting LIVE costs the Welcome, Friends,
// Inside Xbox and Marketplace tabs. That conclusion was wrong. Those five are
// offline PLACEHOLDER panels, not the tabs. The top-level tabs come from the
// channel system -- homepage.xur out of the homepage section, and
// controlpack://MobyChannelScene.xur -- and they are present either way. With
// LIVE reported the dashboard fills them with real channel content instead of the
// offline placeholders, which is more, not less.
//
// The tabs that actually went missing were being blocked by the two modal dialogs
// raised during startup; suppressing those (see live_dialog.cpp) is what brought
// them back, and it had nothing to do with signin_state.
//
// So the honest summary of the setting:
//
//     live_signin = true    account screens work (Manage Account, Active
//                           Downloads, Windows Live ID, Memberships), Network
//                           Settings reports Xbox LIVE: Connected, and the blades
//                           use real channel panels.
//     live_signin = false   the console reports only a local sign-in: the
//                           gamercard reads Disconnected and the account screens
//                           fail, and the blades fall back to the offline
//                           placeholder panels.
//
// true is the default because it is strictly the better of the two here.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/types.h>
#include "profile_list.h"

using namespace rex;

// Set in nxe_dash.toml. See the note above on what each setting costs.
REXCVAR_DEFINE_BOOL(live_signin, true, "Dashboard",
                    "Report the profile as signed in to Xbox LIVE (simulated locally). "
                    "Enables the account screens; hides the offline Welcome/Friends/"
                    "Marketplace tabs.");

namespace nxe_profile {
const std::string& GamerTag();
const std::vector<uint8_t>& AccountBlob();
}  // namespace nxe_profile

namespace nxe_live {

namespace {

// XAMACCOUNTINFO::xuidOnline.
// Within the *decrypted blob*, not within XAMACCOUNTINFO: the structure starts
// 8 bytes in, so its +0x28 online XUID lands here at 0x30. Reading 0x28
// straight off the blob lands in the gamertag's tail, which is always zero --
// so this always concluded the profile had no online XUID and fell back to a
// derived one, even for an account that has a real one.
constexpr size_t kAccountXuidOnline = 0x30;

// Real Xbox LIVE XUIDs carry this in their top 16 bits.
constexpr uint64_t kLiveXuidPrefix = 0x0009000000000000ull;

uint64_t LoadBe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | p[i];
  }
  return v;
}

}  // namespace

// Non-zero, stable across runs, and taken from the profile when the profile has
// one. See the note above.
uint64_t OnlineXuid() {
  return nxe_profile::ProfileScoped([]() -> uint64_t {
    const auto& blob = nxe_profile::AccountBlob();
    if (blob.size() >= kAccountXuidOnline + 8) {
      const uint64_t stored = LoadBe64(blob.data() + kAccountXuidOnline);
      if (stored != 0) {
        REXLOG_INFO("LIVE: using the profile's own online XUID {:#x}", stored);
        return stored;
      }
    }

    const auto& profile = REX_KERNEL_STATE()->user_profile();
    const uint64_t offline = profile ? profile->xuid() : 0;
    const uint64_t derived = kLiveXuidPrefix | (offline & 0x0000FFFFFFFFFFFFull);
    REXLOG_INFO("LIVE: profile has no online XUID; using {:#x} derived from the offline one",
                derived);
    return derived;
  });
}

}  // namespace nxe_live

namespace {

// XUSER_SIGNIN_STATE: 0 not signed in, 1 signed in locally, 2 signed in to LIVE.
constexpr uint32_t kSignedInToLive = 2;
constexpr uint32_t kSignedInLocally = 1;

// 0 until a profile is signed in.
//
// This used to answer "signed in" from boot, before any profile had been
// chosen. The dashboard believed it, found nobody it recognised behind the
// claim, and went looking for someone to sign in -- repeatedly.
uint32_t SigninState() {
  if (!nxe_profile::SignedIn()) {
    return 0;
  }
  return REXCVAR_GET(live_signin) ? kSignedInToLive : kSignedInLocally;
}

// The runtime's own struct, which is not in a public header.
struct X_USER_SIGNIN_INFO {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> unk08;
  rex::be<uint32_t> signin_state;
  rex::be<uint32_t> unk10;
  rex::be<uint32_t> unk14;
  char name[16];
};
static_assert(sizeof(X_USER_SIGNIN_INFO) == 40, "signin info must be 40 bytes");

const std::string& UserName() {
  return nxe_profile::ProfileScoped([]() -> std::string {
    const std::string tag = nxe_profile::GamerTag();
    if (!tag.empty()) {
      return tag;
    }
    const auto& profile = REX_KERNEL_STATE()->user_profile();
    return profile ? profile->name() : std::string("User");
  });
}

// XamPartyGetUserListInternal
// ---------------------------
// Reporting the console as signed in to LIVE starts the Party subsystem, which
// this bare stub then crashed. Guest 0x92526FE0 shows why -- it trusts a count
// the stub never wrote:
//
//     if ( XamPartyGetUserListInternal(&v17) < 0 ) { ...no party... }
//     v8 = v17;                       // member count, written by the call
//     if ( v17 ) {
//         v9 = &v18;                  // 128-byte records, 8 bytes after the count
//         do { memcpy(v14, v9, 128); v9 += 128; } while ( ++v7 < v8 );
//     }
//
// With the count left as stack residue the copy ran off into nothing -- the
// fault address was 0x70160000 past the guest base, far outside guest memory.
//
// The honest answer is an empty party: this console is not in one, and there is
// no LIVE to be in one on. Writing zero and reporting success sends the caller
// down its own no-party path, which it already handles.
u32 XamPartyGetUserListInternal_entry(mapped_void buffer) {
  auto* dst = buffer.as<uint8_t*>();
  if (dst == nullptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::memset(dst, 0, 8);  // count, and the padding before the first record

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamPartyGetUserListInternal -> empty party");
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserGetSigninState_entry(u32 user_index) {
  if (user_index != 0) {
    return 0;  // only one user on this console
  }

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamUserGetSigninState(0) -> {} ({})", SigninState(),
                 REXCVAR_GET(live_signin) ? "signed in to LIVE, simulated locally"
                                          : "signed in locally; LIVE reporting disabled");
  }
  return SigninState();
}

i32 XamUserGetSigninInfo_entry(u32 user_index, u32 flags, mapped_void info_ptr) {
  (void)flags;
  auto* info = info_ptr.as<X_USER_SIGNIN_INFO*>();
  if (info == nullptr) {
    return X_E_INVALIDARG;
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  if (user_index != 0) {
    return X_E_NO_SUCH_USER;
  }

  const auto& profile = REX_KERNEL_STATE()->user_profile();
  info->xuid = profile ? profile->xuid() : 0;
  info->signin_state = SigninState();  // must agree with the call above
  rex::string::copy_truncating(info->name, UserName(), rex::countof(info->name));
  return X_E_SUCCESS;
}

}  // namespace

REX_EXPORT(__imp__XamPartyGetUserListInternal, XamPartyGetUserListInternal_entry)
REX_EXPORT(__imp__XamUserGetSigninState, XamUserGetSigninState_entry)
REX_EXPORT(__imp__XamUserGetSigninInfo, XamUserGetSigninInfo_entry)
