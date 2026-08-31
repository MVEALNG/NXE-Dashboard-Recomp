// EcoInLiveLocale, which decides five of the seven top-level tabs.
//
// The tabs are not missing because the channels are missing. All seven channels
// are parsed and survive (channel_trace.cpp): Games, Video, Friends, Inside Xbox,
// Promotions, WELCOME and XBOX360. What decides whether a channel draws is a set
// of named conditions the homepage .xur binds to, looked up in the table at
// 0x92027D78 and evaluated by 0x922CA280:
//
//     EcoVideoMarketplaceAvailable  sub_922C8A48 bit 3
//     EcoInsideXboxAvailable        sub_922C8A48 bit 4
//     EcoEventsAvailable            sub_922C8A48 bit 5
//
// and sub_922C8A48 is:
//
//     if (cache == 0x80000000) {
//       r = sub_922C89F8();          // EcoInLiveLocale
//       if (r) r = sub_92526748(r);  // locale id -> feature bitmask
//       cache = r;                   // otherwise 0 -- every bit clear at once
//     }
//
// So a false EcoInLiveLocale takes all three tabs out together, which is exactly
// the symptom. Traced, it is false:
//
//     [eco] early-out flag (0x92828B10+0x1DC) = 0x1
//     [eco] InLiveLocale -> locale id 0x0
//     [eco] feature mask -> 0x0  (VideoMarketplace=0 InsideXbox=0 Events=0)
//     [eco] EcoShowWelcomeChannel -> 0x1        <- the one tab that does draw
//
// The value itself is right. sub_922C89F8 is
//
//     if (sub_922CBE00()) return 0;                       // 0x92828B10+0x1DC
//     if (!cached) cached = sub_92526900();
//     return cached;
//
// and the early-out fires first, so the locale is never computed -- the country
// trace below it never prints. Computed, it would succeed: user_country 103 (US)
// maps through 0x920DD510 and 0x920CF618 to locale id 0x67, and the table at
// 0x927FA1F8 gives 0x67 -> 0x3F, which has bits 3, 4 and 5 set. A US console is
// in a Live locale; the dashboard just never gets as far as asking.
//
// Fix the answer, not the flag. That same flag is read by the manifest download
// retry loop at 0x922D0E50, where it decides how long to keep waiting, so
// clearing it would change behaviour that has nothing to do with locale. This
// overrides EcoInLiveLocale alone: when the original returns 0, compute the
// locale the way the original would have, and cache it in the guest's own
// variable at 0x92828AE0 so the rest of the dashboard sees one consistent value.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

using namespace rex;

extern "C" {
void __imp__sub_922C89F8(PPCContext& __restrict ctx, uint8_t* base);  // EcoInLiveLocale
void __imp__sub_92526900(PPCContext& __restrict ctx, uint8_t* base);  // the locale computation
}

REXCVAR_DEFINE_BOOL(eco_force_live_locale, true, "Dashboard",
                    "Answer EcoInLiveLocale from the configured country even when the "
                    "dashboard's own early-out suppresses it. Restores the Video "
                    "Marketplace, Inside Xbox and Events tabs offline.");

namespace {

// The guest's cache for the computed locale id, written by sub_922C89F8.
constexpr uint32_t kCachedLocaleId = 0x92828AE0;

uint32_t Be32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreBe32(uint8_t* base, uint32_t addr, uint32_t value) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

}  // namespace

extern "C" void sub_922C89F8(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_922C89F8(ctx, base);
  if (ctx.r3.u32 || !REXCVAR_GET(eco_force_live_locale)) {
    return;
  }

  // Already computed once on a previous call through here.
  const uint32_t cached = Be32(base, kCachedLocaleId);
  if (cached) {
    ctx.r3.u32 = cached;
    return;
  }

  __imp__sub_92526900(ctx, base);
  const uint32_t locale_id = ctx.r3.u32;
  if (!locale_id) {
    // The country genuinely is not a Live locale. Leave the answer alone.
    return;
  }
  StoreBe32(base, kCachedLocaleId, locale_id);

  static bool s_reported = false;
  if (!s_reported) {
    s_reported = true;
    REXKRNL_INFO("EcoInLiveLocale was suppressed by the dashboard's early-out; answering {:#x} "
                 "from the configured country instead",
                 locale_id);
  }
}
