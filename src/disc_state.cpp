// Disc tray and media state.
//
// This is the fix for the intermittent crash that has been in this port from the
// start: an access violation reading guest address 5, inside the async loader
// manager's destructor at guest 0x9227C750.
//
// The destructor ends with a virtual call:
//
//     lwz  r3, 0x24(r30)      ; the object parked at +36
//     lwz  r11, 0(r3)         ; its vtable
//     lwz  r11, 4(r11)        ; vtable[1]      <-- faults when the vtable is 1
//     bctrl
//
// The parked object's vtable read as 1, so vtable[1] resolved to address 5. The
// object itself was fine when constructed -- guest 0x922772F8 sets
// *(obj) = off_92021824 and *(obj + 72) = 1 -- and its reference counting was
// watched end to end and is perfectly balanced (1 -> 2 -> 3 -> 2 -> 1 -> 0, one
// destruct). Nothing was double-freed. The vtable was simply overwritten with 1
// between construction and first use, and the culprit was this file's subject.
//
// XamLoaderGetDvdTrayState takes no arguments
// -------------------------------------------
// It reports the tray state as its RETURN value. The runtime declared it as
//
//     u32 XamLoaderGetDvdTrayState_entry(mapped_u32 out_state) {
//       if (out_state) *out_state = 1;
//       return X_STATUS_SUCCESS;
//     }
//
// which invents an out-parameter that does not exist, then writes 1 through
// whatever r3 happens to hold. The call site at guest 0x92276AF0 shows exactly
// what r3 holds:
//
//     mr   r31, r3                    ; r31 = the object, straight from arg 0
//     lwz  r11, 0x210(r31)
//     bl   XamLoaderGetDvdTrayState   ; r3 is NEVER reloaded -- still the object
//     mr   r30, r3                    ; and the state is taken from the return
//
// So the "out parameter" was the caller's own this-pointer, and the write landed
// on offset 0 of that object: its vtable, set to 1. That is the 1, and the
// 0x100000005 fault, and why the crash only became reliable once the Game
// Library started working -- this path is reached through the async loader
// subsystem, which only spins up when there is content to load.
//
// Taking no arguments and returning the value writes nothing at all, which is
// the whole fix.
//
// What it reports
// ---------------
// The guest tests the state against 0 before looking at the media:
//
//     bl    XamLoaderGetDvdTrayState
//     mr    r30, r3
//     ...
//     cmplwi cr6, r30, 0        ; DVD_TRAY_STATE_EMPTY
//     bne   skip
//
// There is no optical drive backing this port, so the tray is empty and 0 is the
// true answer rather than a convenient one.
//
// Why this was left alone before
// ------------------------------
// media_loader.cpp already records this defect and deliberately did not fix it,
// on the grounds that correcting the signature meant changing something the rest
// of the runtime shares, and that with its media type reporting -2 the caller it
// had examined (guest 0x922E1694) no longer reached the tray branch anyway.
//
// That reasoning was right about the branch and wrong about the danger. The harm
// is not which branch the RETURN value selects -- it is the WRITE through the
// out-parameter that does not exist, which lands on a different caller's object
// at guest 0x92276AF0 and has nothing to do with any tray branch. And correcting
// it needs no runtime change at all: an exe-side REX_EXPORT overrides the
// runtime's, the same way every other fix in this port does.
//
// XamLoaderGetMediaInfo is left to media_loader.cpp, which already answers it.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

using namespace rex;

namespace {

// The guest compares the returned state against 0 to mean "empty".
constexpr uint32_t kDvdTrayStateEmpty = 0;

// No arguments. Writing through r3 here is what corrupted the caller.
u32 XamLoaderGetDvdTrayState_entry() {
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamLoaderGetDvdTrayState -> {} (no optical drive)", kDvdTrayStateEmpty);
  }
  return kDvdTrayStateEmpty;
}

}  // namespace

REX_EXPORT(__imp__XamLoaderGetDvdTrayState, XamLoaderGetDvdTrayState_entry)
