// The "Eco" condition system, traced.
//
// The top-level tabs are not decided by the channel list -- all seven channels
// are created and survive (see channel_trace.cpp). They are decided by named
// conditions the homepage .xur binds to, evaluated by guest 0x922CA280 over a
// 21-entry name table at 0x92027D78:
//
//     id 0x0d EcoConnectedToLive            -> sub_9213F278
//     id 0x0f EcoInLiveLocale               -> sub_922C89F8 (!= 0)
//     id 0x10 EcoVideoMarketplaceAvailable  -> sub_922C8A48 bit 3
//     id 0x11 EcoInsideXboxAvailable        -> sub_922C8A48 bit 4
//     id 0x13 EcoEventsAvailable            -> sub_922C8A48 bit 5
//
// sub_922C8A48 is the choke point for three of the missing tabs:
//
//     if (cache == 0x80000000) {            // 0x927F25E0, correct in the image
//       r = sub_922C89F8();                 // EcoInLiveLocale
//       if (r) r = sub_92526748(r);         // locale id -> feature bitmask
//       cache = r;                          // else 0 -> every bit clear
//     }
//
// Statically this should all pass: user_country 103 (US) maps through
// 0x920DD510 and 0x920CF618 to locale id 0x67, and the table at 0x927FA1F8
// gives 0x67 -> mask 0x3F, which has bits 3, 4 and 5 set. Nothing writes
// 0x92828CEC, so the early-out in sub_922C89F8 never fires either.
//
// So the static reading says the tabs should be there and they are not. This
// logs what actually happens instead of what should. Pass-through: every hook
// calls the original and reports its result.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

using namespace rex;

extern "C" {
void __imp__sub_922CA280(PPCContext& __restrict ctx, uint8_t* base);  // Eco evaluator
void __imp__sub_922C8A48(PPCContext& __restrict ctx, uint8_t* base);  // feature bitmask
void __imp__sub_92658770(PPCContext& __restrict ctx, uint8_t* base);  // country -> locale index
void __imp__sub_92526618(PPCContext& __restrict ctx, uint8_t* base);  // locale index -> locale id
void __imp__sub_92526748(PPCContext& __restrict ctx, uint8_t* base);  // locale id -> mask
void __imp__sub_922CBE00(PPCContext& __restrict ctx, uint8_t* base);  // the early-out flag
void __imp__sub_92526900(PPCContext& __restrict ctx, uint8_t* base);  // locale id, computed
void __imp__sub_92526880(PPCContext& __restrict ctx, uint8_t* base);  // its first source
}

namespace {

uint32_t Be32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Ids as they appear in the table at 0x92027D78. Only the ones that matter for
// the tabs are named; anything else prints as its number.
const char* EcoName(uint32_t id) {
  switch (id) {
    case 0x04: return "EcoLiveTier";
    case 0x05: return "EcoHdDvdInstalled";
    case 0x06: return "EcoHardDiskConnected";
    case 0x07: return "EcoMediaroomEnabled";
    case 0x08: return "EcoGameOfferAvailable";
    case 0x09: return "EcoVideoOfferAvailable";
    case 0x0a: return "EcoGamerZone";
    case 0x0b: return "EcoGamePlayed";
    case 0x0c: return "EcoGamePlayedAndOfferNotAvailable";
    case 0x0d: return "EcoConnectedToLive";
    case 0x0e: return "EcoExperienceMode";
    case 0x0f: return "EcoInLiveLocale";
    case 0x10: return "EcoVideoMarketplaceAvailable";
    case 0x11: return "EcoInsideXboxAvailable";
    case 0x12: return "EcoShowWelcomeChannel";
    case 0x13: return "EcoEventsAvailable";
    case 0x14: return "EcoUserMembershipMilestone";
    case 0x15: return "EcoSubscriptionType";
    default: return "?";
  }
}

}  // namespace

// The evaluator. The condition kind lives at obj+24 and the evaluated value is
// written to obj+32; the return in r3 is a status, not the answer, for the
// cases that store inline.
extern "C" void sub_922CA280(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t kind = obj ? Be32(base, obj + 24) : 0;
  __imp__sub_922CA280(ctx, base);
  const uint32_t value = obj ? Be32(base, obj + 32) : 0;
  REXKRNL_WARN("[eco] {} (id {:#x}) -> value {:#x} (status {:#x})", EcoName(kind), kind, value,
               ctx.r3.u32);
}

extern "C" void sub_922C8A48(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_922C8A48(ctx, base);
  const uint32_t m = ctx.r3.u32;
  REXKRNL_WARN("[eco] feature mask -> {:#x}  (VideoMarketplace={} InsideXbox={} Events={})", m,
               (m >> 3) & 1, (m >> 4) & 1, (m >> 5) & 1);
}

extern "C" void sub_92658770(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_92658770(ctx, base);
  REXKRNL_WARN("[eco] country -> locale index {}", ctx.r3.u32);
}

extern "C" void sub_92526618(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_92526618(ctx, base);
  REXKRNL_WARN("[eco] locale index -> locale id {:#x}", ctx.r3.u32);
}

extern "C" void sub_92526748(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t id = ctx.r3.u32;
  __imp__sub_92526748(ctx, base);
  REXKRNL_WARN("[eco] mask lookup: locale id {:#x} -> {:#x}", id, ctx.r3.u32);
}

// EcoInLiveLocale itself is hooked in eco_locale.cpp, which fixes it rather than
// only reporting it; one strong definition per guest function.
//
// sub_922C89F8 returns 0 without computing anything when this is non-zero. The
// locale trace below it never fires, so this is the suspect; it reads a field at
// 0x92828B10+0x1DC, which the constructor at 0x922CCDC8 zeroes, so whatever sets
// it does so later and through the base pointer.
extern "C" void sub_922CBE00(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_922CBE00(ctx, base);
  REXKRNL_WARN("[eco] early-out flag (0x92828B10+0x1DC) = {:#x}", ctx.r3.u32);
}

extern "C" void sub_92526900(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_92526900(ctx, base);
  REXKRNL_WARN("[eco] compute locale id -> {:#x}", ctx.r3.u32);
}

extern "C" void sub_92526880(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t out = ctx.r3.u32;
  __imp__sub_92526880(ctx, base);
  REXKRNL_WARN("[eco] locale source -> out {:#x}", out ? Be32(base, out) : 0);
}
