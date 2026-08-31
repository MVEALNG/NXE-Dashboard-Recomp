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

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

using namespace rex;

extern "C" {
void __imp__sub_922E9108(PPCContext& __restrict ctx, uint8_t* base);  // blade dispatcher
void __imp__sub_921F6248(PPCContext& __restrict ctx, uint8_t* base);  // dash_294c navigate
void __imp__sub_921F6268(PPCContext& __restrict ctx, uint8_t* base);  // dash_29a6 variant pick
}

namespace {

// The two scene names, read out of the image as UTF-16 rather than guessed.
constexpr uint32_t kAccountHomeScene = 0x9201E3F4;            // 1500_AccountManagementHome.xur
constexpr uint32_t kAccountHomeGraduationScene = 0x9201E3A0;  // ...HomeGraduation.xur

// Offset of the navigator within the blade object.
constexpr uint32_t kNavigatorOffset = 0x10C;

// Set only while a blade dispatch is running on this thread.
thread_local uint32_t t_blade = 0;

uint32_t GuestBe32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

}  // namespace

extern "C" {

void sub_922E9108(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t previous = t_blade;
  t_blade = ctx.r3.u32;
  __imp__sub_922E9108(ctx, base);
  t_blade = previous;
}

REX_HOOK_RAW(__imp__XamShowGamerCardUI) {
  const uint32_t user_index = ctx.r3.u32;

  // Only redirect the blade's Profile item. Background callers are left alone.
  const uint32_t navigator = t_blade ? GuestBe32(base, t_blade + kNavigatorOffset) : 0;
  if (!navigator) {
    static int s_seen = 0;
    if (++s_seen <= 3) {
      REXKRNL_WARN("XamShowGamerCardUI(user={}) outside a blade dispatch -- no system UI here",
                   user_index);
    }
    ctx.r3.u64 = X_ERROR_FUNCTION_FAILED;
    return;
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


// Messages -- case 0 of the same blade -- is handled in guide_bridge.cpp, which
// routes it into the recompiled Guide (xam.xex as a rexglue DLL module).

}  // extern "C"
