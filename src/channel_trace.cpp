// Channel loading, traced.
//
// The top-level tabs -- Events, Inside Xbox, Friends, Video & Music Marketplace,
// Game Marketplace -- are channels, not slots. They come from the channel system
// (homepage.xur out of the homepage section, plus
// controlpack://MobyChannelScene.xur), and they have been seen both present and
// absent on the same build, which points at the channel data rather than at any
// setting.
//
// Welcome and My Xbox are always there because they are local blades; everything
// else arrives with the channel definitions, so if those definitions fail or are
// slow the tabs are simply missing.
//
// Guest 0x922DBB88 is the manifest parser -- a state machine over an XML document
// with "homepage", "channel", "channeldef", "sysext" and "futureassets"
// elements. Its two terminal states are the useful signal:
//
//     if ( v29 == 8 ) { *(a1 + 1048) = 1; return -2147024885; }   // gave up
//     if ( v29 == 9 ) { *(a1 + 1048) = 1; *(a1 + 5196) = 1; }     // completed
//
// So a return of 0x8007000E (E_OUTOFMEMORY) means the parse abandoned, and 0
// means it ran to completion. This logs which, and how many times it is entered,
// which is enough to tell "the definitions never arrived" from "the definitions
// arrived and the tabs still did not draw".
//
// Pass-through: the runtime's own parser runs and its result is what the guest
// sees. Logged at warning level so it survives a default log_level.

#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

using namespace rex;

extern "C" {
void __imp__sub_922DBB88(PPCContext& __restrict ctx, uint8_t* base);  // manifest state machine
void __imp__sub_922CEB00(PPCContext& __restrict ctx, uint8_t* base);  // channel create
void __imp__sub_922CF148(PPCContext& __restrict ctx, uint8_t* base);  // channel REMOVE
}

// Guest 0x922CF148 is the remover: it unlinks a channel, decrements the same
// count at +80 that the creator increments, and frees it. So the creator's count
// is a peak, not a total -- channels are made and then taken away again.
//
// The manifest calls it on each </channel> when the channel's own definition
// sub-parse ended in a non-zero state:
//
//     if ( v11 && *(v11 + 5200) ) sub_922CF148(container, channel);
//
// +5200 is that sub-parser's state, and 8 is its "gave up" state. So a channel
// whose definition could not be parsed is discarded, which is what leaves only
// the local blades behind.
namespace {

uint32_t Be32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

std::string WideAt(uint8_t* base, uint32_t str) {
  if (!str) return "(none)";
  std::string out;
  for (int i = 0; i < 96; ++i) {
    const uint16_t ch = (uint16_t(base[str + i * 2]) << 8) | base[str + i * 2 + 1];
    if (ch == 0) break;
    out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
  }
  return out.empty() ? "(empty)" : out;
}

// The channel's id, written by the manifest into +24 as a wide string.
std::string ChannelId(uint8_t* base, uint32_t channel) {
  return WideAt(base, Be32(base, channel + 24));
}

// Walk the list from container+72 and name every surviving channel. This is the
// question that matters: are the seven that survive the seven tabs, or something
// else entirely?
void DumpChannels(uint8_t* base, uint32_t container) {
  const uint32_t head = container + 72;
  uint32_t node = Be32(base, head);
  int i = 0;
  while (node && node != head && i < 40) {
    // Nodes are linked through the channel's +4; the channel starts one word back.
    const uint32_t channel = node - 4;
    REXKRNL_WARN("[channel]   [{}] id='{}' defpath='{}'", i, ChannelId(base, channel),
                 WideAt(base, Be32(base, channel + 32)));
    node = Be32(base, node);
    ++i;
  }
  REXKRNL_WARN("[channel] === {} channel(s) in the list ===", i);
}

}  // namespace

extern "C" void sub_922CF148(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t container = ctx.r3.u32;
  __imp__sub_922CF148(ctx, base);
  if (container) {
    REXKRNL_WARN("[channel] REMOVED, channel count now {}", Be32(base, container + 80));
    DumpChannels(base, container);
  }
}

// Guest 0x922CEB00 allocates a channel, links it into the list at a1[18]/a1[19]
// and increments the count at a1[20] (capped at 32). That count is the number of
// top-level tabs, so logging it makes tab presence measurable from a log instead
// of only visible on screen -- which is what has made this hard to pin down.
extern "C" void sub_922CEB00(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t owner = ctx.r3.u32;
  __imp__sub_922CEB00(ctx, base);
  if (owner) {
    const uint8_t* p = base + owner + 20 * 4;  // a1[20]
    const uint32_t count =
        (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    REXKRNL_WARN("[channel] created -> {:#x}, channel count now {}", ctx.r3.u32, count);
  }
}

extern "C" void sub_922DBB88(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_922DBB88(ctx, base);

  static uint32_t s_calls = 0;
  static uint32_t s_failed = 0;
  ++s_calls;
  const uint32_t result = ctx.r3.u32;
  if (result != 0) {
    ++s_failed;
  }
  // Report the first few, then only on a change of fortune, so a long run stays
  // readable.
  if (s_calls <= 3 || result != 0) {
    REXKRNL_WARN("[channel] manifest step #{} -> {:#x} ({} failed so far)", s_calls, result,
                 s_failed);
  }
}
