// What kind of title an id refers to.
//
// XamIsXbox1TitleId ships as a bare REX_EXPORT_STUB, so it logs and leaves r3
// undefined. Three call sites branch on it, and one of them is the game detail
// panel in the library, at guest 0x922E7078:
//
//     if ( XamIsXbox1TitleId(*(a1 + 52)) )      // the title id
//     {
//         sub_92270ED8(*(a1 + 32), 0);          // hide a control
//         *(a1 + 248) = 1;
//         sub_922DF968(*(a1 + 44), 0);          // hide another
//         ... format the name through a different string ...
//     }
//     else if ( *(a1 + 244) ) { ...the ordinary 360 path... }
//
// So whether a game renders as a 360 title or as an original-Xbox one was
// decided by whatever happened to be in a register. The other two sites use it
// the same way, as a veto: guest 0x9226E180 returns 0 outright if it is true.
//
// The answer here is not a guess, and it is not a placeholder.
//
// This port runs one recompiled Xbox 360 executable. There is no original-Xbox
// emulation in it, no backward-compatibility layer for one, and no way for an
// Xbox 1 package to be installed on the storage device -- the content the
// enumerators can see is what is staged under the 360 content layout. So no
// title id this dashboard will ever be asked about refers to an Xbox 1 title,
// and "no" is the true answer for every one of them, not a convenient default.
//
// Being wrong in the other direction would be worse than useless: claiming a
// title is an Xbox 1 game sends the dashboard down a path this port cannot
// follow. What matters most is that the answer is now the same every time,
// rather than differing run to run with the contents of r3.
//
// If original-Xbox support is ever added, this is where the real test goes.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

using namespace rex;

namespace {

u32 XamIsXbox1TitleId_entry(u32 title_id) {
  static uint32_t s_reported = 0xFFFFFFFFu;
  if (s_reported != title_id) {
    s_reported = title_id;
    REXKRNL_INFO("XamIsXbox1TitleId({:#010x}) -> no (this port runs 360 titles only)", title_id);
  }
  return 0;
}

}  // namespace

REX_EXPORT(__imp__XamIsXbox1TitleId, XamIsXbox1TitleId_entry)
