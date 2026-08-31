// Why the Sign In list is empty.
//
// Everything up to the guest is known good: XamProfileCreateEnumerator offers
// both staged profiles and XamProfileEnumerate hands them over one at a time,
// ending cleanly.
//
//     XamProfileEnumerate(handle=0xf8000228, flags=0x2) -> result 0x0, 1 written
//     XamProfileEnumerate(handle=0xf8000228, flags=0x2) -> result 0x0, 1 written
//     XamProfileEnumerate(handle=0xf8000228, flags=0x2) -> result 0x12, 0 written
//
// So the records reach the guest and something inside it drops them. The two
// functions that could are sub_922E4258, which allocates a 704-byte entry per
// record and links it into the list at r3+16, and sub_922E4018, which fills
// that entry in and can return 0 -- and a zero return makes sub_922E4258 skip
// the profile without a trace.
//
// Both are wrapped here rather than replaced. DEFINE_REX_FUNC makes `sub_X` a
// *weak* alias for `__imp__sub_X`, which holds the real body, so a strong
// definition of `sub_X` wins the link and can still call the original. That
// gives arguments and return values without changing behaviour.
//
// Purely diagnostic. Delete once the list works.

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

using namespace rex;

// The real bodies, as emitted by the recompiler.
extern "C" REX_FUNC(__imp__sub_922E4018);
extern "C" REX_FUNC(__imp__sub_922E4258);

// The four guest functions that build a profile list. Only sub_922E3BA0 was
// ever observed running, and it polls every frame -- which makes it a cached
// refresh for something, not necessarily what the Sign In scene draws from.
// Wrapping all four says which one the scene actually uses, and who calls it.
extern "C" REX_FUNC(__imp__sub_922E3BA0);
extern "C" REX_FUNC(__imp__sub_922EBF58);
extern "C" REX_FUNC(__imp__sub_92526790);
extern "C" REX_FUNC(__imp__sub_921F72A8);

// sub_922EC430 asks sub_922EBF58 "are there any profiles at all"; whatever calls
// it is close to the sign-in flow. Neither has been seen running.
extern "C" REX_FUNC(__imp__sub_922EC430);

// sub_92658238 wraps XamAvatarGetManifestsByXuid -- the dashboard's only route
// to another profile's avatar, and so what the sign-in tiles would need. It has
// no static callers (indirect dispatch only), so whether it runs at all can
// only be answered at runtime.
extern "C" REX_FUNC(__imp__sub_92658238);

REXCVAR_DEFINE_BOOL(signin_trace, false, "Profile",
                    "Trace the guest functions that build the Sign In profile list.");

// sub_922E4018(entry, record, account_info, device_id, unk) -> entry or 0
//
// r4 is the enumerated record, so its first 8 bytes are the offline XUID --
// enough to say which profile each call is about.
extern "C" REX_FUNC(sub_922E4018) {
  const bool trace = REXCVAR_GET(signin_trace);
  uint64_t xuid = 0;
  if (trace && ctx.r4.u32) {
    const uint8_t* record = base + ctx.r4.u32;
    for (int i = 0; i < 8; ++i) {
      xuid = (xuid << 8) | record[i];
    }
  }

  __imp__sub_922E4018(ctx, base);

  if (trace) {
    REXKRNL_INFO("[signin] entry ctor for {:016X}: device={:#x} -> {:#010x} ({})", xuid,
                 ctx.r7.u32, ctx.r3.u32, ctx.r3.u32 ? "kept" : "DROPPED");
  }
}

// sub_922E4258(list, record, account_info, 1, unk, device) -- links the entry.
extern "C" REX_FUNC(sub_922E4258) {
  const bool trace = REXCVAR_GET(signin_trace);
  const uint32_t list = ctx.r3.u32;

  __imp__sub_922E4258(ctx, base);

  if (trace) {
    // The bucket counters the caller keeps, four words at list-12+64.
    const uint32_t owner = list >= 12 ? list - 12 : 0;
    if (owner) {
      auto word = [&](uint32_t off) {
        const uint8_t* p = base + owner + off;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
      };
      REXKRNL_INFO("[signin] after add: offline={} online={} flagged={} plain={}", word(64),
                   word(68), word(72), word(76));
    }
  }
}

// Each of the four list builders, reported once per distinct caller so a
// per-frame poll does not bury a one-shot call from the sign-in scene.
namespace {

void TraceOnce(const char* who, uint32_t caller) {
  static std::mutex mutex;
  static std::set<std::pair<std::string, uint32_t>> seen;
  std::lock_guard<std::mutex> lock(mutex);
  if (seen.insert({who, caller}).second) {
    REXKRNL_WARN("[signin] {} called from {:#010x}", who, caller);
  }
}

}  // namespace

#define TRACE_LIST_BUILDER(sym)                                    extern "C" REX_FUNC(sym) {                                         if (REXCVAR_GET(signin_trace)) {                                   TraceOnce(#sym, static_cast<uint32_t>(ctx.lr));                }                                                                __imp__##sym(ctx, base);                                       }

TRACE_LIST_BUILDER(sub_922E3BA0)
TRACE_LIST_BUILDER(sub_922EBF58)
TRACE_LIST_BUILDER(sub_92526790)
TRACE_LIST_BUILDER(sub_921F72A8)
TRACE_LIST_BUILDER(sub_922EC430)
TRACE_LIST_BUILDER(sub_92658238)
