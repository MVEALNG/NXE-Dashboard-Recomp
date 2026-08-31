// Startup dialogs that describe hardware or services this port does not have.
//
// With everything else in place -- the console reporting signed in to LIVE, the
// service lookup answered, the party list fixed -- the account screens work:
// Account Management, Windows Live ID, Microsoft Points and Memberships all
// render and navigate, and Network Settings reports Xbox LIVE: Connected. The
// one thing still in the way was a modal error box thrown by whatever LIVE
// content the screen could not fetch.
//
// Three specific messages are suppressed here, listed and justified one by one
// at IsSuppressed below. Everything else is forwarded and still shows.
//
// Why this is a suppression and not a fix
// ---------------------------------------
// The dialog is raised because the dashboard asks the LIVE service for real
// files and cannot have them. The account screen fetches, for example:
//
//     GET http://127.0.0.1:1000/xedl/ProfileInfo/103/CountryInfo.cib
//
// There is no service to serve that, and synthesising its contents would mean
// inventing a binary payload for a parser whose format is not known here -- the
// kind of guess that has already misfired twice in this port. xhttp.cpp answers
// such requests 404, which is true, and the dashboard mostly copes: it has its
// own softer "Some Xbox LIVE content is temporarily unavailable. You can still
// access your profile..." message for exactly this case. This one box is the
// place it does not cope, so this stops the box rather than pretending the file
// arrived. Nothing downstream is told the fetch succeeded.
//
// Everything else still shows
// ---------------------------
// A blanket suppression would also swallow legitimate dialogs -- "Set Pass Code"
// and the rest come through the same entry point. So this matches on the message
// text and hands every other dialog to the runtime's own implementation, which is
// the thing that actually draws it.
//
// Reaching that implementation takes one step, because REX_EXPORT here overrides
// it: this file defines __imp__XamShowMessageBoxUI, so the runtime's copy is no
// longer reachable by name from the exe. It is still exported from
// rexruntimed.dll, so it is fetched once with GetProcAddress and called with the
// SAME PPCContext and base. That matters -- XamShowMessageBoxUI takes nine
// arguments, and the ninth (the overlapped) is past r10 and lives on the guest
// stack. Forwarding the untouched context carries every argument through
// exactly as the guest laid them out, with nothing re-marshalled and no ABI
// assumption of my own.
//
// If the forward cannot be resolved the dialog is suppressed rather than lost
// silently, and that is logged once so it is visible rather than mysterious.

#include <cstdint>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace rex;

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

// The runtime's own implementation, still exported by the DLL.
GuestFunc RuntimeMessageBox() {
  static GuestFunc fn = [] () -> GuestFunc {
    HMODULE module = GetModuleHandleA("rexruntimed.dll");
    if (module == nullptr) {
      module = GetModuleHandleA("rexruntime.dll");
    }
    if (module == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<GuestFunc>(
        reinterpret_cast<void*>(GetProcAddress(module, "__imp__XamShowMessageBoxUI")));
  }();
  return fn;
}

uint32_t GuestBe32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Guest strings are UTF-16 big-endian.
std::string GuestWide(uint8_t* base, uint32_t addr, size_t max_chars = 220) {
  std::string out;
  if (!addr) {
    return out;
  }
  const uint8_t* p = base + addr;
  for (size_t i = 0; i < max_chars; ++i) {
    const uint16_t ch = (uint16_t(p[i * 2]) << 8) | p[i * 2 + 1];
    if (ch == 0) break;
    out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
  }
  return out;
}

// Matched on the message itself, so each suppression is a deliberate decision
// about one message rather than a blanket silencing.
//
//   "Can't connect to Xbox LIVE"
//       The account screens cannot be reached past it. See above.
//
//   "Some Xbox LIVE content is temporarily unavailable"
//       True, and harmless -- but it is the standing consequence of a LIVE that
//       is simulated locally and has no content behind it, so it would appear on
//       every boot forever. Suppressed at the user's request.
//
//   "Update required. Insert the disc that came with your device."
//       This one is not about LIVE at all. It comes from the device-update check
//       at guest 0x92140828, reached from startup at 0x921433E8:
//
//           if ( dword_92800F48 )
//               sub_92141968(&unk_92800DF8);
//
//       That global reads 1 and is never written anywhere in the image -- the
//       only reference to it is that read -- so it ships set and the check always
//       runs. And once it runs a dialog is unavoidable: 0x92140828 shows one on
//       BOTH branches, keyed on whether the overlapped comes back with
//       ERROR_CANCELLED (1223). So no return value from
//       XamContentDeviceCheckUpdates can prevent it; the message can only be
//       stopped here. It is describing an optical drive this port does not have.
bool IsSuppressed(const std::string& text) {
  return text.find("connect to Xbox LIVE") != std::string::npos ||
         text.find("temporarily unavailable") != std::string::npos ||
         text.find("Insert the disc that came with your device") != std::string::npos;
}

}  // namespace

REX_HOOK_RAW(__imp__XamShowMessageBoxUI) {
  const std::string title = GuestWide(base, ctx.r4.u32, 64);
  const std::string text = GuestWide(base, ctx.r5.u32);

  if (!IsSuppressed(text)) {
    if (auto* forward = RuntimeMessageBox()) {
      forward(ctx, base);  // untouched context: all nine arguments carry through
      return;
    }
    static bool s_warned = false;
    if (!s_warned) {
      s_warned = true;
      REXKRNL_WARN("XamShowMessageBoxUI: runtime implementation not resolvable; dialogs suppressed");
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
    return;
  }

  static uint32_t s_suppressed = 0;
  if (++s_suppressed <= 2) {
    REXKRNL_INFO("Suppressed dialog '{}': describes hardware or a service this port does not have",
                 title);
  }

  // Answer as the focused button, which is what the runtime does when it has no
  // way to present the box.
  if (ctx.r10.u32) {
    uint8_t* result = base + ctx.r10.u32;
    const uint32_t button = ctx.r8.u32;
    result[0] = static_cast<uint8_t>(button >> 24);
    result[1] = static_cast<uint8_t>(button >> 16);
    result[2] = static_cast<uint8_t>(button >> 8);
    result[3] = static_cast<uint8_t>(button);
  }

  // And complete the overlapped. This is the part that was missing, and leaving
  // it out did real damage: a suppressed dialog answered its caller but never
  // finished the request, so anything waiting on it simply stopped. The LIVE
  // service lookup (XLIVEBASE 0x58007) is downstream of one of these, and it
  // vanished from the logs the moment the "temporarily unavailable" message
  // started being suppressed -- which is what took the Marketplace, Friends and
  // Inside Xbox tabs with it.
  //
  // The overlapped is the ninth argument, so it is not in a register. It sits in
  // the caller's parameter save area, and the offset is read off a call site
  // rather than assumed -- the thunk at guest 0x921468D0 forwards its own ninth
  // argument into exactly that slot before calling:
  //
  //     lwz  r11, 0x60+arg_54(r1)      ; its ninth argument
  //     stw  r11, 0x60+var_C(r1)       ; -> r1 + 0x54, the callee's ninth slot
  //     bl   XamShowMessageBoxUI
  //
  // so on entry it is at r1 + 0x54.
  const uint32_t overlapped = GuestBe32(base, ctx.r1.u32 + 0x54);
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    ctx.r3.u64 = X_ERROR_IO_PENDING;
    return;
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
