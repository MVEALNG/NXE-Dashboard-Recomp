// The boot animation.
//
// A console plays its startup animation while the dashboard loads behind it,
// and that is what happens here: the video goes up in its own borderless
// topmost window covering the display, the runtime boots underneath it, and the
// window comes down when playback ends. By then the dashboard is up, so the
// animation runs to completion and the blade is simply there afterwards.
//
// Playing it first and starting the dashboard after would show the animation
// and then sixteen seconds of black while the runtime loads -- the video is
// 16.5s and a cold boot is about the same, which is why overlapping them lines
// up as well as it does.
//
// Media Foundation does the decoding (MFPlay: a player bound to an HWND, a
// callback for end-of-playback). Everything here is best effort -- a missing
// file, a codec that will not load, or MF failing to start all skip the
// animation rather than hold up the boot. Nothing about the dashboard depends
// on it.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mfapi.h>
#include <mfplay.h>

#include <filesystem>

#include <rex/cvar.h>
#include <rex/logging.h>

// Empty by default. The startup animation is not ours to ship, so anyone who
// wants one points this at their own copy.
REXCVAR_DEFINE_STRING(boot_video, "", "Boot",
                      "Video played over the dashboard while it loads. Empty disables it.");

REXCVAR_DEFINE_BOOL(boot_video_skip_on_input, true, "Boot",
                    "Let a key press or click end the boot animation early.");

namespace nxe_boot {
namespace {

constexpr wchar_t kWindowClass[] = L"NxeBootVideo";

std::atomic<bool> g_finished{false};

// MFPlay reports end-of-playback through this. Errors end it too: a video that
// cannot play should not hold the screen.
class PlayerCallback : public IMFPMediaPlayerCallback {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) {
      *out = static_cast<IMFPMediaPlayerCallback*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 1; }   // stack-owned
  STDMETHODIMP_(ULONG) Release() override { return 1; }

  void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* header) override {
    if (!header) {
      return;
    }
    if (FAILED(header->hrEvent)) {
      REXLOG_WARN("Boot video: playback error {:#010x}; skipping the animation",
                  static_cast<uint32_t>(header->hrEvent));
      g_finished.store(true);
      return;
    }
    if (header->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
      g_finished.store(true);
    }
  }
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_ERASEBKGND: {
      // Paint it black so nothing shows through before the first frame.
      RECT rc{};
      GetClientRect(hwnd, &rc);
      FillRect(reinterpret_cast<HDC>(w), &rc,
               static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      return 1;
    }
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
      if (REXCVAR_GET(boot_video_skip_on_input)) {
        g_finished.store(true);
      }
      return 0;
    case WM_CLOSE:
      g_finished.store(true);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, w, l);
}

// This process's dashboard window -- the visible top-level one that is not the
// animation's own.
struct FindState {
  HWND skip = nullptr;
  HWND found = nullptr;
};

BOOL CALLBACK FindDashProc(HWND hwnd, LPARAM param) {
  auto* state = reinterpret_cast<FindState*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || hwnd == state->skip || GetWindow(hwnd, GW_OWNER)) {
    return TRUE;
  }
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  if (wcscmp(cls, kWindowClass) == 0) {
    return TRUE;  // the animation's own window
  }
  state->found = hwnd;
  return FALSE;
}

HWND DashboardWindow() {
  FindState state;
  EnumWindows(FindDashProc, reinterpret_cast<LPARAM>(&state));
  return state.found;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 0) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  }
  return out;
}

void Play(const std::string& path) {
  if (FAILED(MFStartup(MF_VERSION))) {
    REXLOG_WARN("Boot video: Media Foundation would not start; skipping the animation");
    return;
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = nullptr;  // no pointer over the animation
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kWindowClass;
  RegisterClassExW(&wc);

  // The whole primary display, above everything, including the dashboard window
  // that is being created behind it while this plays.
  const int w = GetSystemMetrics(SM_CXSCREEN);
  const int h = GetSystemMetrics(SM_CYSCREEN);
  //
  // WS_EX_NOACTIVATE matters: without it this window takes the foreground, and
  // Windows minimizes a fullscreen window that loses focus -- so the dashboard
  // ended up on the taskbar and had to be clicked back. Not activating means it
  // never loses focus in the first place.
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kWindowClass, L"", WS_POPUP, 0, 0, w, h, nullptr, nullptr,
                              wc.hInstance, nullptr);
  if (!hwnd) {
    REXLOG_WARN("Boot video: could not create the window; skipping the animation");
    MFShutdown();
    return;
  }
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);

  PlayerCallback callback;
  IMFPMediaPlayer* player = nullptr;
  const std::wstring url = Widen(path);
  HRESULT hr = MFPCreateMediaPlayer(url.c_str(), TRUE /* start playing */, 0, &callback, hwnd,
                                    &player);
  if (FAILED(hr) || !player) {
    REXLOG_WARN("Boot video: could not open '{}' ({:#010x}); skipping the animation", path,
                static_cast<uint32_t>(hr));
    DestroyWindow(hwnd);
    MFShutdown();
    return;
  }

  REXLOG_INFO("Boot video: playing {}", path);

  // Pump until playback ends. The deadline is a backstop only: without it a
  // codec that stalls would leave a black window over the dashboard forever.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  MSG msg{};
  while (!g_finished.load() && std::chrono::steady_clock::now() < deadline) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  REXLOG_INFO("Boot video: finished; handing the screen to the dashboard");

  // Deliberately minimal teardown.
  //
  // Hide first so the dashboard is revealed before anything is torn down, and
  // then stop at destroying this window. MFShutdown is process-wide and the
  // runtime has its own media threads (there is an XMA decoder running), and
  // UnregisterClass plus taking the foreground on the way out disturbed the
  // SDL window as well: with the full cleanup in place the dashboard received a
  // close as soon as the animation ended --
  //
  //     Boot video: finished; handing the screen to the dashboard
  //     Window closing, shutting down...
  //     Title terminated; hard-exiting process.
  //
  // -- which is the animation taking the dashboard down with it. A window class
  // and an MF startup ref left behind for the life of the process cost nothing;
  // this runs once at boot.
  ShowWindow(hwnd, SW_HIDE);
  player->Shutdown();
  player->Release();
  DestroyWindow(hwnd);

  // Hand the screen back, in case the dashboard was minimized anyway. Restoring
  // and raising it is what makes the animation ending and the blade appearing
  // one movement rather than leaving it on the taskbar.
  if (HWND dash = DashboardWindow()) {
    if (IsIconic(dash)) {
      ShowWindow(dash, SW_RESTORE);
    }
    SetWindowPos(dash, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(dash);
    SetFocus(dash);
  }
}

}  // namespace

// Starts the animation and returns immediately, so the runtime carries on
// loading behind it.
void StartBootVideo() {
  const std::string path = REXCVAR_GET(boot_video);
  if (path.empty()) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    REXLOG_WARN("Boot video: '{}' does not exist; skipping the animation", path);
    return;
  }
  std::thread(Play, path).detach();
}

}  // namespace nxe_boot
