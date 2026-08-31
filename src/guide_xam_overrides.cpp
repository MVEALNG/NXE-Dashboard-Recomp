// Overrides compiled into the recompiled Guide itself (nxe_dash_xam.dll).
//
// These cannot live in the dashboard executable. Recompiled code calls its own
// internal functions directly --
//
//     sub_816DD118(ctx, base);
//
// -- not through the dispatcher, so replacing the dispatch-table entry with
// FunctionDispatcher::SetFunction changes nothing for XAM's internal calls. It
// was tried, and the replacement was simply never reached.
//
// What does work is the weak-alias route this port already uses for the
// dashboard (see channel_trace.cpp): DEFINE_REX_FUNC(sub_X) emits the body as
// __imp__sub_X and makes sub_X a weak alias of it, so a strong sub_X linked into
// the same binary wins. This file is compiled into nxe_dash_xam.dll for exactly
// that reason.
//
// Everything here is a deliberate lie to XAM about state it would normally own
// itself, and each one is here because XAM's start-up -- which is what fills that
// state in -- segfaults in this process.

#include "nxe_dash_init.h"

#include <string>

#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// XAM's internal sign-in state, at guest 0x816DD118 in 2.0.17559.0.
//
// XamShowMessagesUI refuses with 5 before drawing anything, and the refusal is
// the first gate in sub_816C1718:
//
//     r25 = 5                              // the return value, set up front
//     ...
//     sub_816DD118(user_index)             // XAM's sign-in state
//     r3 = (result == 2) ? 1 : 0           // clz/rotate idiom: "is it exactly 2"
//     if (r3 == 0) -> return 5
//
// 2 is XUSER_SIGNIN_STATE_SIGNED_IN_TO_LIVE. The real function gets there by
// reading XAM's own user table at 0x81A228E0, a global only XAM's start-up ever
// fills in, so it reads as empty and XAM concludes nobody is signed in.
//
// Answering 2 directly is the same trade live_signin.cpp already makes for the
// dashboard, and it makes the two halves of this process agree about the user
// instead of disagreeing.
extern "C" void sub_816DD118(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = 2;  // XUSER_SIGNIN_STATE_SIGNED_IN_TO_LIVE
}

// XAM's allocator, at guest 0x816E67E8.
//
// With the sign-in gate satisfied, XAM stops refusing and starts actually
// building the UI -- and immediately fails allocating it. The return moved from
// 5 to 0xE, and 0xE is where sub_816C16C8 lands after translating 0x8007000E,
// E_OUTOFMEMORY, out of sub_816A2FF8:
//
//     sub_816E67E8(0x20180000, 0x10000000, size, &out)   // allocate
//     if (result < 0) -> E_OUTOFMEMORY
//
// The real function is a pure XAM-internal heap -- sub_816E5F10, sub_816E6070,
// sub_816E63D8, no kernel imports anywhere in it -- built over structures that
// only XAM's start-up fills in. Nothing can be handed to it from outside.
//
// So the allocation is served from the runtime's system heap instead, which is
// what that heap is for: guest-addressable memory for kernel-side structures.
// XAM gets a real, writable guest pointer and cannot tell the difference; what it
// loses is its own pooling and the tag in r3, neither of which anything here
// depends on.
//
//     r3 = tag/pool   r4 = flags   r5 = size   r6 = where to store the pointer
//
// Deliberately never freed. XAM's matching free path walks the same uninitialised
// heap structures, so handing these pointers back to it would be worse than
// leaking them, and the Guide is opened by hand a handful of times per run.
extern "C" void sub_816E67E8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t size = ctx.r5.u32;
  const uint32_t out = ctx.r6.u32;

  auto* memory = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->memory() : nullptr;
  const uint32_t address = (memory && size) ? memory->SystemHeapAlloc(size) : 0;

  static uint32_t s_logged = 0;
  if (++s_logged <= 8) {
    REXKRNL_INFO("Guide: XAM allocated {} byte(s) -> {:#010x}", size, address);
  }

  if (!address || !out) {
    ctx.r3.u64 = 0x8007000E;  // E_OUTOFMEMORY, which is what it would have said
    return;
  }

  uint8_t* slot = base + out;
  slot[0] = static_cast<uint8_t>(address >> 24);
  slot[1] = static_cast<uint8_t>(address >> 16);
  slot[2] = static_cast<uint8_t>(address >> 8);
  slot[3] = static_cast<uint8_t>(address);
  ctx.r3.u64 = 0;
}

// XAM's heap initialisation is NO LONGER skipped.
//
// It used to be, because it faulted -- but the fault was not the heap's doing:
//
//     0xC0000005, read of 0x181d82b60
//     [6] __imp__sub_8172DE40   [7] __imp__sub_816E6618
//
// With this run's guest base at 0x100000000 that address is guest 0x81D82B60,
// inside 0x81C00000+, which is where XAM's baked REX_LOOKUP_FUNC used to compute
// its dispatch table. sub_8172DE40 was making an indirect call and reading a
// table that was not there. Relocating XAM's REX_IMAGE_BASE/REX_IMAGE_SIZE to
// match the registered table fixed that, so the reason for skipping is gone.
//
// Skipping it was also doing far more damage than intended. sub_816E6618 creates
// EIGHT heaps and runs the CRT setup (sub_8172FC98, sub_8172FC50) that
// initialises XAM's locks -- and without those, start-up later deadlocked with
// two threads inside sub_8167E918 on RtlEnterCriticalSection(0x81A0132C), a
// critical section nothing had ever initialised.

// ExCreateThread, traced from inside the module.
//
// XAM's start-up now runs, allocates, and creates three threads -- and then
// stops. The last thing its thread ever logs is the third
// "[ExCreateThread] Guest is creating a system thread!", and none of the three
// threads it asked for ever reaches XThread::Execute. So either the call does
// not return, or it returns and XAM blocks immediately after waiting on threads
// that never ran.
//
// The log cannot separate those, so this wraps the call: entry and exit are
// logged around the runtime's own implementation, which is reached through
// GetProcAddress because this definition shadows the import for the module.
// Behaviour is otherwise unchanged.
extern "C" void __imp__ExCreateThread(PPCContext& __restrict ctx, uint8_t* base) {
  using GuestFunc = void (*)(PPCContext&, uint8_t*);
  static GuestFunc forward = [] () -> GuestFunc {
    HMODULE m = GetModuleHandleA("rexruntimed.dll");
    if (m == nullptr) m = GetModuleHandleA("rexruntime.dll");
    return m ? reinterpret_cast<GuestFunc>(
                   reinterpret_cast<void*>(GetProcAddress(m, "__imp__ExCreateThread")))
             : nullptr;
  }();

  // Signature: (handle_out, stack_size, tid_out, xapi_startup, start, context, flags)
  static uint32_t s_seq = 0;
  const uint32_t seq = ++s_seq;
  const uint32_t flags = ctx.r9.u32;

  REXKRNL_INFO("Guide: ExCreateThread #{} start={:#010x} context={:#x} flags={:#x}", seq,
               ctx.r7.u32, ctx.r8.u32, flags);

  // Drop the system-thread request.
  //
  // XAM asks for system threads, because on a console XAM *is* the system. The
  // runtime declines: its log shows the warning and then nothing -- neither the
  // "stack=... flags=..." line that precedes creation nor the "-> handle=... tid="
  // line that follows it -- and no XThread::Execute ever appears for the three
  // threads XAM asks for. The call still reports success, so XAM carries on and
  // then blocks forever waiting on threads that were never made.
  //
  // These are ordinary worker threads as far as this port is concerned; nothing
  // here depends on the privileges the flag asks for. Clearing it lets them be
  // created as normal guest threads, which is the only kind this runtime makes.
  // Drop the system-thread request, but leave CREATE_SUSPENDED alone.
  //
  // XAM asks for 0x83 = 0x80 | system(0x2) | suspended(0x1). The system bit is
  // the one the runtime refuses: its log shows the "Guest is creating a system
  // thread!" warning and then neither the "stack=... flags=..." line that
  // precedes creation nor the "-> handle=... tid=" line that follows it, while
  // still reporting success. So XAM carried on believing it had three threads
  // that were never made.
  //
  // Clearing CREATE_SUSPENDED as well was tried and is worse: the threads then
  // start at creation and ExCreateThread itself stops returning -- the third
  // call never logs its exit. Creating a running guest thread from inside a guest
  // call appears to deadlock in the runtime, so the suspended request is left as
  // XAM wrote it.
  constexpr uint32_t kCreateSystemThread = 0x2;
  if (flags & kCreateSystemThread) {
    ctx.r9.u64 = flags & ~kCreateSystemThread;
    REXKRNL_INFO("Guide: ExCreateThread #{} -- flags {:#x} -> {:#x} (system bit cleared)", seq,
                 flags, ctx.r9.u32);
  }

  if (forward == nullptr) {
    REXKRNL_WARN("Guide: ExCreateThread #{} -- runtime implementation not resolvable", seq);
    ctx.r3.u64 = 0x8000FFFF;
    return;
  }
  forward(ctx, base);
  REXKRNL_INFO("Guide: ExCreateThread #{} returned {:#x}", seq, ctx.r3.u32);
}

// What XAM's start-up waits on.
//
// With its threads created, XAM blocks immediately after the third one and never
// returns from its entry point. Something is waiting; these two are how guest
// code waits, so they are traced to say which object and for how long. Forwarded
// unchanged -- this only reports.
namespace {
using GuestFn = void (*)(PPCContext&, uint8_t*);
GuestFn RuntimeFn(const char* name) {
  HMODULE m = GetModuleHandleA("rexruntimed.dll");
  if (m == nullptr) m = GetModuleHandleA("rexruntime.dll");
  return m ? reinterpret_cast<GuestFn>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}
}  // namespace

extern "C" void __imp__KeWaitForSingleObject(PPCContext& __restrict ctx, uint8_t* base) {
  static GuestFn forward = RuntimeFn("__imp__KeWaitForSingleObject");
  static uint32_t s_n = 0;
  const uint32_t object = ctx.r3.u32;
  if (++s_n <= 12) {
    REXKRNL_INFO("Guide: KeWaitForSingleObject(object={:#x}) entering", object);
  }
  if (forward) forward(ctx, base);
  if (s_n <= 12) {
    REXKRNL_INFO("Guide: KeWaitForSingleObject(object={:#x}) -> {:#x}", object, ctx.r3.u32);
  }
}

extern "C" void __imp__NtWaitForSingleObjectEx(PPCContext& __restrict ctx, uint8_t* base) {
  static GuestFn forward = RuntimeFn("__imp__NtWaitForSingleObjectEx");
  static uint32_t s_n = 0;
  const uint32_t handle = ctx.r3.u32;
  if (++s_n <= 12) {
    REXKRNL_INFO("Guide: NtWaitForSingleObjectEx(handle={:#x}) entering", handle);
  }
  if (forward) forward(ctx, base);
  if (s_n <= 12) {
    REXKRNL_INFO("Guide: NtWaitForSingleObjectEx(handle={:#x}) -> {:#x}", handle, ctx.r3.u32);
  }
}

// XAM's entry point, wrapped so its execution is provable.
//
// Whether XAM's start-up thread ever runs could not be settled from the logs:
// the "XThread::Execute thid ..." line belongs to XHostThread and never appears
// for guest threads, and the guest equivalent is REXSYS_NOISY_DEBUG, which may
// not be compiled in at all. Twice now a conclusion has been drawn from a
// missing log line and been wrong, so this logs from inside XAM instead, where
// there is no ambiguity: if this line appears, XAM's entry point ran.
//
// Pass-through: __imp__xstart is the real body.
extern "C" void __imp__xstart(PPCContext& __restrict ctx, uint8_t* base);

// Whether XAM has been through DLL_PROCESS_ATTACH, published for the dashboard.
//
// This file is compiled into nxe_dash_xam.dll, not the executable, so it cannot
// call into guide_bridge.cpp -- the link fails on an undefined symbol. The flag
// is exported instead and the dashboard reads it through GetProcAddress, which
// is the same way it already reaches the runtime's own exports.
namespace {
volatile long g_xam_started_flag = 0;
}  // namespace

extern "C" __declspec(dllexport) int nxe_guide_xam_started() { return g_xam_started_flag != 0; }

extern "C" void xstart(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: XAM xstart ENTERED (r3={:#x} r4={:#x})", ctx.r3.u32, ctx.r4.u32);
  __imp__xstart(ctx, base);
  REXKRNL_INFO("Guide: XAM xstart returned {:#x}", ctx.r3.u32);
  g_xam_started_flag = 1;
}

// KeWaitForMultipleObjects, traced for the dispatch-header type.
//
// A XAM worker thread trips an assert inside the runtime:
//
//     rex::system::XObject::GetNativeObject      xobject.cpp:439
//       <- KeWaitForMultipleObjects_entry        xboxkrnl_threading.cpp:882
//
// GetNativeObject only builds Event (0, 1), Mutant (2) and Semaphore (5) from a
// raw dispatch header; every other type falls into `default: assert_always()`,
// which is EXCEPTION_BREAKPOINT and takes the process with it.
//
// Which type XAM actually waits on decides what to do about it, and the type is
// the first byte of each object's X_DISPATCH_HEADER. Logged here rather than by
// patching the SDK, so the answer costs nothing.
extern "C" void __imp__KeWaitForMultipleObjects(PPCContext& __restrict ctx, uint8_t* base) {
  static GuestFn forward = RuntimeFn("__imp__KeWaitForMultipleObjects");

  const uint32_t count = ctx.r3.u32;
  const uint32_t objects_ptr = ctx.r4.u32;
  static uint32_t s_n = 0;
  if (++s_n <= 6 && objects_ptr) {
    for (uint32_t i = 0; i < count && i < 8; ++i) {
      const uint8_t* slot = base + objects_ptr + i * 4;
      const uint32_t obj = (uint32_t(slot[0]) << 24) | (uint32_t(slot[1]) << 16) |
                           (uint32_t(slot[2]) << 8) | slot[3];
      const uint8_t type = obj ? base[obj] : 0xFF;
      REXKRNL_INFO("Guide: KeWaitForMultipleObjects[{}/{}] object={:#010x} dispatch type={}", i,
                   count, obj, type);
    }
  }
  if (forward) forward(ctx, base);
}

// Checkpoints through XAM's main init, sub_816968A8.
//
// Start-up now runs deep -- 24 threads, no crash -- but xstart never returns and
// the per-user UI context stays empty, so whatever finishes initialisation has
// not run. The call sequence of sub_816968A8 is known from the generated source;
// these wrap it at intervals so one run says which call is entered and never
// left. Absent log lines have misled this investigation three times, so the exit
// line matters as much as the entry.
//
// Pass-through only.

extern "C" void __imp__sub_8168B920(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816FED30(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816FF008(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_81699DF0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816BA650(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816C9EA0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816A17B8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816A95A0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8167ED90(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816C0698(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816A45F8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816DB788(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816F44D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_81696808(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_81696280(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_81710D18(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_81778F68(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816BB468(PPCContext& __restrict ctx, uint8_t* base);

extern "C" void sub_8168B920(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_8168B920");
  __imp__sub_8168B920(ctx, base);
  REXKRNL_INFO("Guide: init << sub_8168B920 done");
}
extern "C" void sub_816FED30(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816FED30");
  __imp__sub_816FED30(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816FED30 done");
}
extern "C" void sub_816FF008(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816FF008");
  __imp__sub_816FF008(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816FF008 done");
}
extern "C" void sub_81699DF0(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_81699DF0");
  __imp__sub_81699DF0(ctx, base);
  REXKRNL_INFO("Guide: init << sub_81699DF0 done");
}
extern "C" void sub_816BA650(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816BA650");
  __imp__sub_816BA650(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816BA650 done");
}
extern "C" void sub_816C9EA0(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816C9EA0");
  __imp__sub_816C9EA0(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816C9EA0 done");
}
extern "C" void sub_816A17B8(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816A17B8");
  __imp__sub_816A17B8(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816A17B8 done");
}
extern "C" void sub_816A95A0(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816A95A0");
  __imp__sub_816A95A0(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816A95A0 done");
}
extern "C" void sub_8167ED90(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_8167ED90");
  __imp__sub_8167ED90(ctx, base);
  REXKRNL_INFO("Guide: init << sub_8167ED90 done");
}
extern "C" void sub_816C0698(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816C0698");
  __imp__sub_816C0698(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816C0698 done");
}
extern "C" void sub_816A45F8(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816A45F8");
  __imp__sub_816A45F8(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816A45F8 done");
}
extern "C" void sub_816DB788(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816DB788");
  __imp__sub_816DB788(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816DB788 done");
}
extern "C" void sub_816F44D0(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816F44D0");
  __imp__sub_816F44D0(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816F44D0 done");
}
extern "C" void sub_81696808(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_81696808");
  __imp__sub_81696808(ctx, base);
  REXKRNL_INFO("Guide: init << sub_81696808 done");
}
extern "C" void sub_81696280(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_81696280");
  __imp__sub_81696280(ctx, base);
  REXKRNL_INFO("Guide: init << sub_81696280 done");
}
extern "C" void sub_81710D18(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_81710D18");
  __imp__sub_81710D18(ctx, base);
  REXKRNL_INFO("Guide: init << sub_81710D18 done");
}
extern "C" void sub_81778F68(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_81778F68");
  __imp__sub_81778F68(ctx, base);
  REXKRNL_INFO("Guide: init << sub_81778F68 done");
}
extern "C" void sub_816BB468(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: init >> sub_816BB468");
  __imp__sub_816BB468(ctx, base);
  REXKRNL_INFO("Guide: init << sub_816BB468 done");
}

// Inside sub_8167ED90, where start-up stops.
//
// Its callees, so the hang can be narrowed one more level. DrvSetContentStorageCallback
// and KeSetEvent are runtime imports and are left alone; these five are XAM's own.

extern "C" void __imp__sub_81722A30(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816BA440(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816D1AB0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8167E918(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_816B7B80(PPCContext& __restrict ctx, uint8_t* base);

extern "C" void sub_81722A30(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: hang >> sub_81722A30");
  __imp__sub_81722A30(ctx, base);
  REXKRNL_INFO("Guide: hang << sub_81722A30 done");
}
extern "C" void sub_816BA440(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: hang >> sub_816BA440");
  __imp__sub_816BA440(ctx, base);
  REXKRNL_INFO("Guide: hang << sub_816BA440 done");
}
extern "C" void sub_816D1AB0(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: hang >> sub_816D1AB0");
  __imp__sub_816D1AB0(ctx, base);
  REXKRNL_INFO("Guide: hang << sub_816D1AB0 done");
}
extern "C" void sub_8167E918(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: hang >> sub_8167E918");
  __imp__sub_8167E918(ctx, base);
  REXKRNL_INFO("Guide: hang << sub_8167E918 done");
}
extern "C" void sub_816B7B80(PPCContext& __restrict ctx, uint8_t* base) {
  REXKRNL_INFO("Guide: hang >> sub_816B7B80");
  __imp__sub_816B7B80(ctx, base);
  REXKRNL_INFO("Guide: hang << sub_816B7B80 done");
}

// XAM's static initialisers, run one at a time with the bad ones refused.
//
// This is where XAM's start-up dies. The chain is
//
//     xstart -> sub_816968A8 (DLL_PROCESS_ATTACH) -> sub_816E6618 -> sub_8172DE40
//
// and sub_8172DE40 is the C++ global-constructor walk. It is very small:
//
//     lis  r11, -32340          ; r11 = 0x81AC0000
//     lwz  r10, 27528(r11)      ; one pointer at 0x81AC6B88
//     cmplwi r10, 0 ; beq       ; skipped when null
//     bctrl                     ; else called
//     ...
//     addi r10, r10, 572        ; table start 0x81A8023C
//     addi r30, r11, 580        ; table end   0x81A80244
//     lwz  r11, 0(r31) ; bctrl  ; each non-null entry, r31 += 4
//
// -- three function pointers in total. One of them does not point into XAM.
// REX_CALL_INDIRECT_FUNC falls back to ResolveIndirectFunction for a target
// outside the module's own code range, which searches globally, so the call
// lands in the dashboard's image instead of failing: the crash stack has XAM
// frames calling dashboard functions (sub_92473168, sub_9246E598), and an older
// run named it outright -- "Call to invalid or unregistered function at guest
// address 0x92478898".
//
// So the walk is done here instead, with the same semantics as the original --
// skip nulls, stop early if a constructor returns non-zero -- and one addition:
// a pointer outside XAM's own code is reported and skipped rather than
// dispatched into another module's image, which is never what a constructor
// table meant.
extern "C" void sub_8172DE40(PPCContext& __restrict ctx, uint8_t* base) {
  constexpr uint32_t kSinglePointer = 0x81AC6B88;
  constexpr uint32_t kTableStart = 0x81A8023C;
  constexpr uint32_t kTableEnd = 0x81A80244;

  // XAM's recompiled code, as reported at registration:
  //   "Function table initialized for module: code=81670000-81A76A88"
  constexpr uint32_t kXamCodeLow = 0x81670000;
  constexpr uint32_t kXamCodeHigh = 0x81A76A88;

  auto* kernel_state = REX_KERNEL_STATE();
  auto* dispatcher = kernel_state ? kernel_state->function_dispatcher() : nullptr;
  if (dispatcher == nullptr) {
    REXKRNL_WARN("Guide: no dispatcher for XAM's initialisers; skipping them all");
    ctx.r3.u64 = 0;
    return;
  }

  const auto read = [&](uint32_t address) -> uint32_t {
    return *reinterpret_cast<const rex::be<uint32_t>*>(base + address);
  };

  int ran = 0;
  int skipped = 0;
  const auto run = [&](uint32_t target) -> bool {
    if (target == 0) {
      return true;  // a null slot is normal and simply skipped
    }
    if (target < kXamCodeLow || target >= kXamCodeHigh) {
      ++skipped;
      REXKRNL_WARN("Guide: XAM initialiser {:#010x} is outside XAM's code "
                   "({:#010x}-{:#010x}); refusing to dispatch it",
                   target, kXamCodeLow, kXamCodeHigh);
      return true;
    }
    auto* fn = dispatcher->GetFunction(target);
    if (fn == nullptr) {
      ++skipped;
      REXKRNL_WARN("Guide: XAM initialiser {:#010x} has no recompiled function; skipped",
                   target);
      return true;
    }
    fn(ctx, base);
    ++ran;
    // The original stops the walk as soon as one returns non-zero.
    return ctx.r3.u32 == 0;
  };

  ctx.r3.u64 = 0;
  bool carry_on = run(read(kSinglePointer));
  for (uint32_t slot = kTableStart; carry_on && slot < kTableEnd; slot += 4) {
    carry_on = run(read(slot));
  }

  REXKRNL_WARN("Guide: XAM initialisers: {} run, {} skipped, result {:#x}", ran, skipped,
               ctx.r3.u32);
}

// A directory opened with a trailing separator, which the VFS will not resolve.
//
// XAM's start-up enumerates the storage devices and stops doing it:
//
//     Guide: hang >> sub_8167E918
//     [NtCreateFile] FAILED: path='\Device\Harddisk0\Partition1\' -> 0xc000000f
//
// The device is mounted -- the same run logs "Mounted ...nxe_dash_gamedir at
// \Device\Harddisk0\Partition1" -- so the mount is fine and the trailing
// backslash is not: the runtime hands the path to the VFS verbatim and the empty
// final component makes the resolve miss, so XAM is told the device root does
// not exist and carries on with a handle it never got.
//
// This lives here rather than in guide_bridge.cpp because that file is the
// executable, and XAM's kernel imports resolve against rexruntimed.dll from
// inside this DLL -- an override in the executable is simply never consulted.
// The first attempt was written there, bound correctly according to nxe_dash.map,
// and changed nothing at all for XAM.
//
// The two spellings name the same directory, so the separator is dropped for the
// call and the caller's ANSI_STRING restored afterwards; it is XAM's own buffer
// and may be a constant it reuses.
//
// NtCreateFile(handle_out, desired_access, object_attrs, ...) -- r5 is
// object_attrs, whose name_ptr at +4 is an X_ANSI_STRING {length,
// maximum_length, pointer}.
extern "C" void __imp__NtCreateFile(PPCContext& __restrict ctx, uint8_t* base) {
  using GuestFunc = void (*)(PPCContext&, uint8_t*);
  static GuestFunc forward = [] () -> GuestFunc {
    HMODULE m = GetModuleHandleA("rexruntimed.dll");
    if (m == nullptr) m = GetModuleHandleA("rexruntime.dll");
    return m ? reinterpret_cast<GuestFunc>(
                   reinterpret_cast<void*>(GetProcAddress(m, "__imp__NtCreateFile")))
             : nullptr;
  }();
  if (forward == nullptr) {
    REXKRNL_WARN("Guide: NtCreateFile has no runtime implementation to forward to");
    ctx.r3.u64 = 0;
    return;
  }

  rex::be<uint16_t>* length_field = nullptr;
  uint16_t original_length = 0;

  const uint32_t attrs = ctx.r5.u32;
  if (attrs) {
    const uint32_t name_ptr = *reinterpret_cast<const rex::be<uint32_t>*>(base + attrs + 4);
    if (name_ptr) {
      auto* length = reinterpret_cast<rex::be<uint16_t>*>(base + name_ptr);
      const uint32_t text = *reinterpret_cast<const rex::be<uint32_t>*>(base + name_ptr + 4);
      uint16_t len = *length;

      // Say what is actually arriving, before deciding anything about it.
      //
      // The previous build logged nothing here at all, which leaves two very
      // different explanations -- the override is not being consulted, or it is
      // and the test is wrong -- and no way to tell them apart. length may or
      // may not count a NUL, so the last byte is reported raw.
      static int s_seen = 0;
      if (s_seen < 12 && text) {
        ++s_seen;
        REXKRNL_WARN("Guide: NtCreateFile sees '{}' (len={}, last byte {:#04x})",
                     std::string(reinterpret_cast<const char*>(base + text), len), len,
                     len ? *(base + text + len - 1) : 0);
      }

      // Trailing NULs are not part of the name; length has been seen to count
      // them, which would hide the separator behind a zero byte.
      while (len > 1 && *(base + text + len - 1) == 0) {
        --len;
      }

      // 0x5C is a backslash, written numerically so the literal cannot be mangled.
      if (text && len > 1 && *(base + text + len - 1) == 0x5C) {
        length_field = length;
        original_length = len;
        *length = static_cast<uint16_t>(len - 1);

        static int s_logged = 0;
        if (s_logged < 4) {
          ++s_logged;
          REXKRNL_WARN("Guide: NtCreateFile dropping the trailing separator from '{}'",
                       std::string(reinterpret_cast<const char*>(base + text), len));
        }
      }
    }
  }

  forward(ctx, base);

  if (length_field) {
    *length_field = original_length;
  }
}
