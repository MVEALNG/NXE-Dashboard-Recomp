// Host LAN address lookup.
//
// Kept in its own translation unit on purpose. rex/system/xsocket.h declares
// XSocket::AddressFamily::AF_INET, XSocket::Type::SOCK_STREAM and friends as
// enumerators, and <winsock2.h> defines those same names as macros -- including
// both in one file turns the enum declarations into "expected identifier". So
// the one place that genuinely needs Winsock lives here, and network.cpp stays
// free of it.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>

#pragma comment(lib, "ws2_32.lib")

namespace nxe_net {

uint32_t HostIpv4NetworkOrder() {
  static const uint32_t kAddr = [] {
    WSADATA wsa{};
    const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;

    uint32_t found = 0;
    char host[256] = {};
    if (gethostname(host, sizeof(host)) == 0) {
      addrinfo hints{};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* results = nullptr;
      if (getaddrinfo(host, nullptr, &hints, &results) == 0) {
        for (auto* it = results; it != nullptr; it = it->ai_next) {
          const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
          const uint32_t addr = sin->sin_addr.s_addr;
          // Skip loopback: 127.x is not a LAN identity.
          if ((ntohl(addr) >> 24) != 127) {
            found = addr;
            break;
          }
        }
        freeaddrinfo(results);
      }
    }

    if (started) {
      WSACleanup();
    }
    // Fall back to loopback so the console still gets a definite answer on a
    // machine with no usable adapter.
    return found != 0 ? found : htonl(INADDR_LOOPBACK);
  }();
  return kAddr;
}

}  // namespace nxe_net
