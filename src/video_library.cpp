// Why the Video Library slot does nothing.
//
// The slot's onclick in homepage.xzp is
//
//     <cmd>EcNavToVideoLibrary</cmd>
//
// which the command table at 0x920288A0 maps to id 5, and the jump table at
// 0x92028AD0 sends id 5 to 0x922D3204:
//
//     li r5, 0 ; li r4, 0 ; mr r3, r30 ; bl sub_92241F18
//
// so the whole thing is sub_92241F18. Its first act is a gate:
//
//     v8 = sub_92241E20();
//     if ( v8 < 0 ) goto LABEL_28;      // -> dash_2966(a1, 16), and nothing opens
//
// which is exactly the reported behaviour: the button is pressed, the shell
// declines, and the dashboard carries on as if nothing happened.
//
// sub_92241E20 builds the media context in dword_92813E80 and can fail six
// different ways, four of them returning 0x8007000E and one 0x80004005:
//
//     if ( !dword_92813E80 ) {
//       v3 = sub_92240770(2728) ? sub_92246628() : 0;   // context
//       if ( !v3 ) return 0x8007000E;
//       v4 = sub_92144098(220);                          // 220-byte object
//       v5 = v4 ? sub_92247EC8(v4, a1) : 0;
//       if ( !v5 ) return 0x8007000E;
//       if ( !vtable_call(...) ) return 0x80004005;
//       if ( sub_922E8718(...) == -1 ) return 0x8007000E;
//     }
//
// Guessing between them is what this avoids. sub_92241F18's return says which
// group failed, and sub_922E8718 says whether it got as far as the last step --
// it and sub_92144098 live in other translation units, so unlike the rest of
// the chain they can be watched from here.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

using namespace rex;

extern "C" void nxe_theme_trace_scope(int delta);

extern "C" {

void __imp__sub_92241F18(PPCContext& __restrict ctx, uint8_t* base);  // video library entry
void __imp__sub_922E8718(PPCContext& __restrict ctx, uint8_t* base);  // last step of the gate

// EcNavToVideoLibrary lands here.
void sub_92241F18(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_WARN("[video] EcNavToVideoLibrary: entering (r3={:#010x} r4={:#x} r5={:#x})",
               ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);

  // The media context, before and after. Non-zero on entry means the gate has
  // already run once and will be skipped, which changes what a failure means.
  const uint32_t context_before = *reinterpret_cast<const be<uint32_t>*>(base + 0x92813E80);

  // The gate turned out to be fine -- it builds the context and the last step
  // returns 0 -- and the failure is further in, at 0x8007065B. What follows the
  // gate is a scene load:
  //
  //     v8 = sub_92241A88(dash_2723(&unk_92013834));
  //     v8 = dash_293d(0, word_92002030, 0, v23, 0x80);   // build the locator
  //     v8 = dash_280c(v23, &unk_92013800, v20, &v18);    // load the .xur
  //
  // and theme_trace.cpp already logs the last two, so its tracing is borrowed
  // for the duration rather than instrumenting them a second time here.
  nxe_theme_trace_scope(1);
  __imp__sub_92241F18(ctx, base);
  nxe_theme_trace_scope(-1);

  const int32_t result = static_cast<int32_t>(ctx.r3.u32);
  const uint32_t context_after = *reinterpret_cast<const be<uint32_t>*>(base + 0x92813E80);
  REXKRNL_WARN("[video] EcNavToVideoLibrary -> {:#x}  (context {:#010x} -> {:#010x})",
               static_cast<uint32_t>(result), context_before, context_after);

  if (result < 0) {
    switch (static_cast<uint32_t>(result)) {
      case 0x8007000Eu:
        REXKRNL_WARN("[video] that is the out-of-memory arm: the context, the 220-byte "
                     "object, or the last step failed");
        break;
      case 0x80004005u:
        REXKRNL_WARN("[video] that is the vtable check on the media object");
        break;
      default:
        break;
    }
  }
}

// The last thing the gate does before succeeding.
void sub_922E8718(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_922E8718(ctx, base);

  static int s_logged = 0;
  if (s_logged < 6) {
    ++s_logged;
    REXKRNL_WARN("[video] gate reached its last step, sub_922E8718 -> {:#x}", ctx.r3.u32);
  }
}

}  // extern "C"
