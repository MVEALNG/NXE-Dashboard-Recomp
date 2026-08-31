// Add slots to a channel after its definition has been parsed.
//
// The offline "upsell" channels each define exactly one slot -- the Game
// Marketplace one is the orange "Let us Play" card -- and that is all the
// content there is. On a live console these channels are replaced wholesale by
// definitions downloaded from Xbox LIVE, which is where the row of real tiles
// comes from. Offline there is nothing to download, so the tiles have to be
// built here.
//
// Editing XML is not an option. The manifest is compiled into the executable
// rather than read from homepage.xzp (deleting that package changes nothing),
// it is not served through the epix resource loader at 0x922D6C48 (hooked;
// never called), and it is not resident in guest memory when the "Epix Homepage
// Def" thread starts (swept; not found). So rather than feed the parser
// different text, this calls the same functions the parser calls.
//
// The seam is 0x922DAEF0, the channeldef element handler. When it has just
// consumed the closing channeldef tag its state word at +5208 is 8, and the
// channel object is at +5220 -- the same object the manifest parser created,
// with its id at +24 and its slot list at +48/+52 counted at +56 (capped at
// 0x40). At that point the definition is complete and nothing has consumed it
// yet, so slots appended here are indistinguishable from parsed ones.
//
// Everything below mirrors what 0x922DAEF0 does for real XML, using the same
// constructors and the same field offsets:
//
//   slot    = 0x922D6380(channel)      232 bytes, linked and counted
//             +20 description   (0x922CA4A8, resolves %EvResStr(...)%)
//             +32 name          (0x922F18B8)
//             +68 epixid        (0x922F18B8)
//             +80..+92          up to four onclick pointers
//   onclick = 0x922CCD88 over a 48-byte allocation
//             +0  action        "EpixCmd"
//             +4  cmd           "EcNavTo..."
//             +36 helptext      (0x922CA4A8)
//             +40 button        0x5800 == A
//   epix    = 0x922D4408(channel)
//             +12 format        1 == EpixScene
//             +16 id            (0x922F1848)
//             +20 path          "Es..."
//
// The format, action and button numbers are the ids the parser itself looks up,
// from the tables at 0x927F2778, 0x927F27C8 and 0x927F2728.
//
// A slot's epixid has to name an epix declared on the same channel -- 0x922D55B0
// rejects duplicates per channel -- so each new slot brings its own epix entry
// pointing at a scene already compiled into the package.

#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

using namespace rex;

extern "C" {
void __imp__sub_922DAEF0(PPCContext& __restrict ctx, uint8_t* base);  // channeldef handler
void __imp__sub_922D6380(PPCContext& __restrict ctx, uint8_t* base);  // append slot
void __imp__sub_922D4408(PPCContext& __restrict ctx, uint8_t* base);  // append epix
void __imp__sub_922CCD88(PPCContext& __restrict ctx, uint8_t* base);  // construct onclick
void __imp__sub_92144098(PPCContext& __restrict ctx, uint8_t* base);  // guest allocator
void __imp__sub_922F18B8(PPCContext& __restrict ctx, uint8_t* base);  // set string field
void __imp__sub_922F1848(PPCContext& __restrict ctx, uint8_t* base);  // set epix id
void __imp__sub_922CA4A8(PPCContext& __restrict ctx, uint8_t* base);  // set display string
}

REXCVAR_DEFINE_BOOL(channel_extra_slots, true, "Dashboard",
                    "Add tiles to the offline Game Marketplace channel, which otherwise carries "
                    "only its single upsell card.");

namespace {

constexpr uint32_t kFormatEpixScene = 1;
constexpr uint32_t kButtonA = 0x5800;

// One tile.
struct SlotSpec {
  const char* name;
  // Either literal text, or a %EvResStr(IDS_...)% reference to one of the
  // shell's own strings. Guest 0x922CA4A8 decides which by the first character:
  //
  //     if ( a2 && *a2 != 37 ) return sub_922F18B8(a1, a2);   // 37 == '%'
  //     if ( sub_922CA3E0(a2, v5, 0x400, 0) < 0 )
  //         return sub_922F18B8(a1, a2);                      // macro failed
  //     return sub_922F1848(a1, v5);                          // resolved
  //
  // so text that does not start with '%' is taken verbatim, and a macro that
  // fails to resolve is displayed as written -- which is the only reason the
  // invented names below were visible on screen rather than silently blank.
  const char* description;
  const char* epix_id;
  const char* scene;        // an Es* scene compiled into the package
  const char* cmd;          // EcNavTo... command
  const char* helptext;
};

// Game Marketplace. Every command and scene here is one the dashboard already
// uses elsewhere, so nothing depends on content that only exists online.
// The shell has exactly twenty-five resource names, listed at 0x920279B8 and
// installed into the lookup table at 0x927F25F0:
//
//     IDS_ADD_FRIEND    IDS_CHANNELNAME_WELCOME  IDS_CHANNELNAME_FRIENDS
//     IDS_CHANNELNAME_XBOX360  IDS_TELLMEMORE    IDS_INSIDEXBOX
//     IDS_GAMES         IDS_FRIENDS              IDS_VIDEO
//     IDS_PRIMETIME     IDS_PROMOTIONS           IDS_HDDVD
//     IDS_SOLUTIONS_DESC  IDS_SOLUTIONS          IDS_SETTINGS
//     IDS_MEDIACENTER_LINE2  IDS_MEDIACENTER     IDS_PICTURELIBRARY
//     IDS_MUSICLIBRARY  IDS_VIDEOLIBRARY         IDS_GAMESLIBRARY
//     IDS_GAMERCARD     IDS_DISKINTRAY           IDS_SELECTSLOT
//     IDS_SELECT
//
// IDS_WHATSNEW, IDS_XBOXBASICS and IDS_STORAGE were never among them -- they
// were invented here, so every lookup missed and the tiles drew the macro text
// itself. There is no resource to point at for those three, so they are written
// as literal text, which the description field takes as-is.
const SlotSpec kGamesSlots[] = {
    {"Games Library", "%EvResStr(IDS_GAMESLIBRARY)%", "SLOT_GAMES_LIBRARY", "EsGameLibrary",
     "EcNavToGamesLibrary", "%EvResStr(IDS_SELECTSLOT)%"},
    {"What is New", "What's New", "SLOT_WHATS_HOT", "EsWhatsHot", "EcNavToWhatsNew",
     "%EvResStr(IDS_SELECTSLOT)%"},
    {"Xbox Basics", "Xbox Basics", "SLOT_XBOX_BASICS", "EsXboxBasics",
     "EcNavToXboxBasics", "%EvResStr(IDS_SELECTSLOT)%"},
    {"Storage", "Storage", "SLOT_STORAGE", "EsStorageUpsell",
     "EcNavToStorageUpsell", "%EvResStr(IDS_SELECTSLOT)%"},
};

uint32_t Be32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreBe32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

using GuestFn = void (*)(PPCContext& __restrict, uint8_t*);

// Call a guest function and put the context back as it was. The callee runs on
// the guest stack this hook was entered with and balances it; restoring the
// saved context afterwards also restores r1, so consecutive calls do not drift.
uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFn fn, uint32_t a, uint32_t b = 0) {
  PPCContext saved = ctx;
  ctx.r3.u32 = a;
  ctx.r4.u32 = b;
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

// The string setters take a guest pointer, so the text has to live in guest
// memory. They copy it, but the block is small and allocated once per boot.
uint32_t GuestString(PPCContext& ctx, uint8_t* base, const char* s) {
  const uint32_t n = uint32_t(std::strlen(s)) + 1;
  const uint32_t p = Call(ctx, base, __imp__sub_92144098, n);
  if (p) std::memcpy(base + p, s, n);
  return p;
}

void SetString(PPCContext& ctx, uint8_t* base, GuestFn setter, uint32_t field, const char* text) {
  const uint32_t s = GuestString(ctx, base, text);
  if (s) Call(ctx, base, setter, field, s);
}

std::string ChannelId(uint8_t* base, uint32_t channel) {
  const uint32_t str = Be32(base, channel + 24);
  if (!str) return {};
  std::string out;
  for (int i = 0; i < 64; ++i) {
    const uint16_t ch = (uint16_t(base[str + i * 2]) << 8) | base[str + i * 2 + 1];
    if (!ch) break;
    out.push_back(ch < 0x80 ? char(ch) : '?');
  }
  return out;
}

bool AddSlot(PPCContext& ctx, uint8_t* base, uint32_t channel, const SlotSpec& spec) {
  // The epix first: a slot whose epixid names nothing has no visual.
  const uint32_t epix = Call(ctx, base, __imp__sub_922D4408, channel);
  if (!epix) return false;
  StoreBe32(base, epix + 12, kFormatEpixScene);
  SetString(ctx, base, __imp__sub_922F1848, epix + 16, spec.epix_id);
  SetString(ctx, base, __imp__sub_922F18B8, epix + 20, spec.scene);

  const uint32_t slot = Call(ctx, base, __imp__sub_922D6380, channel);
  if (!slot) return false;
  SetString(ctx, base, __imp__sub_922CA4A8, slot + 20, spec.description);
  SetString(ctx, base, __imp__sub_922F18B8, slot + 32, spec.name);
  SetString(ctx, base, __imp__sub_922F18B8, slot + 68, spec.epix_id);

  const uint32_t onclick_mem = Call(ctx, base, __imp__sub_92144098, 48);
  if (onclick_mem) {
    const uint32_t onclick = Call(ctx, base, __imp__sub_922CCD88, onclick_mem);
    if (onclick) {
      SetString(ctx, base, __imp__sub_922F18B8, onclick + 0, "EpixCmd");
      SetString(ctx, base, __imp__sub_922F18B8, onclick + 4, spec.cmd);
      SetString(ctx, base, __imp__sub_922CA4A8, onclick + 36, spec.helptext);
      StoreBe32(base, onclick + 40, kButtonA);
      StoreBe32(base, slot + 80, onclick);  // first of the four onclick pointers
    }
  }
  return true;
}

}  // namespace

extern "C" void sub_922DAEF0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t parser = ctx.r3.u32;
  __imp__sub_922DAEF0(ctx, base);
  if (!parser || !REXCVAR_GET(channel_extra_slots)) return;

  // State 8 is "the channeldef just closed"; anything else is mid-definition.
  if (Be32(base, parser + 5208) != 8) return;

  const uint32_t channel = Be32(base, parser + 5220);
  if (!channel) return;
  const std::string id = ChannelId(base, channel);
  if (id != "Games") return;

  static bool s_done = false;
  if (s_done) return;
  s_done = true;

  const uint32_t before = Be32(base, channel + 56);

  // Hide the stock upsell card. The parser writes 0 to a slot's +104 for
  // <visible>no</visible>, so the same field hides the one slot the offline
  // definition ships with -- no list surgery, and the slot stays allocated and
  // linked exactly as it was.
  //
  // Only touch it if it currently reads as visible. If the default turns out
  // not to be 1 then +104 does not mean what this assumes, and blanking it
  // would risk hiding everything; in that case leave it alone and say so.
  const uint32_t head = Be32(base, channel + 48);
  if (head) {
    const uint32_t upsell = head - 4;
    const uint32_t visible = Be32(base, upsell + 104);
    if (visible == 1) {
      StoreBe32(base, upsell + 104, 0);
      REXKRNL_WARN("[slots] hid the stock upsell slot at {:#x}", upsell);
    } else {
      REXKRNL_WARN("[slots] upsell slot at {:#x} has visible={:#x}, not the expected 1; leaving it",
                   upsell, visible);
    }
  }

  int added = 0;
  for (const auto& spec : kGamesSlots) {
    if (AddSlot(ctx, base, channel, spec)) ++added;
  }
  REXKRNL_WARN("[slots] '{}': {} slot(s) before, {} added, {} now", id, before, added,
               Be32(base, channel + 56));
}
