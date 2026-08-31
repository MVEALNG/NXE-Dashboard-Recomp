// XLIVEBASE message tracing.
//
// The "Xbox LIVE" leg of the connection test does not go through a single
// kernel export the way the other two legs did. It is driven by XAM app
// messages: guest 0x921EB9A0 marshals a request and issues
//
//     XMsgInProcessCall((HXAMAPP)0xFC, 0x58017, buffer, 0)
//     XMsgStartIORequest((HXAMAPP)0xFC, ..., overlapped, ...)
//
// where 0xFC is XLIVEBASE. The runtime's XLiveBaseApp handles 0x58004, 0x58006,
// 0x58007, 0x58020, 0x58023, 0x58037 and 0x58046 -- and neither 0x58003 nor
// 0x58017, which is what the logon path actually uses. Both fall through to
// "Unimplemented XLIVEBASE message" and return X_E_FAIL.
//
// Those handlers live in XLiveBaseApp::DispatchMessageSync, a C++ method inside
// rexruntimed rather than an exported guest function, so the usual trick of
// redefining the export does not reach them. The reachable seam is one level
// up: XMsgInProcessCall itself is an export, and the runtime's version is a
// thin wrapper --
//
//     u32 XMsgInProcessCall_entry(u32 app, u32 message, u32 arg1, u32 arg2) {
//       return REX_KERNEL_STATE()->app_manager()->DispatchMessageSync(...);
//     }
//
// -- over an AppManager that KernelState exposes publicly. So this file can sit
// in front of the dispatcher, deal with specific messages itself, and hand
// everything else to exactly the same code that would have run anyway. No SDK
// change, and every message this file does not care about behaves identically.
//
// 0x58003 -- "is Xbox LIVE reachable?"
// ------------------------------------
// This one is now answered, and it is worth recording why that became
// defensible when it was previously declined.
//
// It was left alone before on the grounds that its semantics were undocumented
// and that inventing returns for an authentication handshake had already
// misfired twice here. Reading the callers settles the first point: it is not a
// handshake, it is a status query, and it takes no buffer at all. Guest
// 0x922BEA50 is the clearest of the dozen senders:
//
//     v2 = XMsgInProcessCall((HXAMAPP)0xFC, 0x58003, 0, 0);
//     if ( v2 == 0x1510F0 )                       // "not established"
//     {
//         XNetLogonGetExtendedStatus(v5, &v4);
//         if ( v4 == 0x80151002 || v4 == 0x80151007 )   // XONLINE logon errors
//             return v4;
//     }
//     return v2;
//
// So the caller asks for a connection status and reports whatever it gets. The
// runtime does not implement the message, so it returned X_E_FAIL, and every
// screen that consults it concluded the console cannot reach LIVE -- which is
// what put "Can't connect to Xbox LIVE" in front of the account screen.
//
// Answering S_OK says the connection is established. That is a simulation, not
// a connection: there is no Xbox LIVE being reached here and nothing is
// authenticated. It is the same trade already made knowingly for the NCSI probe,
// made again deliberately at the user's direction so the offline dashboard can
// get past a gate it would otherwise never pass. Every screen behind it is the
// dashboard's own, rendered from local data.
//
// 0x58007 -- CXLiveLogon::GetServiceInfo
// --------------------------------------
// This is the one that actually put the "Can't connect to Xbox LIVE. Please try
// again later." box on screen. It is not unimplemented -- the runtime answers it,
// and the answer is a hardcoded refusal:
//
//     case 0x00058007: {
//       // XOnlineGetServiceInfo, expects dwServiceId and pServiceInfo
//       REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo(...)");
//       return 0x80151802;                       // ERROR_CONNECTION_INVALID
//     }
//
// The logs show the dialog raised immediately after each call. The arguments are
// the service id and a pointer to an XONLINE_SERVICE_INFO to fill:
//
//     CXLiveLogon::GetServiceInfo(0000000B, 7015F0C0)
//     XamShowMessageBoxUI(...)
//
// It is answered here with the service pointed at the loopback address, which is
// the truthful shape of this arrangement: the "service" is this machine, and
// nothing leaves it. Anything the dashboard subsequently tries to open against
// that address will fail on its own terms rather than being told up front that
// LIVE is unreachable.
//
// Everything else is still only traced. The unknown messages stay unanswered --
// the argument that talked this file out of guessing still applies to them.

#include <cstdint>
#include <cstdio>
#include <set>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>
#include <rex/types.h>

using namespace rex;

namespace {

constexpr uint32_t kAppXLiveBase = 0xFC;

// "Is Xbox LIVE reachable?" -- see the note above.
constexpr uint32_t kMsgLiveConnectionStatus = 0x00058003;

// CXLiveLogon::GetServiceInfo -- the source of the error dialog.
constexpr uint32_t kMsgGetServiceInfo = 0x00058007;

// XONLINE_SERVICE_INFO: id, ip, port, reserved.
struct XONLINE_SERVICE_INFO {
  rex::be<uint32_t> id;
  uint8_t ip[4];
  rex::be<uint16_t> port;
  rex::be<uint16_t> reserved;
};
static_assert(sizeof(XONLINE_SERVICE_INFO) == 12, "service info must be 12 bytes");

// Messages XLiveBaseApp::DispatchMessageSync already implements; tracing these
// would be noise.
bool RuntimeHandles(uint32_t message) {
  switch (message) {
    case 0x00058004:
    case 0x00058006:
    case 0x00058007:
    case 0x00058020:
    case 0x00058023:
    case 0x00058037:
    case 0x00058046:
      return true;
    default:
      return false;
  }
}

std::mutex g_mutex;
std::set<uint32_t> g_seen;

// Dump the head of the message buffer. Argument shapes here are unknown, so
// showing the bytes is worth more than a decoded guess.
void TraceMessage(uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length, const char* via) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_seen.insert(message).second) {
      return;  // once per message is enough to characterise it
    }
  }

  char hex[3 * 32 + 1] = {};
  char ascii[33] = {};
  const auto* raw = buffer_ptr ? REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(buffer_ptr)
                               : nullptr;
  if (raw != nullptr) {
    for (int i = 0; i < 32; ++i) {
      std::snprintf(hex + i * 3, 4, "%02X ", raw[i]);
      ascii[i] = (raw[i] >= 0x20 && raw[i] < 0x7F) ? static_cast<char>(raw[i]) : '.';
    }
  }

  REXKRNL_INFO("XLIVEBASE {} msg={:#010x} buf={:#010x} len={} : {}|{}|", via, message, buffer_ptr,
               buffer_length, raw ? hex : "(no buffer)", ascii);
}

u32 XMsgInProcessCall_entry(u32 app, u32 message, u32 arg1, u32 arg2) {
  if (app == kAppXLiveBase && message == kMsgLiveConnectionStatus) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      REXKRNL_INFO("XLIVEBASE {:#010x}: reporting LIVE as connected (simulated locally)",
                   kMsgLiveConnectionStatus);
    }
    return X_ERROR_SUCCESS;
  }
  if (app == kAppXLiveBase && message == kMsgGetServiceInfo) {
    auto* info = arg2 ? REX_KERNEL_MEMORY()->TranslateVirtual<XONLINE_SERVICE_INFO*>(arg2) : nullptr;
    if (info == nullptr) {
      return X_ERROR_INVALID_PARAMETER;
    }
    info->id = arg1;
    info->ip[0] = 127;  // the "service" is this machine
    info->ip[1] = 0;
    info->ip[2] = 0;
    info->ip[3] = 1;
    info->port = 1000;
    info->reserved = 0;

    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      REXKRNL_INFO("XLIVEBASE {:#010x}: service {:#x} -> 127.0.0.1 (simulated locally)",
                   kMsgGetServiceInfo, arg1);
    }
    return X_ERROR_SUCCESS;
  }
  if (app == kAppXLiveBase && !RuntimeHandles(message)) {
    TraceMessage(message, arg1, arg2, "sync ");
  }
  // Same call the runtime's own entry point makes.
  return REX_KERNEL_STATE()->app_manager()->DispatchMessageSync(app, message, arg1, arg2);
}

u32 XMsgSystemProcessCall_entry(u32 app, u32 message, u32 buffer, u32 buffer_length) {
  if (app == kAppXLiveBase && !RuntimeHandles(message)) {
    TraceMessage(message, buffer, buffer_length, "async");
  }
  return REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, message, buffer,
                                                                 buffer_length);
}

}  // namespace

REX_EXPORT(__imp__XMsgInProcessCall, XMsgInProcessCall_entry)
REX_EXPORT(__imp__XMsgSystemProcessCall, XMsgSystemProcessCall_entry)
