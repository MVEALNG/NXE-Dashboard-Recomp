// Change Theme, traced.
//
// Change Theme is case 5 of the gamer blade's nine-way dispatcher at guest
// 0x922E9108 (the same jump table profile_ui.cpp documents):
//
//     lis  r11, unk_9202ADC0@ha        ; L"ThemesRoot.xur"
//     lis  r10, unk_92029E84@ha        ; L"Gamer.xzp"
//     addi r5, r11, unk_9202ADC0@l
//     addi r4, r10, unk_92029E84@l
//     b    loc_922E91D8                ; -> dash_2a65(blade->0x10C, r4, r5, 0, 0, 0)
//
// so pressing it is one call to dash_2a65 and nothing else. It returns to the
// profile blade, which is exactly what "it bounces straight back" looks like: the
// navigation was attempted and refused, and the blade simply stayed where it was.
//
// The data is not the problem, which is worth recording so it is not re-checked.
// gamer.xzp is present in the game directory and contains both ThemesRoot and
// ThemePicker, and theme enumeration itself now works -- all four installed
// themes come back from the content enumerator with their real display names:
//
//     ContentEnum: type 00030000 under title 0xfffe07d1 -> 4 item(s)
//       -> 'Halo Trilogy'  'Halo 3 Unite to Fight Theme'  'Halo 3'  'Halo 3 Uprising'
//
// So something along the load-and-push path returns negative. There are five
// places it can, and no way to tell them apart from outside:
//
//     dash_2a65     0x921F6190   build locator, load scene, push it
//       dash_293d   0x921F4EB0     "Gamer.xzp" -> a section:// locator
//       dash_280c   0x9218F518     load ThemesRoot.xur out of that locator
//       dash_280e   0x9218FA60     push the loaded scene onto the navigator
//     sub_922E79A0  0x922E79A0   the scene's own init (message 19), which then
//       sub_922E76B0  0x922E76B0    loads ThemePicker.xur into slot 0
//
// This file wraps each one and reports its result, so the next run says which.
//
// Pass-through only: every wrapper calls the guest's own implementation and
// returns its result untouched. Nothing here changes behaviour -- this is a
// measurement, and it comes out once the answer is in.
//
// How the wrapping works
// ----------------------
// DEFINE_REX_FUNC(sub_X) emits the real body as __imp__sub_X and makes sub_X a
// weak alias of it. Generated code calls the weak name, so a strong sub_X defined
// here wins the link and intercepts guest-internal calls, while __imp__sub_X
// still reaches the original. Same technique as channel_trace.cpp.
//
// Noise control: dash_280c and dash_280e are on every scene load in the
// dashboard, so they are only logged while a theme navigation is actually on the
// stack. The flag is thread-local and set by the two entry points above.

#include <cstdint>
#include <set>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>
#include "discord_presence.h"
#include "profile_list.h"

using namespace rex;

extern "C" {
void __imp__sub_921F6190(PPCContext& __restrict ctx, uint8_t* base);  // dash_2a65
void __imp__sub_921F4EB0(PPCContext& __restrict ctx, uint8_t* base);  // dash_293d
void __imp__sub_9218F518(PPCContext& __restrict ctx, uint8_t* base);  // dash_280c
void __imp__sub_9218FA60(PPCContext& __restrict ctx, uint8_t* base);  // dash_280e
void __imp__sub_922E79A0(PPCContext& __restrict ctx, uint8_t* base);  // ThemesRoot init
void __imp__sub_922E76B0(PPCContext& __restrict ctx, uint8_t* base);  // load a .xur into a slot
void __imp__sub_922E7810(PPCContext& __restrict ctx, uint8_t* base);  // vision-effects check
void __imp__sub_9218FC50(PPCContext& __restrict ctx, uint8_t* base);  // dash_280f, navigate BACK
void __imp__sub_922E88C0(PPCContext& __restrict ctx, uint8_t* base);  // build the theme list
void __imp__sub_92143D10(PPCContext& __restrict ctx, uint8_t* base);  // apply wallpapers
void __imp__sub_921C2850(PPCContext& __restrict ctx, uint8_t* base);  // dash_28b9, load an image
void __imp__sub_92143D98(PPCContext& __restrict ctx, uint8_t* base);  // resolve+mount+apply
}

namespace {

// Set while a theme navigation is running on this thread.
thread_local int t_in_theme = 0;

// Let another file turn this file's scene tracing on for a moment.
//
// dash_293d and dash_280c already log the locator they build and the .xur they
// load, which is exactly what is needed to see why a scene will not open --
// but only while a theme navigation is in progress. The Guide's look goes
// through the same two calls, so it borrows the same switch rather than growing
// a second copy of the logging.
extern "C" void nxe_theme_trace_scope(int delta) { t_in_theme += delta; }

// The handle of the last theme scene that was successfully pushed, so the back
// navigation can say whether it is the one being torn down.
uint32_t g_theme_scene = 0;

// Guest strings are UTF-16 big-endian.
uint32_t Be32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

std::string Wide(uint8_t* base, uint32_t addr, size_t max_chars = 160) {
  if (!addr) {
    return "(null)";
  }
  std::string out;
  const uint8_t* p = base + addr;
  for (size_t i = 0; i < max_chars; ++i) {
    const uint16_t ch = (uint16_t(p[i * 2]) << 8) | p[i * 2 + 1];
    if (ch == 0) break;
    out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
  }
  return out;
}

// r3 as a signed result -- these are HRESULTs, and only the sign matters.
int32_t Result(const PPCContext& ctx) { return static_cast<int32_t>(ctx.r3.u32); }

}  // namespace

extern "C" {

// dash_2a65(navigator, container, scene, a4, a5, out) -- the whole button.
//
// Every navigation is logged, not just the theme one. That is deliberate: if
// pressing Change Theme produces no theme line here, the button is not case 5 at
// all and the search moves somewhere else entirely. Either way the next run
// answers it, and navigations are user-driven so the volume stays small.
void sub_921F6190(PPCContext& __restrict ctx, uint8_t* base) {
  const std::string container = Wide(base, ctx.r4.u32, 64);
  const std::string scene = Wide(base, ctx.r5.u32, 64);
  const bool theme = scene.find("Themes") != std::string::npos;

  REXKRNL_WARN("[theme] navigate container='{}' scene='{}' nav={:#x} from caller {:#010x}",
               container, scene, ctx.r3.u32, ctx.lr);

  // Sign-in offers are only acted on while the chooser is the current scene.
  nxe_profile::NoteSceneOpened(container);
  if (theme) {
    ++t_in_theme;
  }

  __imp__sub_921F6190(ctx, base);

  if (theme) {
    --t_in_theme;
  }
  const int32_t result = Result(ctx);
  REXKRNL_WARN("[theme] navigate '{}' -> {:#x}", scene, static_cast<uint32_t>(result));

  // This is the shell telling us which screen it moved to, which is the only
  // statement of the kind it makes -- there is nothing to read back afterwards
  // asking "what is on screen now". Only a navigation that succeeded counts.
  if (result >= 0 && !scene.empty()) {
    nxe_discord::SetScene(scene);
  }
}

// dash_293d(0, container, 0, out_locator, out_chars) -- container name to a
// section:// locator. If this produces the wrong string nothing after it can work.
void sub_921F4EB0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t out = ctx.r6.u32;
  __imp__sub_921F4EB0(ctx, base);
  if (t_in_theme) {
    REXKRNL_WARN("[theme] dash_293d -> locator '{}'", Wide(base, out, 128));
  }
}

// dash_280c(locator, name, a3, out_scene) -- load a .xur.
void sub_9218F518(PPCContext& __restrict ctx, uint8_t* base) {
  const std::string locator = Wide(base, ctx.r3.u32, 128);
  const std::string name = Wide(base, ctx.r4.u32, 64);
  const uint32_t out = ctx.r6.u32;

  __imp__sub_9218F518(ctx, base);

  if (t_in_theme) {
    uint32_t scene = 0;
    if (out) {
      const uint8_t* p = base + out;
      scene = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    REXKRNL_WARN("[theme] dash_280c '{}' + '{}' -> {:#x}, scene {:#x}", locator, name,
                 static_cast<uint32_t>(Result(ctx)), scene);
  }
}

// dash_280e(navigator, 0, scene, 255) -- push the loaded scene.
void sub_9218FA60(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t nav = ctx.r3.u32;
  const uint32_t scene = ctx.r5.u32;
  __imp__sub_9218FA60(ctx, base);
  if (t_in_theme) {
    if (Result(ctx) >= 0) {
      g_theme_scene = scene;
    }
    REXKRNL_WARN("[theme] dash_280e nav={:#x} scene={:#x} -> {:#x}", nav, scene,
                 static_cast<uint32_t>(Result(ctx)));
  }
}

// dash_280f(leaving, target, transition) -- navigate BACK.
//
// This is the counterpart to dash_280e and the thing that undoes it: it walks
// the parent chain from `leaving` up to `target` with sub_922366C8, destroys
// every scene in between via dash_272d, and makes `target` current again.
//
// The push of ThemesRoot.xur succeeds completely -- load, init and dash_280e all
// return 0 on all nine presses -- and the screen still does not appear, so
// something takes it straight back off again. That something has to come through
// here.
//
// ctx.lr is the guest return address: the recompiler assigns it immediately
// before each call ("ctx.lr = 0x922882E8; sub_9218FC50(ctx, base);"), so it names
// the exact instruction that asked for the back navigation, which can be looked
// up in the image. That is the piece of information this whole file exists to
// get -- knowing that the scene is being popped is not the same as knowing who
// popped it.
void sub_9218FC50(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t leaving = ctx.r3.u32;
  const uint32_t target = ctx.r4.u32;
  const uint32_t transition = ctx.r5.u32;
  const uint64_t caller = ctx.lr;
  const bool ours = g_theme_scene && leaving == g_theme_scene;

  __imp__sub_9218FC50(ctx, base);

  REXKRNL_WARN("[theme] BACK leaving={:#x}{} target={:#x} transition={} from caller {:#x} -> {:#x}",
               leaving, ours ? " (THE THEME SCENE)" : "", target, transition, caller,
               static_cast<uint32_t>(Result(ctx)));

  // The only statement the shell makes that a screen has been left. The scene
  // it lands on is an object rather than a name, so instead of trying to name
  // it, the presence keeps the screens it was told about in a stack and this
  // pops one -- landing back on the dashboard empties it, which is what puts the
  // plain dashboard text back.
  if (Result(ctx) >= 0) {
    nxe_discord::PopScene();
  }

  // Backing out of the Sign In chooser is what commits the pending sign-in, and
  // it comes through here rather than through a navigate -- which is why the
  // commit never fired when it was hooked only to scene navigation.
  nxe_profile::NoteSceneOpened("");
}

// The ThemesRoot scene's init, message 19. Reached only if the push above got
// far enough to initialise the scene, so seeing this at all is already an answer.
void sub_922E79A0(PPCContext& __restrict ctx, uint8_t* base) {
  ++t_in_theme;
  REXKRNL_WARN("[theme] ThemesRootScene init entered");
  __imp__sub_922E79A0(ctx, base);
  REXKRNL_WARN("[theme] ThemesRootScene init -> {:#x}", static_cast<uint32_t>(Result(ctx)));
  --t_in_theme;
}

// sub_922E76B0(scene, name, slot) -- ThemePicker.xur into slot 0, and
// ThemeVisionEffects.xur into slot 1 when a camera is attached.
void sub_922E76B0(PPCContext& __restrict ctx, uint8_t* base) {
  const std::string name = Wide(base, ctx.r4.u32, 64);
  const uint32_t slot = ctx.r5.u32;
  __imp__sub_922E76B0(ctx, base);
  REXKRNL_WARN("[theme] load '{}' into slot {} -> {:#x}", name, slot,
               static_cast<uint32_t>(Result(ctx)));
}

// The vision-effects branch. XUsbcamGetState answers 0 in this port, so this
// should be a no-op returning 0 -- confirming that rules out the camera path,
// which is the one part of the scene's init that depends on hardware.
void sub_922E7810(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_922E7810(ctx, base);
  if (t_in_theme) {
    REXKRNL_WARN("[theme] vision-effects check -> {:#x}", static_cast<uint32_t>(Result(ctx)));
  }
}

// sub_922E88C0 -- where the picker decides which installed themes to list.
//
// The four themes staged on the storage device enumerate correctly at boot
// ("type 00030000 under title 0xfffe07d1 -> 4 item(s)", with their real display
// names) and none of them appear in the picker, which shows only the four the
// dashboard carries itself. So they are either absent from the list this reads
// or rejected by one of its three filters:
//
//     v3 = (*(**(a1 + 48) + 56))(*(a1 + 48));         // the content list
//     do {
//         v6 = v5 + *v3;                              // records, stride 512
//         if ( *(_DWORD *)(v6 + 4) == 196608 )        // 0x30000, kTheme
//         {
//             v7 = *(_WORD *)(v6 + 506);
//             if ( (v7 & 1) == 0 && (v7 & 2) == 0 )   // flags
//             {
//                 v8 = *(_QWORD *)(v6 + 312);         // xuid
//                 if ( (_DWORD)v8 == *(_DWORD *)(a1 + 44) || !(_DWORD)v8 )
//                     ... add it ...
//
// The list is reachable without calling anything: the vtable slot at +56 is a
// one-instruction getter,
//
//     loc_92200F78: addi r3, r3, 0x14 ; blr
//
// so the array is at manager+0x14 (base) and manager+0x1C (count), and the same
// records can be read straight out of guest memory here. Each is dumped with the
// three filter inputs and the verdict, which says whether the themes are missing
// from the list or being discarded by it -- and if discarded, by which test.
void sub_922E88C0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t manager = self ? Be32(base, self + 48) : 0;
  const uint32_t user_xuid_lo = self ? Be32(base, self + 44) : 0;

  if (manager) {
    const uint32_t array = Be32(base, manager + 0x14);
    const uint32_t count = Be32(base, manager + 0x1C);
    REXKRNL_WARN("[theme] list source: {} record(s), user xuid low {:#010x}", count, user_xuid_lo);

    for (uint32_t i = 0; i < count && i < 64; ++i) {
      const uint32_t rec = array + i * 512;
      const uint32_t type = Be32(base, rec + 4);
      const uint16_t flags = static_cast<uint16_t>((uint16_t(base[rec + 506]) << 8) |
                                                   base[rec + 507]);
      const uint32_t xuid_lo = Be32(base, rec + 316);

      const char* verdict = "LISTED";
      if (type != 0x30000) {
        verdict = "skipped: not a theme";
      } else if ((flags & 3) != 0) {
        verdict = "REJECTED by the flags at +506";
      } else if (xuid_lo != user_xuid_lo && xuid_lo != 0) {
        verdict = "REJECTED: belongs to another profile";
      }

      REXKRNL_WARN("[theme]   [{}] type {:#010x} flags {:#06x} xuid_lo {:#010x} '{}' -- {}", i,
                   type, flags, xuid_lo, Wide(base, rec + 8, 48), verdict);
    }
  } else {
    REXKRNL_WARN("[theme] list source: no content manager on the picker");
  }

  __imp__sub_922E88C0(ctx, base);

  if (self) {
    REXKRNL_WARN("[theme] picker took {} custom theme(s)", Be32(base, self + 16));
  }
}

}  // extern "C"

// The last leg: turning a mounted theme package into the on-screen wallpaper.
//
// sub_92143D10 is where a theme actually takes effect, and its first act is to
// throw away the current one:
//
//     sub_92143AB0(a1, a2);                                  // reset to DEFAULT
//     do { snwprintf(v7, 0x104u, L"file://%S\WallPaper%d", a1, v3 + 1);
//          if ( dash_28b9(v7, v4) < 0 ) break;               // load each wallpaper
//          ...
//     } while ( v5 < 8 );
//
// So a theme that mounts but whose first wallpaper will not load leaves exactly
// the default look -- which is the reported symptom, and indistinguishable from
// the outside from never having applied anything at all.
//
// The URL is worth seeing in full. The image on disk is Wallpaper1, and this
// asks for WallPaper1 with a capital P; the dashboard itself uses both spellings
// (the photo path at 0x92223B48 uses "Wallpaper1"), which is harmless on a
// console where the package filesystem is case-insensitive and is not
// necessarily harmless here.
extern "C" {

void sub_92143D98(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_92143D98(ctx, base);
  REXKRNL_WARN("[theme] resolve+mount+apply -> {:#x}", static_cast<uint32_t>(Result(ctx)));
}

void sub_92143D10(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t root = ctx.r3.u32;
  std::string name;
  if (root) {
    const char* p = reinterpret_cast<const char*>(base + root);
    for (int i = 0; i < 32 && p[i]; ++i) name.push_back(p[i]);
  }
  REXKRNL_WARN("[theme] applying wallpapers from root '{}'", name);
  __imp__sub_92143D10(ctx, base);
  REXKRNL_WARN("[theme] wallpapers applied");
}

// A zero-length image is retried forever, which reads as the dashboard freezing.
//
// The game rows draw their artwork from the content metadata as
// "memory://<thumbnail>,<size>", and this port reports no thumbnail -- there is
// none in an extracted package -- so the size is zero and the load fails. The
// shell does not treat that as final: it asks again, every frame, and after
// backing out of the Game Library two of those requests pin the UI thread:
//
//     [theme] load 'memory://40EC5546,0' -> 0x80070057
//     [theme] load 'memory://40EB8846,0' -> 0x80070057
//     ... forever ...
//
// The failure is not only the zero length. Guest 0x9245BCA0 is what parses
// these, and it accepts exactly one separator:
//
//     if ( v4 == 47 && !seen_separator )   // 47 == '/'
//         ...
//     if ( v4 < 48 || v4 > 57 ) break;     // otherwise a hex digit, or fail
//     return -2147024809;                  // 0x80070057
//
// so the form it wants is "memory://<hexaddr>/<hexsize>". The shell writes a
// comma, and a comma is neither '/' nor a hex digit, so the parse breaks out
// and returns 0x80070057 -- which is the error every one of these loads
// reports, character for character. No memory:// image in this port can ever
// resolve, whatever the size, so there is nothing to fix up at the source.
//
// The retry cannot be stopped from here, so the load is made to succeed
// instead: a zero-length memory URL is answered with a real image on disk.
// That is also the whole disc-tile art path -- the tile asks for its box art
// through this same URL and gets 0x80070057 -- so the file it is pointed at is
// the game's cover, and the tile draws it.
//
// Rewriting rather than faking the result matters: the caller wants a decoded
// image object back, and reporting success without one is what crashed this
// port the last time a thumbnail was involved.
// Which image the disc tile is asking for, decided by who is calling.
//
// Guest 0x922E19C0 is the tile's small corner inset. It builds the URL into a buffer at
// object+568 that snwprintf is given as 0x104 wide characters, so a longer path
// than the original string is safe there and nowhere else.
constexpr uint32_t kDiscArtStart = 0x922E19C0;
constexpr uint32_t kDiscArtEnd = 0x922E1B68;
constexpr uint32_t kDiscArtBufferChars = 0x104;

// dash_2a8c, guest 0x921FEBF0, loads the picture that fills the whole tile.
constexpr uint32_t kDiscIconStart = 0x921FEBF0;
constexpr uint32_t kDiscIconEnd = 0x921FED38;

bool RewriteEmptyImageUrl(uint8_t* base, uint32_t address, const std::string& url,
                          bool force = false, const std::string* replacement = nullptr,
                          size_t capacity = 0) {
  // "memory://<hex>,0" -- a buffer with nothing in it.
  if (url.rfind("memory://", 0) != 0) {
    return false;
  }
  // Normally only the empty ones; force covers an image the loader refused.
  if (!force && (url.size() < 3 || url.compare(url.size() - 2, 2, ",0") != 0)) {
    return false;
  }

  // The replacement has to fit where the original was.
  //
  // The URL lives in a buffer the shell sized for what it wrote, and the first
  // attempt here put a 46-character path into space for a 19-character one and
  // crashed the process. "memory://XXXXXXXX,0" is always 19 characters, so the
  // replacement is a five-character file at the root of media: -- the longest
  // name that fits -- and the write is refused outright if it ever would not.
  //
  // p.png is the cover art, not a blank tile. It was a 64x64 placeholder while
  // the retry storm was the thing being fixed, which is why the disc tile drew
  // an empty frame even though this path was already feeding it.
  static const std::string kPlaceholder = "file://media:/p.png";
  const std::string& text_out = replacement ? *replacement : kPlaceholder;

  // Without a stated capacity the only safe budget is what was already there.
  const size_t budget = capacity ? capacity : url.size();
  if (text_out.size() + 1 > budget) {
    return false;
  }

  auto* text = reinterpret_cast<rex::be<uint16_t>*>(base + address);
  for (size_t i = 0; i < text_out.size(); ++i) {
    text[i] = static_cast<uint16_t>(text_out[i]);
  }
  text[text_out.size()] = 0;

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("[theme] '{}' has no image; drawing the placeholder instead", url);
  }
  return true;
}

// dash_28b9(url, out) -- the image loader. UTF-16 big-endian URL.
void sub_921C2850(PPCContext& __restrict ctx, uint8_t* base) {
  std::string url = Wide(base, ctx.r3.u32, 128);

  // The disc tile picks its picture in guest 0x922E19C0, and the choice is not
  // the obvious one:
  //
  //     if ( !*(a1+36) && sub_921FF2C0(v13) >= 0 ) {
  //         snwprintf(a1+568, 0x104, L"memory://%x,%x", v14, v15);
  //         dash_28b9(a1+568, a1+36);          // the box art
  //     }
  //     v8 = *(a1+36);
  //     if ( !*(a1+36) || *(a1+44) ) v8 = *(a1+40);   // <-- generic art
  //     *(a2+20) = v8;
  //
  // a1+36 is not the full-tile picture. Pointing it at the cover put Halo 3 in
  // the small square in the tile's bottom corner, which is where that handle is
  // drawn. The full-tile art is a1+44, loaded by dash_2a8c, and the line
  //
  //     if ( !*(a1+36) || *(a1+44) ) v8 = *(a1+40);
  //
  // is what then puts the plain green disc picture in the inset -- the console
  // layout exactly: cover across the tile, disc glyph in the corner.
  //
  // So the cover belongs to the dash_2a8c load, and each caller gets a
  // different answer:
  //
  //   0x921FEBF0  the full-tile art. The cover.
  //   0x922E19C0  the inset. Answered with the placeholder, which is never
  //               drawn once a1+44 exists -- but it has to succeed, because a
  //               load that keeps failing is retried every frame, and that is
  //               the retry storm that froze the shell.
  //
  // Everything else keeps the placeholder, so the freeze fix stands where it
  // was actually needed.
  static const std::string kDiscArt = "file://media:/disc.png";
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const bool is_inset = caller >= kDiscArtStart && caller < kDiscArtEnd;
  const bool is_cover = caller >= kDiscIconStart && caller < kDiscIconEnd;

  if (is_cover) {
    if (RewriteEmptyImageUrl(base, ctx.r3.u32, url, /*force=*/true, &kDiscArt,
                             kDiscArtBufferChars)) {
      url = Wide(base, ctx.r3.u32, 128);
    }
  } else if (RewriteEmptyImageUrl(base, ctx.r3.u32, url, /*force=*/is_inset)) {
    url = Wide(base, ctx.r3.u32, 128);
  }

  __imp__sub_921C2850(ctx, base);
  int32_t result = Result(ctx);

  // A memory image the loader rejects must not be left failing.
  //
  // The shell retries a failed image every frame and never gives up, which is
  // what froze the dashboard when the game rows had no artwork. Any rejected
  // memory:// load is therefore retried once against a real file on disk, so a
  // picture the loader will not take costs a placeholder rather than the UI.
  // The media scenes ask dashmain for icons that live in dashcommon.
  //
  // MediaSourceSelection.xur, the Video Library's first screen, builds
  //
  //     section://30021000,dashmain#ico_64x_AllDevices.png   -> 0x80300013
  //     section://30021000,dashmain#ico_96x_DVD.png          -> 0x80300013
  //
  // and dashmain is 3,512 bytes -- it cannot hold a 64 pixel icon, let alone
  // two. Both names are in dashcommon, which the same scene reaches for
  // everything else through the common:// form: 'common://upfocus.png' and the
  // rest all load with 0x0 in the same breath. So the container is simply wrong
  // in those two references, and asking dashcommon for them by the name the
  // shell already uses elsewhere is what it meant.
  //
  // Done as a retry rather than a rewrite up front, so a load that would have
  // worked is never diverted. common:// is shorter than the section form, so it
  // always fits in the buffer the caller wrote.
  if (result < 0) {
    static const std::string kDashMain = "section://30021000,dashmain#";
    if (url.rfind(kDashMain, 0) == 0) {
      const std::string retry = "common://" + url.substr(kDashMain.size());
      if (retry.size() < url.size()) {
        auto* text = reinterpret_cast<rex::be<uint16_t>*>(base + ctx.r3.u32);
        for (size_t i = 0; i < retry.size(); ++i) {
          text[i] = static_cast<uint16_t>(retry[i]);
        }
        text[retry.size()] = 0;
        __imp__sub_921C2850(ctx, base);
        result = Result(ctx);
        static int s_logged = 0;
        if (s_logged < 6) {
          ++s_logged;
          REXKRNL_WARN("[theme] '{}' is not in dashmain; retried as '{}' -> {:#x}", url, retry,
                       static_cast<uint32_t>(result));
        }
        url = retry;
      }
    }
  }

  if (result < 0 && !is_cover && url.rfind("memory://", 0) == 0) {
    if (RewriteEmptyImageUrl(base, ctx.r3.u32, url, /*force=*/true)) {
      const std::string fallback = Wide(base, ctx.r3.u32, 128);
      __imp__sub_921C2850(ctx, base);
      result = Result(ctx);
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        REXKRNL_WARN("[theme] '{}' was rejected; fell back to '{}' -> {:#x}", url, fallback,
                     static_cast<uint32_t>(result));
      }
      url = fallback;
    }
  }

  // Every distinct image the shell asks for, once each.
  //
  // Only failures were logged before, which answers "what went wrong" but not
  // "what does this screen even want" -- and for the disc tile's artwork that
  // second question is the one that matters: whether it requests an image at
  // all, and under what URL, decides where the art has to come from.
  {
    static std::set<std::string> seen;
    if (seen.size() < 60 && seen.insert(url).second) {
      REXKRNL_INFO("[theme] image '{}' from {:#x}{} -> {:#x}", url, caller,
                   is_cover ? "  [disc cover]" : (is_inset ? "  [disc inset]" : ""),
                   static_cast<uint32_t>(result));
    }
  }

  if (url.find("WallPaper") != std::string::npos || url.find("Wallpaper") != std::string::npos ||
      result < 0) {
    REXKRNL_WARN("[theme] load '{}' -> {:#x}", url, static_cast<uint32_t>(result));
  }
}

}  // extern "C"
