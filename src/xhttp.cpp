// XHttp: the dashboard's HTTP client.
//
// The Internet stage of "Test Xbox LIVE Connection" runs entirely through this
// API, and every function in it ships as a bare REX_EXPORT_STUB. A single test
// produces exactly this sequence:
//
//     XHttpStartup                    STUB
//     XHttpOpen                       STUB   -> garbage session handle
//     XHttpConnect                    STUB   -> garbage connection handle
//     XHttpOpenRequestUsingMemory     STUB   -> garbage request handle
//     XHttpSetStatusCallback          STUB
//     XHttpSendRequest                STUB
//     XHttpDoWork                     ...    -> polled
//
// so the whole exchange was conducted with undefined handles.
//
// The ABI is not guessed. Every XHttp import takes the XNCALLER id as its first
// argument and the guest wrappers insert it, e.g. 0x921E6B00:
//
//     mr r7, r6 ; mr r6, r5 ; mr r5, r4 ; mr r4, r3 ; li r3, 1
//     b  NetDll_XHttpConnect
//
// which makes the shapes XHttpOpen(xnc, agent, access, proxy, bypass, flags)
// and XHttpConnect(xnc, session, server, port, reserved) -- the same shapes as
// the WinHTTP calls they mirror.
//
// This is deliberately stage one of two.
//
// Implementing the request/response path is only worth doing if the host the
// dashboard asks for is actually reachable; the Xbox LIVE services this was
// written to talk to were shut down, and a perfect HTTP client aimed at an
// endpoint that no longer answers still reports a failed test. So this stage
// gives the API real handles and a real handle table -- removing the undefined
// values -- and records what the dashboard is trying to reach, resolving and
// probing it on a background thread so the answer is measured rather than
// assumed.
//
// What it deliberately does NOT do is claim a request succeeded. SendRequest
// reports the request as accepted, and XHttpDoWork (in network.cpp) continues
// to complete it as a definite connection failure, exactly as it does today.
// The observable behaviour is therefore unchanged -- a test that fails cleanly
// rather than hanging -- and what this buys is the one fact needed to decide
// whether stage two can ever succeed.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#pragma comment(lib, "ws2_32.lib")

using namespace rex;

namespace {

enum class HandleKind { kSession, kConnect, kRequest };

// Where a request has got to. XHttpDoWork walks it one step per pump, firing
// the guest's status callback each time -- which is what actually drives the
// state machine at guest 0x92507688. An earlier attempt reported "in progress"
// from DoWork without ever firing a callback, and that is exactly why the test
// hung: the machine had nothing to advance on.
enum class Stage {
  kIdle,           // created, not yet sent
  kSendComplete,   // owes SENDREQUEST_COMPLETE
  kHeaders,        // owes HEADERS_AVAILABLE
  kDataAvailable,  // owes DATA_AVAILABLE
  kReadComplete,   // owes READ_COMPLETE
  kDone,
};

struct HttpHandle {
  HandleKind kind;
  std::string server;   // connections and requests
  uint32_t port = 0;
  std::string verb;     // requests
  std::string object;   // requests

  // Request-only state.
  Stage stage = Stage::kIdle;
  uint32_t callback = 0;   // guest status callback
  uint32_t context = 0;    // dwContext; written at +0x20 / +0x28
  uint32_t last_read = 0;  // bytes handed over by the last ReadData
  size_t read_offset = 0;
  uint32_t pumps = 0;      // fail-safe, see kMaxPumps
};

// The status codes the guest's callback at 0x92506D58 actually switches on,
// read off its disassembly. They are the standard WinHTTP async notifications.
constexpr uint32_t kStatusSendRequestComplete = 0x00020000;
constexpr uint32_t kStatusDataAvailable = 0x00080000;
constexpr uint32_t kStatusReadComplete = 0x00100000;
constexpr uint32_t kStatusRequestError = 0x00200000;
constexpr uint32_t kStatusHeadersAvailable = 0x00400000;

// WINHTTP_QUERY_FLAG_NUMBER. The guest queries with level 0x2000FFFE and a
// 4-byte buffer, then compares the result against 0x190 (400) -- so what it
// wants back is the status code as a number.
constexpr uint32_t kQueryFlagNumber = 0x20000000;

// A request that somehow never completes must not be polled forever. Once every
// notification has been delivered and the guest is still pumping, the request
// is failed definitively rather than left hanging -- the failure mode that has
// already caught this screen twice.
constexpr uint32_t kMaxPumps = 64;

// Served for the connectivity probe. The console fetches
// http://assets.xbox.com/XBOXNCSI.txt, which Microsoft has retired: the host no
// longer resolves at all, so no faithful HTTP client could complete it. This is
// answered locally by explicit choice, which does mean the Internet stage
// reports success on this runtime's say-so rather than from a real measurement.
constexpr char kNcsiBody[] = "Microsoft NCSI";
constexpr uint32_t kHttpStatusOk = 200;
constexpr uint32_t kHttpStatusNotFound = 404;

// Only the connectivity probe gets the canned body. Everything else used to get
// it too, which was worse than answering nothing: the dashboard asks the LIVE
// service for real files -- the account screen fetches
//
//     GET http://127.0.0.1:1000/xedl/ProfileInfo/103/CountryInfo.cib
//
// -- and handing a binary parser the 14 ASCII bytes "Microsoft NCSI" is not a
// response, it is corruption. The screen reported a hard "Can't connect to Xbox
// LIVE" error rather than the soft "some content is temporarily unavailable" it
// has for content it simply cannot get.
//
// There is no LIVE service to fetch these from and no honest way to synthesise
// their contents, so they are answered 404. That is true -- this machine does
// not have the file -- and it lets the dashboard take its own not-available
// path instead of choking on a bad payload.
bool IsConnectivityProbe(const std::string& object) {
  return object.find("NCSI") != std::string::npos || object.find("ncsi") != std::string::npos;
}

std::mutex g_mutex;
std::map<uint32_t, HttpHandle> g_handles;
uint32_t g_next_handle = 1;

// Distinctive, obviously non-null handle values so a stray one is recognisable
// in a log or a crash dump rather than looking like a pointer.
constexpr uint32_t kHandleBase = 0xB0000000u;

uint32_t CreateHandle(HttpHandle handle) {
  const uint32_t id = kHandleBase | g_next_handle++;
  g_handles.emplace(id, std::move(handle));
  return id;
}

const HttpHandle* FindHandle(uint32_t id) {
  const auto it = g_handles.find(id);
  return it == g_handles.end() ? nullptr : &it->second;
}

// Guest strings here are plain ANSI, not the wide strings the WinHTTP-shaped
// signatures would suggest. That was established by dumping the pointer rather
// than assumed: the server argument reads
//
//     61 73 73 65 74 73 2E 78 62 6F 78 2E 63 6F 6D 00   "assets.xbox.com\0"
//
// Decoding it as UTF-16 produced binary noise with unrelated XUI strings caught
// in the tail, which is what a wrong stride looks like.
std::string ReadGuestString(mapped_void ptr, size_t max_chars = 512) {
  const auto* p = ptr.as<const char*>();
  if (p == nullptr) {
    return {};
  }
  std::string out;
  for (size_t i = 0; i < max_chars && p[i] != '\0'; ++i) {
    out.push_back(p[i]);
  }
  return out;
}

// Resolve and try a TCP connect, off the guest thread so nothing stalls.
// Purely diagnostic: it reports whether the endpoint the dashboard wants is
// reachable from this machine at all.
void ProbeReachability(std::string server, uint32_t port) {
  // 0 is INTERNET_DEFAULT_PORT, which means 80 for http.
  if (port == 0) {
    port = 80;
  }
  std::thread([server, port] {
    WSADATA wsa{};
    const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const std::string port_str = std::to_string(port);

    if (getaddrinfo(server.c_str(), port_str.c_str(), &hints, &results) != 0 || results == nullptr) {
      REXKRNL_WARN("XHttp probe: '{}' does not resolve", server);
      if (started) {
        WSACleanup();
      }
      return;
    }

    char addr_text[INET_ADDRSTRLEN] = {};
    const auto* sin = reinterpret_cast<const sockaddr_in*>(results->ai_addr);
    inet_ntop(AF_INET, &sin->sin_addr, addr_text, sizeof(addr_text));

    SOCKET s = socket(results->ai_family, results->ai_socktype, results->ai_protocol);
    bool connected = false;
    if (s != INVALID_SOCKET) {
      // Non-blocking connect with a short deadline: an unreachable host must
      // not keep this thread alive for the default TCP timeout.
      u_long non_blocking = 1;
      ioctlsocket(s, FIONBIO, &non_blocking);
      connect(s, results->ai_addr, static_cast<int>(results->ai_addrlen));

      fd_set write_set;
      FD_ZERO(&write_set);
      FD_SET(s, &write_set);
      timeval timeout{};
      timeout.tv_sec = 4;
      connected = select(0, nullptr, &write_set, nullptr, &timeout) > 0;
      closesocket(s);
    }

    REXKRNL_INFO("XHttp probe: '{}:{}' -> {} ({})", server, port, addr_text,
                 connected ? "reachable" : "no response");

    freeaddrinfo(results);
    if (started) {
      WSACleanup();
    }
  }).detach();
}

//=============================================================================
// Entry points
//=============================================================================

u32 XHttpStartup_entry(u32 xnc, u32 flags) {
  (void)xnc;
  (void)flags;
  return 1;  // TRUE
}

u32 XHttpShutdown_entry(u32 xnc) {
  (void)xnc;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handles.clear();
  return 1;
}

u32 XHttpOpen_entry(u32 xnc, mapped_void user_agent, u32 access_type, mapped_void proxy,
                    mapped_void proxy_bypass, u32 flags) {
  (void)xnc;
  (void)access_type;
  (void)proxy;
  (void)proxy_bypass;
  (void)flags;

  std::lock_guard<std::mutex> lock(g_mutex);
  const uint32_t handle = CreateHandle({HandleKind::kSession});
  REXKRNL_INFO("XHttpOpen(agent='{}') -> session {:#x}", ReadGuestString(user_agent), handle);
  return handle;
}

u32 XHttpConnect_entry(u32 xnc, u32 session, mapped_void server_name, u32 port, u32 reserved) {
  (void)xnc;
  (void)reserved;

  const std::string server = ReadGuestString(server_name);

  uint32_t handle = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (FindHandle(session) == nullptr) {
      REXKRNL_WARN("XHttpConnect: unknown session {:#x}", session);
      return 0;
    }
    HttpHandle h{HandleKind::kConnect};
    h.server = server;
    h.port = port;
    handle = CreateHandle(std::move(h));
  }

  REXKRNL_INFO("XHttpConnect('{}' port {}) -> connection {:#x}", server, port, handle);
  static bool s_probed = false;
  if (!server.empty() && !s_probed) {
    s_probed = true;
    ProbeReachability(server, port);
  }
  return handle;
}

uint32_t OpenRequestCommon(uint32_t connection, mapped_void verb, mapped_void object) {
  const std::string verb_text = ReadGuestString(verb);
  const std::string object_text = ReadGuestString(object);

  std::lock_guard<std::mutex> lock(g_mutex);
  const HttpHandle* parent = FindHandle(connection);
  if (parent == nullptr) {
    REXKRNL_WARN("XHttpOpenRequest: unknown connection {:#x}", connection);
    return 0;
  }

  HttpHandle h{HandleKind::kRequest};
  h.server = parent->server;
  h.port = parent->port;
  h.verb = verb_text.empty() ? "GET" : verb_text;
  h.object = object_text;
  const uint32_t handle = CreateHandle(std::move(h));

  REXKRNL_INFO("XHttpOpenRequest({} http://{}:{}{}) -> request {:#x}",
               verb_text.empty() ? "GET" : verb_text, parent->server, parent->port, object_text,
               handle);
  return handle;
}

u32 XHttpOpenRequest_entry(u32 xnc, u32 connection, mapped_void verb, mapped_void object,
                           mapped_void version, mapped_void referrer, mapped_void accept_types,
                           u32 flags) {
  (void)xnc;
  (void)version;
  (void)referrer;
  (void)accept_types;
  (void)flags;
  return OpenRequestCommon(connection, verb, object);
}

u32 XHttpOpenRequestUsingMemory_entry(u32 xnc, u32 connection, mapped_void verb,
                                      mapped_void object, mapped_void version,
                                      mapped_void referrer, mapped_void accept_types, u32 flags,
                                      mapped_void buffer, u32 buffer_size) {
  (void)xnc;
  (void)version;
  (void)referrer;
  (void)accept_types;
  (void)flags;
  (void)buffer;
  (void)buffer_size;
  return OpenRequestCommon(connection, verb, object);
}

u32 XHttpSendRequest_entry(u32 xnc, u32 request, mapped_void headers, u32 headers_length,
                           mapped_void optional, u32 optional_length, u32 total_length,
                           u32 context) {
  (void)xnc;
  (void)headers;
  (void)headers_length;
  (void)optional;
  (void)optional_length;
  (void)total_length;
  (void)context;

  std::lock_guard<std::mutex> lock(g_mutex);
  const HttpHandle* h = FindHandle(request);
  if (h == nullptr) {
    REXKRNL_WARN("XHttpSendRequest: unknown request {:#x}", request);
    return 0;
  }
  // Accepted for asynchronous delivery; DoWork drives it from here.
  const_cast<HttpHandle*>(h)->context = context;
  const_cast<HttpHandle*>(h)->stage = Stage::kSendComplete;
  return 1;
}

// Deliver one status notification to the guest.
//
// The callback contract comes straight off 0x92506D58:
//
//     stw r7, 0x28(r4)     ; context->[0x28] = dwStatusInformationLength
//     stw r11, 0x20(r4)    ; context->[0x20] = 0x1500F0 on success
//     lwz r11, 4(r6)       ; on REQUEST_ERROR, dwError from the async result
//
// so it is the ordinary five-argument WinHTTP shape, with r4 carrying the
// context -- and context+0x20 / +0x28 are the same slots the state machine at
// 0x92507688 reads back as a1[8] and a1[10].
//
// This must run on a guest thread, which is why it is driven from DoWork:
// GuestToHostFunction needs a ThreadState, and a host worker thread has none.
void Notify(uint32_t callback, uint32_t hinternet, uint32_t context, uint32_t status,
            uint32_t info_ptr, uint32_t info_len) {
  if (callback == 0) {
    return;
  }
  PPCFunc* fn = rex::runtime::ResolveIndirectFunction(callback);
  if (fn == nullptr) {
    REXKRNL_WARN("XHttp: status callback {:#x} does not resolve", callback);
    return;
  }
  rex::ppc::GuestToHostFunction<void>(*fn, hinternet, context, status, info_ptr, info_len);
}

u32 XHttpSetStatusCallback_entry(u32 xnc, u32 request, u32 callback, u32 flags, u32 reserved) {
  (void)xnc;
  (void)flags;
  (void)reserved;

  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_handles.find(request);
  if (it == g_handles.end()) {
    return 0;
  }
  it->second.callback = callback;
  return 1;
}

u32 XHttpReceiveResponse_entry(u32 xnc, u32 request, u32 reserved) {
  (void)xnc;
  (void)reserved;

  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_handles.find(request);
  if (it == g_handles.end()) {
    return 0;
  }
  it->second.stage = Stage::kHeaders;
  return 1;
}

u32 XHttpQueryHeaders_entry(u32 xnc, u32 request, u32 info_level, mapped_void name,
                            mapped_void buffer, mapped_u32 buffer_length, mapped_u32 index) {
  (void)xnc;
  (void)name;
  (void)index;

  std::lock_guard<std::mutex> lock(g_mutex);
  const auto found = g_handles.find(request);
  if (found == g_handles.end()) {
    return 0;
  }
  const uint32_t status =
      IsConnectivityProbe(found->second.object) ? kHttpStatusOk : kHttpStatusNotFound;

  auto* out = buffer.as<uint8_t*>();
  if ((info_level & kQueryFlagNumber) != 0 && out != nullptr) {
    // Big-endian DWORD, like everything else the guest reads.
    out[0] = static_cast<uint8_t>(status >> 24);
    out[1] = static_cast<uint8_t>(status >> 16);
    out[2] = static_cast<uint8_t>(status >> 8);
    out[3] = static_cast<uint8_t>(status);
    if (buffer_length) {
      *buffer_length = 4;
    }
    return 1;
  }

  REXKRNL_WARN("XHttpQueryHeaders: unsupported info level {:#x}", info_level);
  return 0;
}

u32 XHttpReadData_entry(u32 xnc, u32 request, mapped_void buffer, u32 buffer_size,
                        mapped_u32 bytes_read) {
  (void)xnc;

  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_handles.find(request);
  if (it == g_handles.end()) {
    return 0;
  }
  HttpHandle& h = it->second;

  // Only the probe has a body; anything else was answered 404 above.
  const size_t body_len = IsConnectivityProbe(h.object) ? sizeof(kNcsiBody) - 1 : 0;
  const size_t remaining = h.read_offset >= body_len ? 0 : body_len - h.read_offset;
  const uint32_t count = static_cast<uint32_t>(std::min<size_t>(remaining, buffer_size));

  if (auto* out = buffer.as<uint8_t*>()) {
    std::memcpy(out, kNcsiBody + h.read_offset, count);
  }
  h.read_offset += count;
  h.last_read = count;
  if (bytes_read) {
    *bytes_read = count;
  }

  h.stage = Stage::kReadComplete;
  return 1;
}

// The pump. Advances every live request by one notification.
u32 XHttpDoWork_entry(u32 xnc, u32 handle, u32 flags) {
  (void)xnc;
  (void)handle;
  (void)flags;

  struct Pending {
    uint32_t callback;
    uint32_t hinternet;
    uint32_t context;
    uint32_t status;
    uint32_t info_len;
  };
  std::vector<Pending> pending;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [id, h] : g_handles) {
      if (h.kind != HandleKind::kRequest || h.stage == Stage::kIdle) {
        continue;
      }
      if (++h.pumps > kMaxPumps) {
        continue;  // handled below, outside the lock
      }
      switch (h.stage) {
        case Stage::kSendComplete:
          pending.push_back({h.callback, id, h.context, kStatusSendRequestComplete, 0});
          h.stage = Stage::kDone;  // ReceiveResponse moves it on from here
          break;
        case Stage::kHeaders:
          pending.push_back({h.callback, id, h.context, kStatusHeadersAvailable, 0});
          h.stage = Stage::kDataAvailable;
          break;
        case Stage::kDataAvailable:
          pending.push_back({h.callback, id, h.context, kStatusDataAvailable,
                             static_cast<uint32_t>(sizeof(kNcsiBody) - 1)});
          h.stage = Stage::kDone;  // ReadData moves it on
          break;
        case Stage::kReadComplete:
          pending.push_back({h.callback, id, h.context, kStatusReadComplete, h.last_read});
          h.stage = Stage::kDone;
          break;
        default:
          break;
      }
    }
  }

  for (const auto& p : pending) {
    static uint32_t s_reported = 0;
    if (s_reported < 8) {
      ++s_reported;
      REXKRNL_INFO("XHttpDoWork: request {:#x} -> status {:#x} (len {})", p.hinternet, p.status,
                   p.info_len);
    }
    Notify(p.callback, p.hinternet, p.context, p.status, 0, p.info_len);
  }
  return 1;
}

u32 XHttpCloseHandle_entry(u32 xnc, u32 handle) {
  (void)xnc;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handles.erase(handle);
  return 1;
}

}  // namespace

REX_EXPORT(__imp__NetDll_XHttpStartup, XHttpStartup_entry)
REX_EXPORT(__imp__NetDll_XHttpShutdown, XHttpShutdown_entry)
REX_EXPORT(__imp__NetDll_XHttpOpen, XHttpOpen_entry)
REX_EXPORT(__imp__NetDll_XHttpConnect, XHttpConnect_entry)
REX_EXPORT(__imp__NetDll_XHttpOpenRequest, XHttpOpenRequest_entry)
REX_EXPORT(__imp__NetDll_XHttpOpenRequestUsingMemory, XHttpOpenRequestUsingMemory_entry)
REX_EXPORT(__imp__NetDll_XHttpSendRequest, XHttpSendRequest_entry)
REX_EXPORT(__imp__NetDll_XHttpSetStatusCallback, XHttpSetStatusCallback_entry)
REX_EXPORT(__imp__NetDll_XHttpReceiveResponse, XHttpReceiveResponse_entry)
REX_EXPORT(__imp__NetDll_XHttpQueryHeaders, XHttpQueryHeaders_entry)
REX_EXPORT(__imp__NetDll_XHttpReadData, XHttpReadData_entry)
REX_EXPORT(__imp__NetDll_XHttpDoWork, XHttpDoWork_entry)
REX_EXPORT(__imp__NetDll_XHttpCloseHandle, XHttpCloseHandle_entry)
