// The Profile item on the gamer blade.
//
// Pressing Profile did nothing at all. The cause is not a failed navigation --
// nothing was ever navigated to. The blade's dispatcher at guest 0x922E9108 is a
// nine-way jump table, and its table at 0x9202ACB0 (00 0c 18 24 5c 84 98 b0 d4)
// maps the case value straight onto the handlers:
//
//     case 0  XamShowMessagesUI
//     case 1  XamShowGamerCardUI                       <- Profile
//     case 2  sub_922E8F98
//     case 3  dash_294c(nav, "1500_AccountManagementHome.xur", 0, 0, 0)
//     case 4  dash_2a65(nav, "Signin.xzp", "SigninScene.xur", ...)
//     case 5  dash_2a65(nav, "Gamer.xzp",  "ThemesRoot.xur",  ...)
//     case 6  XamShowSigninUIp
//     case 7  XamShowCreateProfileUI
//     case 8  XamShowMarketplaceUIEx
//
// A trace of the blade confirmed which one Profile takes -- pressing it produced
// exactly this, three times, and never any navigation:
//
//     blade action dispatch #1
//     XamShowGamerCardUI(user=0, ...) -- not implemented
//     blade action #1 done -> 0x0
//
// XamShowGamerCardUI is XAM's own system gamercard blade, not a dashboard scene.
// This runtime has no system UI layer at all -- the XamShow*UI functions it does
// implement (XamShowMessageBoxUI, for one) complete their overlapped immediately
// without drawing anything -- so there is nothing behind that call and no way to
// put the real one there short of building a UI framework. The guest ignores the
// return value, so there is no fallback to lean on either:
//
//     bl  dash_297a
//     bl  XamShowGamerCardUI
//     b   done
//
// What this does instead, and why
// -------------------------------
// It runs the dashboard's own sign-in / profile scene -- case 4 above, a sibling
// entry in this very menu. That scene is entirely local: it needs no Xbox LIVE,
// and it loads through the same section:// mechanism as every scene that already
// works.
//
// Account management was tried first and abandoned, which is worth recording so
// it is not tried again. 1500_AccountManagementHome.xur (case 3) loads fine --
// the "accountm" section is present at 0x92BD2300, 735197 bytes -- but it is the
// LIVE account editor, so it immediately asks whether Xbox LIVE is reachable and
// refuses when the answer is no. Answering that query yes (see xlivebase.cpp)
// gets past the refusal and straight into an account subsystem that is not
// there: XamProfileFindAccount, XamProfileLoadAccountInfo,
// XamUserGetOnlineCountryFromXUID, MembershipTier, SubscriptionType, UserTenure
// and OnlineXUIDFromOfflineXUID are all bare stubs. Implementing them one by one
// simply advanced the screen to the next missing dependency, and the crash rate
// with that screen wired up went the wrong way -- 1 in 3, then 2 in 4, then 3 in
// 4. It needs XAM's LIVE account layer built properly, not a chain of guesses.
//
// Being plain about it: this is not what the button does on a console. There,
// Profile opens a system blade owned by XAM. That blade does not exist here and
// cannot be conjured, so the choice is between a button that does nothing and a
// button that opens the closest real profile screen the dashboard can render
// without inventing an account. Nothing is faked -- the scene, the navigation and
// the container are all the dashboard's own, reached through its own code -- but
// the mapping from this button to that screen is this port's decision, not the
// guest's. If a system UI layer ever exists, delete this file and the button goes
// back to XAM.
//
// The real blade, and why it is not the default
// ---------------------------------------------
// On a console Profile opens a Guide blade -- a list on the left (View Games,
// Edit Profile, Game Defaults, Auto Sign-In, Join Xbox LIVE) with the gamercard
// panel beside it -- and not a dashboard scene at all. Those scenes are not
// lost: gamer.xzp carries GamerRootScene.xur and GamerCardPanelScene.xur, which
// are exactly that list and that panel, and the dashboard already navigates
// into gamer.xzp for Themes (case 5 above). So the blade is tried by name
// before anything else.
//
// It draws, which was worth finding out -- the ThemesRoot symptom does not apply
// to the whole package. What it will not do is show you. GamerRootScene picks
// its panel from the gamer it is given in dash_2a65's fourth argument, and with
// nothing there it comes up as a stranger: 0 G, Zone None, and the Add Friend /
// Compare Games panel instead of the profile menu.
//
// Passing the descriptor the dashboard itself last used made it worse rather
// than better -- the gamertag went blank too -- so that pointer does not stay
// valid to be borrowed later. Binding it properly means finding where the
// descriptor is built rather than catching one in flight, and that needs the
// dashboard image open in a disassembler.
//
// So it is off by default and kept behind the cvar. The profile page below is
// what the button opens, because it is the one that shows your profile and
// carries Change Gamer Picture.
//
// The profile page
// ----------------
// Account management is not a profile -- it is the billing and subscription
// editor, which is not what anyone presses Profile to see. profile.xboxlive.com
// still serves the actual profile, so tools/fetch_profile.py writes the
// gamertag, gamerscore, reputation, membership, location, bio and follower
// counts to gamedir/profile.txt, and that is built into a page at boot like the
// inbox is. This opens that page, and only falls back to account management
// when it has not been fetched.
//
// Whose profile it is, plainly: the dashboard's gamercard shows the staged
// local profile it boots with, and this page shows the account signed in to
// Xbox Live. They are different profiles with different scores. The live one is
// used here because it is the one with a bio and a location in it.
//
// How it is wired
// ---------------
// The navigator lives at *(blade + 0x10C), which the dispatcher has in r3 but
// XamShowGamerCardUI does not receive. So the dispatcher is wrapped to record the
// blade for the duration of the call, and the redirect only fires while a blade
// dispatch is actually on the stack -- XamShowGamerCardUI is also called from
// elsewhere in the background, and those calls must keep their old behaviour.
//
// The call itself mirrors case 3 exactly, including asking dash_29a6 which of
// the two scene variants to use:
//
//     li   r7, 0
//     lwz  r3, 0x10C(r31)
//     li   r6, 0
//     li   r5, 0
//     bl   dash_294c

#include <atomic>
#include <cstring>
#include <cstdint>
#include <string>

#include "blade_nav.h"
#include "profile_list.h"
#include "channel_pages.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

using namespace rex;

extern "C" {
void __imp__sub_922E9108(PPCContext& __restrict ctx, uint8_t* base);  // blade dispatcher
void __imp__sub_921F6248(PPCContext& __restrict ctx, uint8_t* base);  // dash_294c navigate
void __imp__sub_921F6268(PPCContext& __restrict ctx, uint8_t* base);  // dash_29a6 variant pick
void __imp__sub_92144098(PPCContext& __restrict ctx, uint8_t* base);  // guest allocator
void __imp__sub_922DF6B0(PPCContext& __restrict ctx, uint8_t* base);  // open the gamer blade
void __imp__sub_9222E9B8(PPCContext& __restrict ctx, uint8_t* base);  // selected row index
}

namespace {

// The two scene names, read out of the image as UTF-16 rather than guessed.
constexpr uint32_t kAccountHomeScene = 0x9201E3F4;            // 1500_AccountManagementHome.xur
constexpr uint32_t kAccountHomeGraduationScene = 0x9201E3A0;  // ...HomeGraduation.xur

// Offset of the navigator within the blade object.
constexpr uint32_t kNavigatorOffset = 0x10C;

// The account-management case of the nine, from the jump table above.
constexpr uint32_t kCaseAccountManagement = 3;

// Where the blade keeps its rows, and how far apart.
//
// The dispatcher does not receive the case in a register. It asks the blade
// which row is selected and reads the action out of the row itself:
//
//     r3  = *(blade + 20)                   ; the list, not the blade
//     r3  = sub_9222E9B8(r3, 0)             ; selected index
//     r11 = *(blade + (r3 + 2) * 12)        ; that row's action, 0..8
//     if (r11 > 8) return
//
// so the rows are in the blade object twelve bytes apart, starting at +24. r4
// is not the case at all -- it is an id checked against *(blade+20), which is
// why logging it showed 131327 rather than a number under nine.
constexpr uint32_t kRowsOffset = 24;
constexpr uint32_t kRowStride = 12;
constexpr uint32_t kBladeIdOffset = 20;
constexpr uint32_t kMaxAction = 8;

// The blade scene to try first, as "package#scene". Empty skips straight to the
// profile page.
//
// Empty is the default because GamerRootScene.xur is the gamer blade -- the blade
// the Profile item is on. Opening it from there draws the same blade again, so
// the item appeared to do nothing but loop back on itself, however many times it
// was pressed. It stays available as a setting because seeing the real blade is
// worth having; it is just not what Profile should do.
// The gamercard the Profile item opens, as "package#scene".
//
// GamerRootScene is the blade Profile sits on, so opening that is a loop: press
// Profile on the gamer blade and the gamer blade comes back, however many times
// you press it. The card beside it is a separate scene -- GamerCardPanelScene --
// and that is what the item is for.
//
// It is navigated rather than pushed, because navigating carries a4: the gamer
// the scene is about. Pushed by name it draws bound to nobody and comes up as a
// stranger, which is the Add Friend / Compare Games panel. Empty falls through to
// whatever the settings below say.
REXCVAR_DEFINE_STRING(profile_card_scene, "Gamer.xzp#GamerCardPanelScene.xur", "Dashboard",
                      "Scene the gamer blade's Profile item opens, as package#scene. Empty "
                      "falls back to profile_blade_bound and then the profile page.");

REXCVAR_DEFINE_STRING(profile_blade_scene, "", "Dashboard",
                      "Scene the gamer blade's Profile item opens, as package#scene -- try "
                      "\"Gamer.xzp#GamerRootScene.xur\" for the real blade. Empty, the "
                      "default, opens the profile page built from gamedir/profile.txt, "
                      "which is the one that actually shows your profile.");

// The last gamer descriptor the dashboard navigated gamer.xzp with.
std::atomic<uint32_t> g_gamer_live{0};  // the pointer the dashboard used
std::atomic<uint32_t> g_gamer_copy{0};  // our copy of what it pointed at

// What the account-management item does instead.
//
// The nine items on this menu are the dashboard's own -- sub_922E9678 turns a
// click into {1, action id} and hands the id to the dispatcher in r4 -- but the
// code that builds the list has not been found, so a tenth row cannot be added.
// A seat can be taken though, and case 3 is the account editor: the one item on
// a profile menu that is not about the profile. It is given to Change Gamer
// Picture, which is the item that was wanted here and has nowhere else to sit.
REXCVAR_DEFINE_BOOL(profile_blade_bound, false, "Dashboard",
                    "Open Profile through the dashboard's own 0x922DF6B0, which binds the "
                    "blade to the signed-in gamer. Off, the default, falls through to the "
                    "profile page. 0x922DF6B0 navigates gamer.xzp, and that is the blade "
                    "Profile is already on, so on it loops back to where it started "
                    "instead of showing you anything -- and it runs first, so the page "
                    "below is never reached.");

REXCVAR_DEFINE_STRING(profile_account_item, "gamerpic", "Dashboard",
                      "What the gamer blade's account-management item opens: gamerpic "
                      "for Change Gamer Picture, or account to leave it alone.");

// Which descriptor the blade is opened with. Three ways to be wrong about this,
// so it is a setting rather than a rebuild: "copy" passes our own copy of what
// the dashboard pointed at, "live" passes the dashboard's pointer as-is (this
// blanked the gamertag), "none" passes nothing (a stranger's card).
REXCVAR_DEFINE_STRING(profile_blade_gamer, "copy", "Dashboard",
                      "Who the gamer blade is opened for: copy, live, or none.");

// Set only while a blade dispatch is running on this thread.
thread_local uint32_t t_blade = 0;

uint32_t GuestBe32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Which of the nine the highlighted row runs, or 0xFFFFFFFF when this is not a
// click on the blade's own list.
uint32_t SelectedAction(PPCContext& ctx, uint8_t* base, uint32_t blade, uint32_t id) {
  if (!blade || id != GuestBe32(base, blade + kBladeIdOffset)) return 0xFFFFFFFFu;
  // r3 is still *(blade+20) at that call in the guest -- the same value r4 was
  // just checked against -- not the blade itself.
  PPCContext ask = ctx;
  ask.r3.u64 = id;
  ask.r4.u64 = 0;
  __imp__sub_9222E9B8(ask, base);
  const uint32_t action =
      GuestBe32(base, blade + kRowsOffset + ask.r3.u32 * kRowStride);
  return action > kMaxAction ? 0xFFFFFFFFu : action;
}

}  // namespace

namespace nxe_blade {

// Shared with guide_bridge.cpp so the blade's Messages item can navigate the
// same way its Profile item does.
uint32_t Navigator(uint8_t* base) {
  return t_blade ? GuestBe32(base, t_blade + kNavigatorOffset) : 0;
}

// How much of the descriptor to keep. Only the first four words have been seen
// -- the XUID, then two more -- but the struct is certainly longer, and copying
// spare bytes off a live heap block costs nothing.
constexpr uint32_t kGamerDescriptorBytes = 64;

void NoteGamerParam(PPCContext& ctx, uint8_t* base, uint32_t param) {
  if (!param) return;
  g_gamer_live.store(param, std::memory_order_relaxed);

  // Borrowing the pointer itself did not work -- reusing it later blanked the
  // gamertag as well, so whatever it points at does not stay put. A copy taken
  // while it is still valid is the next thing to try.
  static uint32_t s_copy = 0;
  if (!s_copy) {
    PPCContext alloc = ctx;
    alloc.r3.u64 = kGamerDescriptorBytes;
    __imp__sub_92144098(alloc, base);
    s_copy = alloc.r3.u32;
  }
  if (!s_copy) return;
  std::memcpy(base + s_copy, base + param, kGamerDescriptorBytes);
  g_gamer_copy.store(s_copy, std::memory_order_relaxed);
}

// A gamer descriptor of our own, holding the signed-in XUID.
//
// NoteGamerParam keeps the one the dashboard used, which was the only option
// while the identity was a placeholder nothing agreed on. Now that
// XamUserGetXUID reports the staged profile, the descriptor is reproducible: it
// is a pointer to the eight bytes of the XUID and nothing more -- see
// sub_922DF6B0, which parks it on its own stack and passes &v6.
//
// Built rather than borrowed so opening the card does not depend on having
// watched the dashboard open gamer.xzp first. Allocated once from the guest
// heap, because a descriptor on our stack would be gone by the time the scene
// reads it -- which is what went wrong when the dashboard's own pointer was
// borrowed instead of copied.
uint32_t OwnGamerDescriptor(PPCContext& ctx, uint8_t* base) {
  static uint32_t s_block = 0;
  if (!s_block) {
    PPCContext alloc = ctx;
    alloc.r3.u64 = kGamerDescriptorBytes;
    __imp__sub_92144098(alloc, base);
    s_block = alloc.r3.u32;
    if (s_block) std::memset(base + s_block, 0, kGamerDescriptorBytes);
  }
  if (!s_block) return 0;
  const uint64_t xuid = nxe_profile::ActiveXuid();
  for (int i = 0; i < 8; ++i) {
    base[s_block + i] = uint8_t(xuid >> (56 - 8 * i));
  }
  return s_block;
}

uint32_t GamerParam() {
  const std::string how = REXCVAR_GET(profile_blade_gamer);
  if (how == "live") return g_gamer_live.load(std::memory_order_relaxed);
  if (how == "none") return 0;
  return g_gamer_copy.load(std::memory_order_relaxed);
}

}  // namespace nxe_blade

extern "C" {

void sub_922E9108(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t previous = t_blade;
  t_blade = ctx.r3.u32;

  const uint32_t action = SelectedAction(ctx, base, ctx.r3.u32, ctx.r4.u32);
  REXKRNL_WARN("[blade] dispatch blade={:#x} action={}", ctx.r3.u32,
               action == 0xFFFFFFFFu ? -1 : int32_t(action));

  // Change Gamer Picture, in the seat account management was using.
  if (action == kCaseAccountManagement &&
      REXCVAR_GET(profile_account_item) == "gamerpic" &&
      nxe_channels::HasCategoryPage("gamerpic", "Gamer Picture")) {
    const uint32_t nav = GuestBe32(base, ctx.r3.u32 + kNavigatorOffset);
    const int32_t hr =
        nav ? nxe_channels::OpenCategoryPageOn(ctx, base, nav, "gamerpic", "Gamer Picture")
            : int32_t(0x80004005);
    REXKRNL_WARN("[blade] account item -> Change Gamer Picture on nav {:#x} -> {:#x}", nav,
                 uint32_t(hr));
    if (hr >= 0) {
      ctx.r3.u64 = 0;
      t_blade = previous;
      return;
    }
    // It would not open; fall through so the button still does its old thing.
    ctx.r3.u32 = t_blade;
  }

  __imp__sub_922E9108(ctx, base);
  t_blade = previous;
}

REX_HOOK_RAW(__imp__XamShowGamerCardUI) {
  const uint32_t user_index = ctx.r3.u32;

  // Only redirect the blade's Profile item. Background callers are left alone.
  const uint32_t navigator = nxe_blade::Navigator(base);
  if (!navigator) {
    static int s_seen = 0;
    if (++s_seen <= 3) {
      REXKRNL_WARN("XamShowGamerCardUI(user={}) outside a blade dispatch -- no system UI here",
                   user_index);
    }
    ctx.r3.u64 = X_ERROR_FUNCTION_FAILED;
    return;
  }

  // The gamercard, bound to whoever the blade is showing.
  //
  // First, because it is the only one of these that is not somewhere you already
  // are. GamerParam() is the descriptor the dashboard itself last opened gamer.xzp
  // with, kept by the dash_2a65 hook in theme_trace.cpp -- see blade_nav.h.
  if (const std::string card = REXCVAR_GET(profile_card_scene); !card.empty()) {
    const std::size_t split = card.find('#');
    if (split != std::string::npos && split > 0 && split + 1 < card.size()) {
      uint32_t gamer = nxe_blade::OwnGamerDescriptor(ctx, base);
      if (!gamer) gamer = nxe_blade::GamerParam();
      const int32_t hr = nxe_channels::NavigatePackageScene(
          ctx, base, navigator, card.substr(0, split), card.substr(split + 1),
          gamer, 0);
      REXKRNL_WARN("Profile -> '{}' nav {:#x} gamer {:#x} = {:08X} {:08X} -> {:#x}",
                   card, navigator, gamer, GuestBe32(base, gamer),
                   GuestBe32(base, gamer + 4), uint32_t(hr));
      if (hr >= 0) {
        ctx.r3.u64 = X_ERROR_SUCCESS;
        return;
      }
    }
  }

  // The dashboard's own way in, which knows who you are.
  //
  // sub_922DF6B0 is the whole job in one call: it asks 0x921F6D58 for the signed
  // in XUID, parks it on the stack, and passes a POINTER TO THOSE EIGHT BYTES as
  // dash_2a65's fourth argument -- that is all the "gamer descriptor" ever was.
  // It also picks a different scene pair when there is no gamer, which is the
  // stranger's card we kept landing on. Taking no arguments, it is simply
  // called rather than reconstructed.
  if (REXCVAR_GET(profile_blade_bound)) {
    PPCContext call = ctx;
    __imp__sub_922DF6B0(call, base);
    const int32_t hr = int32_t(call.r3.u32);
    REXKRNL_WARN("Profile -> the dashboard's own gamer blade -> {:#x}", uint32_t(hr));
    if (hr >= 0) {
      ctx.r3.u64 = X_ERROR_SUCCESS;
      return;
    }
  }

  // Failing that, the scene by name -- unbound, but it does draw.
  const std::string wanted = REXCVAR_GET(profile_blade_scene);
  const std::size_t hash = wanted.find('#');
  if (hash != std::string::npos && hash > 0 && hash + 1 < wanted.size()) {
    // Pushed, not navigated.
    //
    // dash_2a65 is what the dashboard's own items use and it takes the gamer as
    // an argument, which is why it was tried -- but it returns 0 and the screen
    // never appears, the same way ThemesRoot does. 0x922C5580 has no place to
    // name a gamer and does draw. Drawing wins: an unbound blade you can see
    // beats a bound one you cannot.
    const int32_t hr = nxe_channels::PushPackageScene(
        ctx, base, navigator, wanted.substr(0, hash), wanted.substr(hash + 1));
    if (hr >= 0) {
      ctx.r3.u64 = X_ERROR_SUCCESS;
      return;
    }
    REXKRNL_WARN("Profile: '{}' would not push -> {:#x}; falling back", wanted,
                 uint32_t(hr));
    ctx.r3.u64 = user_index;
  }

  // The profile page, when there is one.
  if (nxe_channels::HasCategoryPage("profile", "Profile")) {
    const int32_t hr =
        nxe_channels::OpenCategoryPageOn(ctx, base, navigator, "profile", "Profile");
    if (hr >= 0) {
      static bool s_said = false;
      if (!s_said) {
        s_said = true;
        REXKRNL_INFO("Profile -> the profile page, on the blade navigator {:#x}", navigator);
      }
      ctx.r3.u64 = X_ERROR_SUCCESS;
      return;
    }
    REXKRNL_WARN("Profile: the profile page would not open -> {:#x}; falling back to "
                 "account management",
                 uint32_t(hr));
    ctx.r3.u64 = user_index;
  }

  // dash_29a6 picks between the ordinary and the graduation scene, exactly as
  // the guest's own case 3 does.
  __imp__sub_921F6268(ctx, base);
  const uint32_t scene = ctx.r3.u32 ? kAccountHomeGraduationScene : kAccountHomeScene;

  ctx.r3.u64 = navigator;
  ctx.r4.u64 = scene;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = 0;
  __imp__sub_921F6248(ctx, base);

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("Profile -> account management scene {:#x}, navigate result {:#x}", scene,
                 ctx.r3.u32);
  }
}


// Messages -- case 0 of the same blade -- is handled in guide_bridge.cpp. It uses
// nxe_blade::Navigator for the same reason this does: the navigator it needs is
// the blade's, not the dashboard's. Pushing the inbox onto the dashboard's own
// navigator is what returned 0x8030000B and then broke the following BACK.

}  // extern "C"
