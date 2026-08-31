// Network settings for the NXE dashboard.
//
// Network Settings showed "Internet: Unknown" and "Xbox LIVE: Unknown" and
// raised a dialog claiming "information must be added to your console" before
// it would test anything.
//
// The dashboard does not use XNetGetConnectStatus (it does not even import it).
// It drives the whole screen through the Xnp family, and three of those ship as
// bare REX_EXPORT_STUB, which log and never assign r3:
//
//     NetDll_XnpLoadConfigParams   492-byte config blob; the update dialog
//     NetDll_XnpSaveConfigParams   nowhere to persist a network config
//     NetDll_XnpGetConfigStatus    the status word the screen is built on
//
// The status word is consumed as a bit field by the state machine at guest
// 0x922952C0, and bit 0 gates the entire thing:
//
//     sub_92506868((PXNetConfigStatus)(v1 + 564));
//     v9 = *(_DWORD *)(v1 + 564);
//     if ((v9 & 1) == 0) return result;      // never leaves state 1
//
// Because the stub never wrote it, that read returned whatever was already in
// the object and the machine never advanced -- which is exactly "Unknown" for
// every row.
//
// Argument order is not guessed: the guest thunks at 0x92506838/0x92506868 do
//
//     mr r4, r3        ; caller's pointer moves to arg 2
//     li r3, 1         ; arg 1 is the XNCALLER id
//     b  NetDll_Xnp... ;
//
// so each of these takes (caller, pointer).
//
// What this can and cannot honestly do
// ------------------------------------
// Xbox LIVE for the 360 was shut down, so "Test Xbox LIVE Connection" cannot
// succeed and nothing here pretends otherwise. The sockets underneath are real
// -- NetDll_connect goes to a genuine host socket -- so a LAN and an internet
// route genuinely exist; what was missing was any console-level identity, since
// XNetGetTitleXnAddr reported loopback with no MAC worth the name.
//
// So the goal is a coherent OFFLINE console: definite answers instead of
// "Unknown", a real address, and a clean failure at the Xbox LIVE stage rather
// than a hang. The ONLINE bit is deliberately left clear, because it would be
// a lie.
//
// The one thing that is inferred rather than derived is the meaning of the
// status bits beyond bit 0. Bit 0 is provably "result present" from the gate
// above; the success and error-cause bits (0x4, 0x10, 0x100..0x1000,
// 0x40000000) are not mapped. Rather than invent them, this reports bit 0 alone
// -- which walks the state machine down its "completed, not connected" path
// without asserting any cause it cannot stand behind.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "storage_device.h"

using namespace rex;

namespace nxe_net {
uint32_t HostIpv4NetworkOrder();
}

namespace {

// Size the guest zeroes before calling XnpLoadConfigParams (guest 0x92294568:
// sub_9214B2C0(a1 + 72, 0, 492)).
constexpr uint32_t kConfigParamsSize = 492;

// Provably "a result is present" -- the gate at 0x922952C0. No other bit is
// asserted; see the note above.
constexpr uint32_t kConfigStatusComplete = 0x00000001;

// XNADDR, laid out as the runtime declares it in xam_net.cpp. The two addresses
// are already in network byte order, so their octets are written in order.
constexpr uint32_t kXnAddrIna = 0;
constexpr uint32_t kXnAddrInaOnline = 4;
constexpr uint32_t kXnAddrPortOnline = 8;
constexpr uint32_t kXnAddrEnet = 10;
constexpr uint32_t kXnAddrOnline = 16;

// XnAddrStatus, from the runtime's own table.
constexpr uint32_t kXnAddrDhcp = 0x00000008;
constexpr uint32_t kXnAddrGateway = 0x00000020;
constexpr uint32_t kXnAddrDns = 0x00000040;
// kXnAddrOnlineFlag (0x80) intentionally never set: there is no online service.

// Ethernet link flags, per the public XDK definitions: ACTIVE | 100MBPS |
// FULL_DUPLEX. The host does have a working link; reporting none was what made
// the console believe the cable was unplugged.
constexpr uint32_t kLinkActive = 0x01;
constexpr uint32_t kLink100Mbps = 0x02;
constexpr uint32_t kLinkFullDuplex = 0x08;

std::mutex g_mutex;
std::vector<uint8_t> g_config_params;
bool g_config_loaded = false;

std::filesystem::path ConfigPath() { return nxe_storage::Root() / "netconfig.bin"; }

void EnsureConfigLoaded() {
  if (g_config_loaded) {
    return;
  }
  g_config_loaded = true;
  g_config_params.assign(kConfigParamsSize, 0);

  FILE* f = nullptr;
  if (fopen_s(&f, ConfigPath().string().c_str(), "rb") != 0 || f == nullptr) {
    return;
  }
  const size_t read = std::fread(g_config_params.data(), 1, kConfigParamsSize, f);
  std::fclose(f);
  if (read == kConfigParamsSize) {
    REXKRNL_INFO("Network: restored saved configuration from {}", ConfigPath().string());
  } else {
    g_config_params.assign(kConfigParamsSize, 0);
  }
}

// Implemented in host_ip.cpp -- see the note there on why Winsock cannot be
// included alongside rex/system/xsocket.h.
uint32_t HostIpv4NetworkOrder() { return nxe_net::HostIpv4NetworkOrder(); }

void StoreBe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

//=============================================================================
// Entry points
//=============================================================================

u32 XnpLoadConfigParams_entry(u32 caller, mapped_void params) {
  (void)caller;
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureConfigLoaded();

  auto* dst = params.as<uint8_t*>();
  if (dst == nullptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  std::memcpy(dst, g_config_params.data(), kConfigParamsSize);

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("NetDll_XnpLoadConfigParams -> {} bytes of network configuration",
                 kConfigParamsSize);
  }
  return X_STATUS_SUCCESS;
}

u32 XnpSaveConfigParams_entry(u32 caller, mapped_void params) {
  (void)caller;
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureConfigLoaded();

  const auto* src = params.as<const uint8_t*>();
  if (src == nullptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  g_config_params.assign(src, src + kConfigParamsSize);

  FILE* f = nullptr;
  if (fopen_s(&f, ConfigPath().string().c_str(), "wb") == 0 && f != nullptr) {
    std::fwrite(g_config_params.data(), 1, g_config_params.size(), f);
    std::fclose(f);
  }
  REXKRNL_INFO("NetDll_XnpSaveConfigParams: saved network configuration");
  return X_STATUS_SUCCESS;
}

u32 XnpGetConfigStatus_entry(u32 caller, mapped_void status) {
  (void)caller;
  auto* dst = status.as<uint8_t*>();
  if (dst == nullptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Only the leading status word is written: the rest of XNetConfigStatus is
  // not mapped, and the state machine reads nothing else.
  dst[0] = static_cast<uint8_t>(kConfigStatusComplete >> 24);
  dst[1] = static_cast<uint8_t>(kConfigStatusComplete >> 16);
  dst[2] = static_cast<uint8_t>(kConfigStatusComplete >> 8);
  dst[3] = static_cast<uint8_t>(kConfigStatusComplete);

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("NetDll_XnpGetConfigStatus -> {:#010x} (complete, not connected)",
                 kConfigStatusComplete);
  }
  return X_STATUS_SUCCESS;
}

u32 XNetGetEthernetLinkStatus_entry(u32 caller) {
  (void)caller;
  return kLinkActive | kLink100Mbps | kLinkFullDuplex;
}

u32 XNetGetTitleXnAddr_entry(u32 caller, mapped_void addr) {
  (void)caller;
  auto* p = addr.as<uint8_t*>();
  if (p == nullptr) {
    return 0;  // XNET_GET_XNADDR_PENDING
  }

  const uint32_t ina = HostIpv4NetworkOrder();
  std::memcpy(p + kXnAddrIna, &ina, 4);

  // No online service exists, so there is no online address, port or identity.
  std::memset(p + kXnAddrInaOnline, 0, 4);
  StoreBe16(p + kXnAddrPortOnline, 0);
  std::memset(p + kXnAddrOnline, 0, 20);

  // A stable, locally-administered MAC (bit 1 of the first octet). This console
  // is virtual, so it gets a virtual address rather than borrowing the host's.
  static const uint8_t kMac[6] = {0x02, 0x52, 0x45, 0x58, 0x33, 0x60};
  std::memcpy(p + kXnAddrEnet, kMac, sizeof(kMac));

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    const uint32_t host_order = (ina >> 24) | ((ina >> 8) & 0xFF00) |
                               ((ina << 8) & 0xFF0000) | (ina << 24);
    REXKRNL_INFO("NetDll_XNetGetTitleXnAddr -> {}.{}.{}.{} (offline)", (host_order >> 24) & 0xFF,
                 (host_order >> 16) & 0xFF, (host_order >> 8) & 0xFF, host_order & 0xFF);
  }
  return kXnAddrDhcp | kXnAddrGateway | kXnAddrDns;
}

//=============================================================================
// WSASendTo
//=============================================================================
//
// Once the status machine advanced, the dashboard did what it is supposed to do
// next: it tried to talk to Xbox LIVE. That send is overlapped, and the SDK's
// NetDll_WSASendTo opens with
//
//     assert(!overlapped);
//     assert(!completion_routine);
//
// -- raw asserts, live in this debug-CRT build, so the first logon packet
// aborted the process (exit 3, abort() in ucrtbased under rexruntimed, entered
// from the guest's WSASendTo thunk at 0x925067B0).
//
// Dropping the asserts alone would be worse than the crash: the send would go
// out but the overlapped operation would never complete, and the guest would
// wait forever for a completion that never arrives.
//
// So this does what the SDK's own comment says should happen -- "TODO:
// Instantly complete overlapped". A synchronous completion is not a shortcut;
// it is what Winsock genuinely does whenever the send fits in the socket
// buffer. The datagram goes out for real, the overlapped record is filled in
// with the byte count, and its event is signalled so the waiting guest thread
// wakes.
//
// The logon still fails, and that is correct: NetDll_WSARecvFrom returns -1
// unconditionally ("we're not going to be receiving packets any time soon"), so
// nothing ever answers. The dashboard times out and reports no Xbox LIVE --
// which is the truth, the service having been shut down. The point of this
// function is that it fails as a failed connection instead of as an abort.

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  rex::be<uint32_t> offset_low;
  rex::be<uint32_t> offset_high;
  rex::be<uint32_t> event_handle;
};

u32 WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers, u32 num_buffers,
                    mapped_u32 num_bytes_sent, u32 flags, ppc_ptr_t<rex::system::XSOCKADDR_IN> to_ptr,
                    u32 to_len, ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  (void)caller;
  (void)completion_routine;

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(socket_handle);
  if (!socket) {
    rex::system::XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return static_cast<u32>(-1);
  }

  // This implementation takes a single buffer, so gather the scatter list --
  // same as the SDK does.
  std::vector<uint8_t> combined;
  for (uint32_t i = 0; i < num_buffers; ++i) {
    const uint32_t len = buffers[i].len;
    const auto* src = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(buffers[i].buf_ptr);
    if (src != nullptr && len != 0) {
      combined.insert(combined.end(), src, src + len);
    }
  }

  rex::system::N_XSOCKADDR_IN native_to(to_ptr);
  const int sent = socket->SendTo(combined.data(), static_cast<uint32_t>(combined.size()), flags,
                                  &native_to, to_len);
  const uint32_t byte_count = sent < 0 ? 0u : static_cast<uint32_t>(sent);

  if (num_bytes_sent) {
    *num_bytes_sent = byte_count;
  }

  if (overlapped) {
    // Completed synchronously: no error, this many bytes transferred.
    overlapped->internal = 0;
    overlapped->internal_high = byte_count;
    if (overlapped->event_handle != 0) {
      auto evt = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(overlapped->event_handle);
      if (evt) {
        evt->Set(0, false);
      }
    }
  }

  return 0;
}

}  // namespace

REX_EXPORT(__imp__NetDll_WSASendTo, WSASendTo_entry)
REX_EXPORT(__imp__NetDll_XnpLoadConfigParams, XnpLoadConfigParams_entry)
REX_EXPORT(__imp__NetDll_XnpSaveConfigParams, XnpSaveConfigParams_entry)
REX_EXPORT(__imp__NetDll_XnpGetConfigStatus, XnpGetConfigStatus_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus, XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, XNetGetTitleXnAddr_entry)
