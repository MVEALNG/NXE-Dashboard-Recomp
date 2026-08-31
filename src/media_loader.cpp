// Optical media state for the NXE dashboard.
//
// XamLoaderGetMediaInfo ships as a bare REX_EXPORT_STUB, so it never wrote
// either of its out-parameters -- and the dashboard called it 178 times in a
// 60-second run, reading uninitialised stack as the media type every time.
//
// That type is a switch selector, not a flag. Guest 0x922E15A8:
//
//     XamLoaderGetMediaInfo(&type, info);
//     if (type) {
//       if (type < 3) goto no_disc_ui;          // unsigned compare
//       switch (type) {
//         case 3: ... error dialog (strings 84/134/135) ...
//         case 5: goto no_disc_ui;
//         case 7: ... treat as a launchable game disc ...
//       }
//     } else {
//       ... query the tray and actuate it ...
//     }
//
// so a stray value could raise a disc-error dialog, start a title launch, or
// drive the tray, purely on stack residue.
//
// The truthful answer is that there is no optical drive and no disc: nothing in
// this port mounts one. Two values express "no media", and the guest's own code
// says which to use. Guest 0x922E16D0 is explicit:
//
//     XamLoaderGetMediaInfo(&type, &extra);
//     if (type == -2 || !type) return 0;        // no disc: do nothing
//     HalOpenCloseODDTray(1);
//
// Both -2 and 0 mean "no disc", but they are not interchangeable in the caller
// above: 0 falls into the else branch that queries and actuates the tray, while
// -2 is non-zero, fails the unsigned "< 3" test, matches no case, and so leaves
// every path alone. -2 is therefore the value that says "nothing there" without
// asking a drive that does not exist to do anything. It is taken from the
// guest's own handling rather than invented.
//
// A related defect is deliberately NOT fixed here, and is worth recording.
// XamLoaderGetDvdTrayState is declared in the runtime as
//
//     u32 XamLoaderGetDvdTrayState_entry(mapped_u32 out_state)
//
// writing the state to an out-parameter and returning X_STATUS_SUCCESS. The
// real API takes no arguments and returns the state in r3, which is exactly how
// the guest uses it (0x922E1694):
//
//     bl      XamLoaderGetDvdTrayState
//     cmplwi  r3, 2
//     blt     close_tray          ; < 2  -> HalOpenCloseODDTray(1)
//     cmplwi  r3, 4
//     bge     done                ; >= 4 -> do nothing
//     li      r3, 0               ; 2..3 -> HalOpenCloseODDTray(0)
//
// Returning X_STATUS_SUCCESS (0) therefore reads as a tray state of 0 and tells
// the dashboard to close a tray that does not exist. Correcting it means
// changing a signature the rest of the runtime shares, which is wider than this
// change needs to be -- and with the media type above reporting -2, the caller
// no longer reaches that branch. Left as is, on the record, rather than fixed
// quietly as a side effect.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstring>

#include <rex/cvar.h>
#include <rex/string.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "game_launch.h"
#include "title_names.h"

using namespace rex;

// The title presented as the disc in the tray, as a hex title id. Empty leaves
// the drive empty, which is what this port reported before.
REXCVAR_DEFINE_STRING(disc_title, "4D5307E6", "Games",
                      "Title id shown in the disc tray, or empty for no disc.");

namespace {

// "Nothing in the drive, take no action" -- see above.
constexpr uint32_t kMediaTypeNone = 0xFFFFFFFEu;

// An Xbox 360 game disc.
//
// Type 7 was tried first and is wrong: it made the tile read "Play CD", because
// 7 is optical audio/video media. The types that name a game are 1 and 2, and
// the tile builder at guest 0x922E1720 shows why -- only they reach the branch
// that formats a string with the disc's own title:
//
//     if ( sub_921FF2C0(v17) < 0 ) { ...generic... }
//     else { snwprintf(label, 0x104, dash_2946(131), v18); }   // "Play %s"
//
// and the worker at 0x921FF438 dispatches on the same value:
//
//     if ( v6 == 1 )      sub_921FF300(lock);   // read the disc
//     else if ( v6 == 2 ) sub_921FF028(lock);
//     else                v3 = 1627;            // unsupported -> error
//
// Type 1 is the one that reads a real disc: 0x921FF300 opens
// \Device\Cdrom0\default.xex, parses it, and copies the title information it
// finds into the block the label is built from. So the name on the tile is not
// something to supply -- it is something the dashboard works out for itself,
// given a disc to read. See MountDiscDevice in nxe_dash_app.h.
constexpr uint32_t kMediaTypeGameDisc = 1;

// The guest passes a DWORD[4] for the second out-parameter (0x922E15A8).
constexpr uint32_t kMediaInfoBytes = 16;

// The staged title being presented as a disc, or 0 for none.
uint32_t DiscTitleId() {
  const std::string text = REXCVAR_GET(disc_title);
  if (text.empty()) {
    return 0;
  }
  const uint32_t id = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
  // Only claim a disc for something actually installed; an empty drive is
  // better than a tile that cannot do anything when pressed.
  return nxe_game::PackagePathForTitle(id).empty() ? 0 : id;
}

u32 LoaderGetMediaInfo_entry(mapped_u32 media_type, mapped_void media_info) {
  const uint32_t title = DiscTitleId();
  const uint32_t type = title ? kMediaTypeGameDisc : kMediaTypeNone;

  if (media_type) {
    *media_type = type;
  }
  if (auto* info = media_info.as<uint8_t*>()) {
    std::memset(info, 0, kMediaInfoBytes);
    if (title) {
      // The block is four dwords and the guest reads it opaquely here; the
      // title id is the one field worth carrying, and zero elsewhere is what it
      // saw before.
      *reinterpret_cast<be<uint32_t>*>(info) = title;
    }
  }

  static uint32_t s_logged = 0xFFFFFFFFu;
  if (s_logged != type) {
    s_logged = type;
    if (title) {
      REXKRNL_INFO("XamLoaderGetMediaInfo -> game disc (type {}), title {:#010x}", type, title);
    } else {
      REXKRNL_INFO("XamLoaderGetMediaInfo -> no media ({:#010x})", kMediaTypeNone);
    }
  }
  return X_STATUS_SUCCESS;
}

// XamLoaderGetMediaInfoEx(type, info, extra)
//
// The tile reports through GetMediaInfo but *activates* through this one: guest
// dash_2948 reads the type from here and only reaches the launch when it says a
// game disc. It ships as a bare stub, so that read was an uninitialised
// register -- the same defect that made Play Game intermittent.
//
// Answered identically to GetMediaInfo, so the tile cannot disagree with itself
// about what is in the drive. The third value is only consulted for a different
// caller (a1 == 0x4000, checking for 5 or 10) and is left zero.
u32 LoaderGetMediaInfoEx_entry(mapped_u32 media_type, mapped_void media_info,
                               mapped_u32 extra) {
  const auto result = LoaderGetMediaInfo_entry(media_type, media_info);
  if (extra) {
    *extra = 0;
  }
  return result;
}

}  // namespace

extern "C" {
// The guest's own reader, replaced. Declared so the fallback path is available
// if it is ever wanted; the override below does not call it.
void __imp__sub_921FF2C0(PPCContext& __restrict ctx, uint8_t* base);
}

// sub_921FF2C0(out) -- the disc's title information.
//
// This is what names the tile. The builder at guest 0x922E1720 does:
//
//     if ( sub_921FF2C0(v17) < 0 ) { ...generic "Play Game"... }
//     else { snwprintf(label, 0x104, dash_2946(131), v18); }   // "Play %s"
//
// with v18 sitting 24 bytes into the block this fills, so the name the tile
// shows is a string at offset 24 of a 184-byte record.
//
// The guest's own way of producing that record is to load the disc's executable
// and read it: guest 0x921FF300 opens \Device\Cdrom0\default.xex and hands it
// to a full XEX loader -- NtAllocateVirtualMemory, XexTransformImageKey,
// XeCryptAesKey, then decompression -- before it ever gets to the title data.
// That is a retail XEX being decrypted, and none of that machinery exists in
// this port, so the read fails and the tile falls back to "Play Game". Making
// it work would mean implementing XEX decryption purely to recover a name this
// port already knows from the package header.
//
// So the record is answered directly, from the same source the Game Library and
// the storage browser use for the same title. The disc is real -- it is mounted
// at \Device\Cdrom0 and the game boots from it -- only the name lookup is
// short-circuited.
//
// Overriding a guest function only works when the call crosses a translation
// unit, and this one does: the reader is defined in nxe_dash_recomp.6.cpp while
// the tile builder that calls it is in .12.cpp, so the call goes through the
// symbol and lands here.
extern "C" void sub_921FF2C0(PPCContext& __restrict ctx, uint8_t* base) {
  constexpr uint32_t kRecordBytes = 184;
  constexpr uint32_t kNameOffset = 24;
  constexpr uint32_t kNoDiscInfo = 0x8007065Bu;  // 1627 | 0x80070000, the guest's own code

  // Only the tile asks this on our behalf.
  //
  // This reader has other callers, and answering all of them was a mistake: the
  // eligibility check at guest 0x9226E180 passes a different object and treats
  // a failure as "not eligible", so reporting success sent it down paths it had
  // never taken -- and the dashboard froze on the way back from the Game
  // Library. Everyone but the tile builder gets the original behaviour.
  //
  // The tile builder is 0x922E1720; its call returns into the middle of that
  // function, so the link register identifies it without needing to know the
  // exact call site.
  constexpr uint32_t kTileBuilderStart = 0x922E1720;
  constexpr uint32_t kTileBuilderEnd = 0x922E1904;  // start + 0x1E4

  // The tile's artwork is chosen in a second function, 0x922E19C0, and it is
  // gated on this same reader:
  //
  //     if ( !*(a1+36) && sub_921FF2C0(v13) >= 0 ) {
  //         snwprintf(a1+568, 0x104, L"memory://%x,%x", v14, v15);
  //         dash_28b9(a1+568, a1+36);
  //     }
  //
  // With only the builder answered, that call fell through to the original,
  // which has no disc state to report and fails -- so the branch never ran and
  // the tile never asked for a picture at all. The log showed it plainly: one
  // memory:// load in a whole session, and it was the gamerpic. Naming the tile
  // and picturing it are the same question, so both callers get the same
  // answer.
  //
  // dash_2a8c at 0x921FEBF0 reads it as well, and that one turns out to be the
  // picture that fills the tile. a1+36, loaded in 0x922E19C0, is the small
  // inset in the corner; a1+44, loaded here, is the full-tile art, and
  //
  //     if ( !*(a1+36) || *(a1+44) ) v8 = *(a1+40);
  //
  // is not the tile throwing artwork away -- it is the inset dropping back to
  // the plain disc picture once real art exists, which is what a console shows.
  constexpr uint32_t kTileArtStart = 0x922E19C0;
  constexpr uint32_t kTileArtEnd = 0x922E1B68;
  constexpr uint32_t kTileCoverStart = 0x921FEBF0;  // dash_2a8c
  constexpr uint32_t kTileCoverEnd = 0x921FED38;

  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const bool from_tile = (caller >= kTileBuilderStart && caller < kTileBuilderEnd) ||
                         (caller >= kTileArtStart && caller < kTileArtEnd) ||
                         (caller >= kTileCoverStart && caller < kTileCoverEnd);
  if (!from_tile) {
    __imp__sub_921FF2C0(ctx, base);
    return;
  }

  const uint32_t out = ctx.r3.u32;
  const uint32_t title = DiscTitleId();
  const auto name = title ? nxe_title::NameFor(title) : std::u16string();

  if (!out || name.empty()) {
    ctx.r3.u64 = kNoDiscInfo;
    return;
  }

  // What the record has to say before a picture is asked for.
  //
  // dash_2a8c does not simply trust this reader; it checks the record against
  // the title it was asked about and only then builds a URL:
  //
  //     if ( v8 == a1 && v9  ) { v6 = v10; ... }   // kind 2, offsets 168/172
  //     if ( v8 == a1 && v11 ) { v6 = v12; ... }   // kind 3, offsets 176/180
  //     if ( v6 ) { snwprintf(v13, 0x104, L"memory://%x,%x"); dash_28b9(...); }
  //
  // v8 is offset 12, the title id. With the record zeroed, offset 12 was 0, so
  // the comparison failed and it returned 0x80004005 without ever asking --
  // which is why no load from it appeared in the log. The image fields only
  // have to be non-zero to get past the guards; the URL built from them is
  // replaced anyway, in theme_trace.cpp.
  constexpr uint32_t kTitleIdOffset = 12;
  constexpr uint32_t kImageOffsets[] = {168, 172, 176, 180};

  auto* record = base + out;
  std::memset(record, 0, kRecordBytes);

  *reinterpret_cast<be<uint32_t>*>(record + kTitleIdOffset) = title;
  for (const uint32_t offset : kImageOffsets) {
    *reinterpret_cast<be<uint32_t>*>(record + offset) = 1;
  }

  // UTF-16, big-endian, NUL terminated -- what the guest's own wide printf
  // expects to find there. Bounded so a long title can neither run off the end
  // of the caller's buffer nor reach the image fields above.
  auto* text = reinterpret_cast<be<uint16_t>*>(record + kNameOffset);
  const size_t room = (kImageOffsets[0] - kNameOffset) / sizeof(uint16_t) - 1;
  const size_t count = name.size() < room ? name.size() : room;
  for (size_t i = 0; i < count; ++i) {
    text[i] = static_cast<uint16_t>(name[i]);
  }
  text[count] = 0;

  static uint32_t s_logged = 0;
  if (s_logged != title) {
    s_logged = title;
    REXKRNL_INFO("Disc title info: {:#010x} -> '{}'", title, rex::string::to_utf8(name));
  }
  ctx.r3.u64 = 0;
}

REX_EXPORT(__imp__XamLoaderGetMediaInfo, LoaderGetMediaInfo_entry)
REX_EXPORT(__imp__XamLoaderGetMediaInfoEx, LoaderGetMediaInfoEx_entry)
