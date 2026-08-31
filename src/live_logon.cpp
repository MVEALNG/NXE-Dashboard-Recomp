// Signing on to Xbox LIVE.
//
// This is the piece that was missing behind "Disconnected / Xbox LIVE" on the
// gamercard, and behind the Marketplace, Friends and Inside Xbox tabs never
// appearing.
//
// Those tabs are channels, and the channels themselves were never the problem --
// the manifest parses cleanly on every run and eight channels are created every
// time (see channel_trace.cpp). They are built and then not shown, because the
// dashboard only surfaces the LIVE channels once it believes it is connected.
//
// And it never believes that, because signing in and being connected are two
// different things:
//
//     signin_state (live_signin.cpp)   "this user is signed in to LIVE"
//     the logon                        "this console has a live connection"
//
// The first was answered. The second could not succeed, because the entire logon
// surface ships as bare REX_EXPORT_STUBs -- XamUserLogon, XNetLogonTaskStart,
// XNetLogonTaskContinue, XNetLogonTaskClose, XNetLogonGetLoggedOnUsers,
// XNetLogonGetMachineID and XNetLogonSetConsoleCertificate are all undefined r3
// with no output written. So every attempt to connect failed, the gamercard read
// Disconnected, and the LIVE channels stayed hidden.
//
// The contract, from guest 0x922BE610:
//
//     result = XamUserLogon((PXUID)(a1 + 48), *(a1 + 80), (PXOVERLAPPED)(a1 + 8));
//     if ( result >= 0 )
//         return -2147483638;                       // E_PENDING
//
// so it is asynchronous: three arguments, and a non-negative return means the
// request was accepted with the outcome delivered through the overlapped. Guest
// 0x922E37B8 confirms the waiting side, spinning while the overlapped reads 997
// and then reading the real result out of it:
//
//     result = XamUserLogon(v12, 0x25u, &v11);
//     if ( result >= 0 )
//     {
//         while ( v11.InternalLow == 997 ) { pump }
//         if ( XamGetOverlappedResult(&v11, 0, 0) || ... )
//
// XamUserLogon itself turns out to be already implemented, in user_profile.cpp,
// and correctly -- it completes the overlapped with success. So the logon call is
// not the gap. What was still missing is the XNetLogon task family around it,
// which is what this file supplies.
//
// What this is
// ------------
// A simulated logon, not a real one. Nothing is authenticated, no credentials
// are checked and no server is contacted -- there is no Xbox LIVE to contact.
// This is the same trade already taken for the NCSI probe, the 0x58003
// reachability query and the sign-in state, made deliberately so an offline
// console can present the parts of itself that are gated behind being connected.
// Every screen behind this gate is rendered from local data.
//
// The machine ID is derived from the console's own identity rather than invented
// per run, so it is at least stable across launches.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/types.h>

using namespace rex;

namespace {

// The XNetLogon task family. All bare stubs, all answered definitely so nothing
// downstream reads an undefined register or an unwritten buffer.
u32 XNetLogonTaskStart_entry(u32 unk1, u32 unk2, u32 unk3) {
  (void)unk1;
  (void)unk2;
  (void)unk3;
  return X_ERROR_SUCCESS;
}

u32 XNetLogonTaskContinue_entry(u32 unk1, u32 unk2) {
  (void)unk1;
  (void)unk2;
  return X_ERROR_SUCCESS;  // the task is already done
}

u32 XNetLogonTaskClose_entry(u32 unk1) {
  (void)unk1;
  return X_ERROR_SUCCESS;
}

// One user, index 0 -- the same single profile the rest of this port reports.
u32 XNetLogonGetLoggedOnUsers_entry(mapped_u32 mask_out) {
  if (mask_out) {
    *mask_out = 1;  // user 0 only
  }
  return X_ERROR_SUCCESS;
}

// Stable across launches: taken from the console's own XUID rather than made up
// per run, so anything that remembers it stays consistent.
u32 XNetLogonGetMachineID_entry(mapped_u64 machine_id_out) {
  if (machine_id_out) {
    const auto& profile = REX_KERNEL_STATE()->user_profile();
    const uint64_t xuid = profile ? profile->xuid() : 0;
    *machine_id_out = 0xFA00000000000000ull | (xuid & 0x00FFFFFFFFFFFFFFull);
  }
  return X_ERROR_SUCCESS;
}

u32 XNetLogonSetConsoleCertificate_entry(u32 a, u32 b, u32 c) {
  (void)a;
  (void)b;
  (void)c;
  return X_ERROR_SUCCESS;
}

// XamShowSigninUIp -- a real source of run-to-run randomness.
//
// It ships as a bare REX_EXPORT_STUB, so it returns an undefined r3, and the
// connect flow at guest 0x922E37B8 branches on it:
//
//     if ( XamGetOverlappedResult(&v11, 0, 0) || XamShowSigninUIp(v6, 1, v4) )
//     {
//         ... second XamUserLogon(0x28), completes, done ...
//     }
//     else
//     {
//         while ( !XamIsUIActive() ) { pump }
//         while (  XamIsUIActive() ) { pump }
//         return XNotifyBroadcast(0x80040016, (v6 << 16) | 0x2712);
//     }
//
// XamUserLogon succeeds here, so XamGetOverlappedResult returns 0 and the whole
// decision rests on this stub. A stale register that happens to be zero takes the
// else branch; anything else takes the first. That is the same flow behaving
// differently from one launch to the next for no reason the dashboard can see,
// which is exactly the kind of thing that makes a symptom look like it appears
// and disappears on its own.
//
// It answers "no sign-in UI was shown", which is true -- there is no system UI
// layer in this port to show one.
//
// Deliberately NOT zero, even though the else branch is the one that broadcasts:
// that branch waits for a sign-in UI to become active and then go away
//
//     while ( !XamIsUIActive() ) { pump }
//
// and with no UI to activate, that first loop never exits. Returning zero here
// would trade an intermittent missing tab for a reliable hang. The first branch
// runs a second logon instead and completes without needing any UI.
u32 XamShowSigninUIp_entry(u32 user_index, u32 panes, u32 flags) {
  (void)panes;
  (void)flags;

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamShowSigninUIp(user {}) -> no sign-in UI in this port", user_index);
  }
  return X_ERROR_FUNCTION_FAILED;
}

}  // namespace

REX_EXPORT(__imp__XamShowSigninUIp, XamShowSigninUIp_entry)
REX_EXPORT(__imp__XNetLogonTaskStart, XNetLogonTaskStart_entry)
REX_EXPORT(__imp__XNetLogonTaskContinue, XNetLogonTaskContinue_entry)
REX_EXPORT(__imp__XNetLogonTaskClose, XNetLogonTaskClose_entry)
REX_EXPORT(__imp__XNetLogonGetLoggedOnUsers, XNetLogonGetLoggedOnUsers_entry)
REX_EXPORT(__imp__XNetLogonGetMachineID, XNetLogonGetMachineID_entry)
REX_EXPORT(__imp__XNetLogonSetConsoleCertificate, XNetLogonSetConsoleCertificate_entry)
