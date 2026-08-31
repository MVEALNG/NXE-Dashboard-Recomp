// Playing a game from the library.
//
// "Play Game" ends at XamContentLaunchImageInternal, guest 0x921F6808:
//
//     if ( a3 && a4 ) XamLoaderSetLaunchData(a3, a4);
//     v8 = XamContentLaunchImageInternal(a1, a2);
//
// which ships as a bare REX_EXPORT_STUB. REX_STUB does not assign r3, so the
// caller read an uninitialised register as the result and reported the launch
// as refused -- the "You can only play this game on the console and storage
// device it was originally installed on" dialog. That message is about a
// licence check the dashboard never actually performed; the call simply never
// returned anything.
//
// Nothing here can honour the launch the way a console would: replacing the
// running title with a recompiled Halo 3 is not something this port can do. So
// the game is handed to an emulator on the host instead. The dashboard stays
// loaded underneath, and when the emulator exits the dashboard is brought back
// with the library still where it was.

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xtypes.h>
#include <rex/string.h>
#include <rex/types.h>

#include "discord_presence.h"
#include "game_launch.h"
#include "installed_titles.h"
#include "storage_device.h"
#include "title_names.h"

using namespace rex;
using namespace rex::system;
using namespace rex::system::xam;

extern "C" {
// The dashboard's own teardown/restore pair, and the test that guards them.
// Guest 0x921F6808 shows the shape: tear down, launch, and -- crucially -- put
// it back afterwards. dash_2947, which the disc tile uses, omits the restore.
void __imp__sub_9243D558(PPCContext& __restrict ctx, uint8_t* base);  // is the UI up?
void __imp__sub_92159B60(PPCContext& __restrict ctx, uint8_t* base);  // restore
void __imp__sub_9215FBC0(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_921405B0(PPCContext& __restrict ctx, uint8_t* base);
}

REXCVAR_DECLARE(std::string, disc_title);

extern "C" {
// The Play Game handler's own body, for the content kinds this does not take over.
void __imp__sub_92278F80(PPCContext& __restrict ctx, uint8_t* base);
}



// No default: this is whatever emulator the person running it has installed,
// and guessing at somebody else's install path only produces a confusing
// "no emulator at ..." on a machine that never had one there. Empty means Play
// Game reports that it has nothing to run, which is the truth.
REXCVAR_DEFINE_STRING(game_emulator, "", "Games",
                      "Emulator run when a game is launched from the library, e.g. a "
                      "xenia_canary.exe. Empty disables launching.");

REXCVAR_DEFINE_BOOL(game_emulator_fullscreen, true, "Games",
                    "Start the emulator in fullscreen.");

namespace {

std::atomic<uint32_t> g_last_title{0};

std::string GuestString(const uint8_t* base, uint32_t address, size_t limit = 260) {
  std::string out;
  if (!address) {
    return out;
  }
  const char* p = reinterpret_cast<const char*>(base + address);
  for (size_t i = 0; i < limit && p[i]; ++i) {
    out.push_back(p[i]);
  }
  return out;
}

// Does this look like a path the guest passed by pointer, rather than a
// content record? A record starts with a big-endian device id, so its first
// bytes are not printable.
bool LooksLikeText(const std::string& s) {
  if (s.size() < 2) {
    return false;
  }
  for (unsigned char c : s) {
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

// Where a staged package lives, laid out the way the content resolver writes it.
std::filesystem::path PackagePath(const XCONTENT_AGGREGATE_DATA& content) {
  char xuid_dir[17] = {};
  std::snprintf(xuid_dir, sizeof(xuid_dir), "%016llX",
                static_cast<unsigned long long>(uint64_t(content.xuid)));
  char title_dir[9] = {};
  std::snprintf(title_dir, sizeof(title_dir), "%08X", uint32_t(content.title_id));
  char type_dir[9] = {};
  std::snprintf(type_dir, sizeof(type_dir), "%08X",
                static_cast<uint32_t>(XContentType(content.content_type)));
  return nxe_storage::ContentRoot() / xuid_dir / title_dir / type_dir / content.file_name();
}

// The executable inside a staged game. An extracted disc has default.xex at its
// root; the directory itself is accepted as a fallback so a differently shaped
// package still gets handed to the emulator rather than refused here.
std::filesystem::path ExecutableIn(const std::filesystem::path& dir) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(dir, ec)) {
    return dir;
  }
  for (const char* name : {"default.xex", "Default.xex"}) {
    const auto candidate = dir / name;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return dir;
}

//=============================================================================
// Covering the hand-off
//=============================================================================
//
// The emulator creates its window at the default size and only then goes
// fullscreen, so for a moment a small window is on screen -- and before that,
// minimising the dashboard uncovers the desktop. Neither belongs in something
// meant to look like a console booting a disc.
//
// A black window over the whole display, put up before anything moves and taken
// down once the emulator is actually fullscreen, hides the entire hand-off. It
// is the same shape as the boot animation's window and for the same reason:
// WS_EX_NOACTIVATE so it never takes the foreground, because a fullscreen window
// that loses focus gets minimised by Windows.
constexpr wchar_t kCoverClass[] = L"NxeLaunchCover";

LRESULT CALLBACK CoverProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_ERASEBKGND) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(reinterpret_cast<HDC>(w), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    return 1;
  }
  return DefWindowProcW(hwnd, msg, w, l);
}

HWND ShowCover() {
  static bool registered = false;
  if (!registered) {
    registered = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CoverProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = nullptr;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kCoverClass;
    RegisterClassExW(&wc);
  }
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kCoverClass,
                              L"", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                              GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  if (hwnd) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
  }
  RECT rc{};
  if (hwnd) {
    GetWindowRect(hwnd, &rc);
  }
  REXKRNL_INFO("Game launch: cover {} ({}x{} at {},{}) visible={} screen {}x{}",
               hwnd ? "up" : "FAILED", rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top,
               hwnd ? IsWindowVisible(hwnd) : 0, GetSystemMetrics(SM_CXSCREEN),
               GetSystemMetrics(SM_CYSCREEN));
  return hwnd;
}


// Hide the emulator's window the instant it is shown.
//
// Polling can only ever be as quick as its interval, and at 4ms a show still
// slipped through often enough to see. A window event hook is told the moment
// EVENT_OBJECT_SHOW fires for that process, so the hide lands in the same beat
// as the show rather than up to a poll afterwards. Nothing is moved or
// restyled -- the emulator's own window handling is untouched, which is what
// went wrong when the window was parked off screen or started minimised.
struct HookState {
  DWORD pid = 0;
  long screen_w = 0;
  long screen_h = 0;
};
HookState g_hook;

void CALLBACK OnWindowShown(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD,
                            DWORD) {
  if (event != EVENT_OBJECT_SHOW || !hwnd || object != OBJID_WINDOW || child != CHILDID_SELF) {
    return;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != g_hook.pid || GetWindow(hwnd, GW_OWNER)) {
    return;
  }
  RECT rc{};
  if (!GetWindowRect(hwnd, &rc)) {
    return;
  }
  // Leave the fullscreen window alone; that is the one we are waiting for.
  if (rc.right - rc.left >= g_hook.screen_w - 2 && rc.bottom - rc.top >= g_hook.screen_h - 2) {
    return;
  }
  ShowWindow(hwnd, SW_HIDE);
}

struct FindState {
  HWND found = nullptr;
};

BOOL CALLBACK FindDashProc(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER) || !IsWindowVisible(hwnd)) {
    return TRUE;
  }

  // Never the cover.
  //
  // The cover is a visible, unowned, top-level window of this very process, so
  // it matches everything above -- and being topmost, EnumWindows hands it back
  // first. That made this return the cover rather than the dashboard: on the
  // way out the cover got minimised instead of the blade, and on the way back
  // the cover was restored and then destroyed, leaving the dashboard still
  // minimised and the screen black.
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  if (wcscmp(cls, kCoverClass) == 0) {
    return TRUE;
  }

  reinterpret_cast<FindState*>(param)->found = hwnd;
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


// Every top-level window the emulator owns, sorted into "the fullscreen one"
// and "everything else".
//
// Tracking only the first match was not enough: the emulator puts up more than
// one window on the way in -- a short-lived transparent one among them -- and
// anything not hidden shows through. Size is not a useful filter either, since
// that one is not the shape of a normal window, so every visible top-level
// window of the process is swept.
struct EmulatorWindows {
  DWORD pid = 0;
  long screen_w = 0;
  long screen_h = 0;
  HWND fullscreen = nullptr;
  HWND any = nullptr;   // fallback for the timeout path
  int hidden = 0;
};

BOOL CALLBACK SweepEmulatorProc(HWND hwnd, LPARAM param) {
  auto* state = reinterpret_cast<EmulatorWindows*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != state->pid || GetWindow(hwnd, GW_OWNER)) {
    return TRUE;
  }

  RECT rc{};
  if (!GetWindowRect(hwnd, &rc)) {
    return TRUE;
  }
  const long w = rc.right - rc.left;
  const long h = rc.bottom - rc.top;

  if (w >= state->screen_w - 2 && h >= state->screen_h - 2) {
    // Size alone, deliberately: a window parked off screen still reports its
    // real size, and the emulator moves it back to 0,0 as part of going
    // fullscreen, so position is not a reliable part of the test.
    state->fullscreen = hwnd;
    return TRUE;  // keep looking; there is only one, but do not stop early
  }

  // Anything that is not the fullscreen window has no business being on screen
  // during the hand-off.
  if (w >= 64 && h >= 64) {
    state->any = hwnd;  // the most window-shaped thing seen, if fullscreen never comes
  }

  // One line per distinct window, so whatever is still flashing can be named
  // rather than guessed at: its class, size, styles and whether it was up.
  {
    static std::set<HWND> seen;
    if (seen.size() < 24 && seen.insert(hwnd).second) {
      wchar_t cls[64] = {};
      GetClassNameW(hwnd, cls, 64);
      char narrow[64] = {};
      for (int i = 0; i < 63 && cls[i]; ++i) {
        narrow[i] = cls[i] < 128 ? char(cls[i]) : '?';
      }
      REXKRNL_INFO("Game launch: emulator window {:#x} class '{}' {}x{} at {},{} "
                   "style {:#010x} exstyle {:#010x} visible={}",
                   reinterpret_cast<uintptr_t>(hwnd), narrow, w, h, rc.left, rc.top,
                   static_cast<uint32_t>(GetWindowLongW(hwnd, GWL_STYLE)),
                   static_cast<uint32_t>(GetWindowLongW(hwnd, GWL_EXSTYLE)),
                   IsWindowVisible(hwnd) ? 1 : 0);
    }
  }

  // Moving it off screen was tried here and is deliberately not done: parked at
  // -30000 the window is on no monitor, the emulator's fullscreen step has
  // nothing to resolve against, and it never becomes fullscreen at all -- it
  // just runs in the background. The emulator's own window management is left
  // alone; only its visibility is touched.
  if (IsWindowVisible(hwnd)) {
    ShowWindow(hwnd, SW_HIDE);
    ++state->hidden;
  }
  return TRUE;
}

// Keep the emulator off screen until it is fullscreen.
//
// The cover was the wrong tool. It comes up correctly -- 1920x1080 at 0,0,
// visible -- and it still loses:
//
//     emulator window 1296x690 at 156,156  fullscreen=false cover_above=false
//     emulator window 1920x1080 at 0,0     fullscreen=true  cover_above=false
//
// WS_EX_TOPMOST only puts a window in the topmost band, and within that band the
// foreground window sits on top. The emulator takes the foreground as it starts,
// so it is above the cover from its first frame, and re-asserting the cover's
// position every few milliseconds cannot beat that. Worse, the nudge that
// restored and foregrounded the emulator was raising it above the cover on
// purpose.
//
// So the window is hidden outright while it is the wrong shape, and shown once
// it is the right one. Hiding is absolute where z-order is a negotiation, and
// it does not stop the emulator resizing itself: GetWindowRect still reports the
// window while it is hidden, which is what the wait watches.
HWND WaitForFullscreen(HWND cover, DWORD pid, HANDLE process) {
  const DWORD deadline = GetTickCount() + 20000;
  const long screen_w = GetSystemMetrics(SM_CXSCREEN);
  const long screen_h = GetSystemMetrics(SM_CYSCREEN);
  int total_hidden = 0;

  g_hook.pid = pid;
  g_hook.screen_w = screen_w;
  g_hook.screen_h = screen_h;
  HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
                                       OnWindowShown, pid, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  struct HookGuard {
    HWINEVENTHOOK h;
    ~HookGuard() {
      if (h) UnhookWinEvent(h);
    }
  } guard{hook};

  while (GetTickCount() < deadline) {
    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
      return nullptr;  // it exited before ever showing a window
    }

    EmulatorWindows state;
    state.pid = pid;
    state.screen_w = screen_w;
    state.screen_h = screen_h;
    EnumWindows(SweepEmulatorProc, reinterpret_cast<LPARAM>(&state));
    total_hidden += state.hidden;

    if (state.fullscreen) {
      // Nothing is shown until this point, which is the whole intent: the first
      // thing on screen is the game, already the right size.
      ShowWindow(state.fullscreen, SW_SHOW);
      SetForegroundWindow(state.fullscreen);
      SetWindowPos(state.fullscreen, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
      Sleep(250);
      REXKRNL_INFO("Game launch: emulator is fullscreen ({} window(s) hidden on the way in)",
                   total_hidden);
      return state.fullscreen;
    }

    if (cover) {
      SetWindowPos(cover, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    // Wake as soon as anything arrives -- the hook is delivered as a message,
    // so waiting on the queue is what makes the hide immediate rather than
    // merely frequent. The 4ms cap keeps the size polling going regardless.
    MsgWaitForMultipleObjects(1, &process, FALSE, 4, QS_ALLINPUT);
  }

  // Never leave the emulator hidden: whatever state it reached, it has to be
  // visible and reachable.
  EmulatorWindows state;
  state.pid = pid;
  state.screen_w = screen_w;
  state.screen_h = screen_h;
  EnumWindows(SweepEmulatorProc, reinterpret_cast<LPARAM>(&state));
  HWND show = state.fullscreen ? state.fullscreen : state.any;
  if (show) {
    ShowWindow(show, SW_SHOW);
    SetForegroundWindow(show);
  }
  REXKRNL_WARN("Game launch: the emulator did not reach fullscreen; showing it as-is");
  return show;
}

// Run the emulator and wait for it.
//
// Blocking is the point: the dashboard is behind a fullscreen emulator, and the
// call it is waiting on is the one that would have replaced it with the game.
// When that returns, the library is still exactly where it was.
// on_return runs after the dashboard window is back but before the cover comes
// down, so anything that has to happen before the screen is shown again is
// still hidden while it happens.
bool RunEmulator(const std::filesystem::path& image,
                 const std::function<void()>& on_return = {}, uint32_t title_id = 0) {
  const std::string emulator = REXCVAR_GET(game_emulator);
  std::error_code ec;
  if (emulator.empty() || !std::filesystem::exists(emulator, ec)) {
    REXKRNL_WARN("Game launch: no emulator at '{}'", emulator);
    return false;
  }

  // Discord hears about this before the emulator starts rather than after, so
  // the presence is already right by the time the game is on screen. It is a
  // lock and a signal; the worker does the talking.
  {
    const auto name = title_id ? nxe_title::NameFor(title_id) : std::u16string();
    nxe_discord::SetPlaying(title_id, rex::string::to_utf8(name));
  }

  std::wstring command = L"\"" + Widen(emulator) + L"\"";
  if (REXCVAR_GET(game_emulator_fullscreen)) {
    command += L" --fullscreen=true";
  }
  command += L" \"" + image.wstring() + L"\"";

  // Black out the display first, then move things underneath it.
  HWND cover = ShowCover();

  // Demoted, not minimised.
  //
  // Minimising it is what put the desktop behind the game, and every attempt to
  // cover that gap has been a race: the game's window is destroyed before its
  // process exits, and whatever is underneath shows in between. Leaving the
  // dashboard up and merely pushing it out of the topmost band means the thing
  // behind the game is the blade itself, so there is no gap to cover -- when
  // the game goes, the dashboard is already there.
  //
  // It still has to leave the topmost band: a fullscreen window that stays
  // topmost would sit over the emulator and hide the game entirely.
  HWND dash = DashboardWindow();
  if (dash) {
    SetWindowPos(dash, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }

  // Started normally, on purpose.
  //
  // Handing it SW_SHOWMINNOACTIVE did keep the default-sized window off screen,
  // but it also meant the window was never shown -- and the emulator's own
  // fullscreen step had nothing to act on, so the game ran minimised in the
  // background and had to be clicked out of the taskbar. Hiding the transition
  // is the cover's job; the emulator is left to start the way it expects to.
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring mutable_command = command;
  const auto working_dir = std::filesystem::path(emulator).parent_path().wstring();

  REXKRNL_INFO("Game launch: running {}", rex::string::to_utf8(std::u16string(
                                              command.begin(), command.end())));

  const BOOL started = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0,
                                      nullptr, working_dir.empty() ? nullptr : working_dir.c_str(),
                                      &si, &pi);
  if (!started) {
    REXKRNL_WARN("Game launch: could not start the emulator ({})", GetLastError());
    if (cover) {
      DestroyWindow(cover);
    }
    if (dash) {
      ShowWindow(dash, SW_RESTORE);
    }
    return false;
  }

  const HWND emulator_window = WaitForFullscreen(cover, pi.dwProcessId, pi.hProcess);

  // The cover stays up for the whole session, parked directly beneath the
  // emulator.
  //
  // Destroying it here was what put the desktop on screen for a beat when the
  // game quit: the emulator's window is gone before its process is, and the
  // replacement cover could only go up after the wait below returned. Leaving
  // this one in place means the moment the game's window disappears, the thing
  // revealed underneath is already black.
  //
  // Inserted after the emulator in z-order rather than left at the top, so the
  // game is in front of it and it is invisible while the game runs.
  if (cover && emulator_window) {
    SetWindowPos(cover, emulator_window, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }

  // Wait for the game, watching for its window to go rather than just its
  // process.
  //
  // The window is destroyed well before the process finishes exiting, and the
  // cover is parked underneath it at this point -- so a plain wait on the
  // process leaves whatever is behind the game on screen for that gap, which is
  // the desktop, because the dashboard is minimised. Raising the cover the
  // moment the window goes closes it: the game is replaced by black in the same
  // frame, and the wait then continues until the process is really gone.
  bool raised = false;
  while (WaitForSingleObject(pi.hProcess, 16) == WAIT_TIMEOUT) {
    if (!raised && emulator_window && (!IsWindow(emulator_window) ||
                                       !IsWindowVisible(emulator_window))) {
      raised = true;
      if (cover) {
        SetWindowPos(cover, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
      }
      REXKRNL_INFO("Game launch: the game's window closed; covering the hand-back");
    }
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  REXKRNL_INFO("Game launch: the emulator exited ({}); returning to the library", exit_code);

  // The cover from the launch is still up, so the screen is already black --
  // put it back on top in case the emulator's exit disturbed the order.
  if (cover) {
    SetWindowPos(cover, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  } else {
    cover = ShowCover();
  }

  // Back to the dashboard, on top, where the library still is. It was only
  // demoted, but restore anyway in case Windows minimised it when the
  // fullscreen window lost focus.
  dash = DashboardWindow();
  if (dash) {
    if (IsIconic(dash)) {
      ShowWindow(dash, SW_RESTORE);
    }
    SetWindowPos(dash, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(dash);
    SetFocus(dash);
  }

  // Still behind the cover: the disc path has to put the dashboard's own UI
  // back before anything is shown, because the tile tore it down on the way in.
  // Doing it after the cover came down was what put the desktop on screen.
  nxe_discord::SetDashboard();

  if (on_return) {
    on_return();
  }

  Sleep(200);  // let the blade paint before the cover comes off
  if (cover) {
    DestroyWindow(cover);
  }
  return true;
}

// The staged package for a title, found the same way the library finds it.
std::filesystem::path PackageForTitle(uint32_t title_id) {
  for (const auto& content : nxe_content::AllInstalledContent()) {
    if (uint32_t(content.title_id) == title_id) {
      return PackagePath(content);
    }
  }
  return {};
}

// Guest globals the restore sequence uses, read straight out of guest memory
// the same way 0x921F6808 does.
constexpr uint32_t kUiObjectPtr = 0x92800E48;   // dword_92800E48
constexpr uint32_t kInLaunchFlag = 0x928013BC;  // dword_928013BC
constexpr uint32_t kUiArgA = 0x9280A348;        // unk_9280A348
constexpr uint32_t kUiArgB = 0x92800DF8;        // unk_92800DF8

}  // namespace

namespace nxe_game {

void RestoreDashboardUi(PPCContext& ctx, uint8_t* base);

void NoteTitleShown(uint32_t title_id) {
  if (!title_id || nxe_content::IsSystemTitleId(title_id)) {
    return;
  }
  if (g_last_title.exchange(title_id) != title_id) {
    REXKRNL_INFO("Game launch: the library is showing title {:#010x}", title_id);
  }
}

uint32_t LastTitleShown() { return g_last_title.load(); }

// Filesystem only, deliberately.
//
// PackageForTitle above goes through the content manager, which needs the
// kernel and the signed-in profile. This one is also called while the devices
// are being mounted, long before either exists, so it walks the content tree
// the same way the resolver lays it out.
std::filesystem::path PackagePathForTitle(uint32_t title_id) {
  std::error_code ec;
  const auto root = nxe_storage::ContentRoot();
  if (!title_id || !std::filesystem::exists(root, ec)) {
    return {};
  }
  char title_dir[9] = {};
  std::snprintf(title_dir, sizeof(title_dir), "%08X", title_id);

  for (const auto& xuid : std::filesystem::directory_iterator(root, ec)) {
    if (!xuid.is_directory(ec)) {
      continue;
    }
    const auto type_dir = xuid.path() / title_dir / "00004000";
    if (!std::filesystem::exists(type_dir, ec)) {
      continue;
    }
    for (const auto& pkg : std::filesystem::directory_iterator(type_dir, ec)) {
      if (pkg.is_directory(ec)) {
        return pkg.path();
      }
    }
  }
  return {};
}

bool LaunchDiscTitle(PPCContext& ctx, uint8_t* base) {
  const std::string text = REXCVAR_GET(disc_title);
  if (text.empty()) {
    return false;
  }
  const auto title = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
  const auto staged = PackagePathForTitle(title);
  if (staged.empty()) {
    return false;
  }
  const auto image = ExecutableIn(staged);
  std::error_code ec;
  if (!std::filesystem::exists(image, ec)) {
    return false;
  }
  REXKRNL_INFO("Disc launch: title {:#010x} from '{}'", title, image.string());
  RunEmulator(image, [&] { RestoreDashboardUi(ctx, base); }, title);
  return true;
}

void RestoreDashboardUi(PPCContext& ctx, uint8_t* base) {
  // Only if the dashboard thinks its UI exists, which is the same condition it
  // used when tearing it down.
  __imp__sub_9243D558(ctx, base);
  if (!ctx.r3.u32) {
    return;
  }

  const uint32_t ui = *reinterpret_cast<const be<uint32_t>*>(base + kUiObjectPtr);

  // sub_92159B60(ui) -- the counterpart to the sub_92159AC8(ui) that tore it
  // down before the launch.
  ctx.r3.u64 = ui;
  __imp__sub_92159B60(ctx, base);

  *reinterpret_cast<be<uint32_t>*>(base + kInLaunchFlag) = 0;

  // sub_9215FBC0(ui, 0, &unk_9280A348) and sub_921405B0(&unk_92800DF8), the two
  // calls 0x921F6808 makes to finish coming back.
  ctx.r3.u64 = ui;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = kUiArgA;
  __imp__sub_9215FBC0(ctx, base);

  ctx.r3.u64 = kUiArgB;
  __imp__sub_921405B0(ctx, base);

  REXKRNL_INFO("Disc launch: dashboard UI restored");
}

}  // namespace nxe_game

// sub_92278F80(item, user, ?, ?) -- the Play Game handler.
//
// The press produces no kernel calls whatsoever: the log shows the achievement
// screen, then nothing until the five-second device poll ticks. So the refusal
// is decided entirely from memory the dashboard already holds, and no kernel
// hook can see it, let alone change it. For content type 0x4000 the handler is
//
//     case 0x4000u:
//       if ( !*(_BYTE *)(a1 + 14) || !v7 || sub_922749A0(v7) )
//         break;                        // -> E_FAIL -> "Can't Play Game"
//       if ( !sub_92278850(v7 + 66) )   // the licence check, never reached
//       ...
//
// and sub_922749A0 only rejects a wholly empty record, so the break is one of
// the two plain memory reads before it.
//
// Overriding the handler is the only remaining lever. Whether it takes depends
// on how the press dispatches: the generated file calls this function directly
// four times, and a direct call inside a translation unit binds locally, so an
// external definition is ignored for those. A press routed through the UI's
// command table is an indirect call, which goes through the registered function
// pointer -- and that resolves here. If this never logs, the press is a direct
// call and this approach is dead.
extern "C" void sub_92278F80(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t item = ctx.r3.u32;
  uint32_t kind = 0;
  if (item) {
    kind = *reinterpret_cast<const be<uint32_t>*>(base + item + 16);
  }

  // Only installed games are taken over.
  //
  // This handler serves every kind the Games blade can hold -- demos (0x80000),
  // Xbox 1 titles (0x5000), arcade (0x7000), the marketplace kinds (0xD0000,
  // 0xE0000) and more. Replacing it wholesale would silently break all of them,
  // so anything that is not an installed game (0x4000) runs its own code.
  constexpr uint32_t kInstalledGame = 0x4000;
  if (kind != kInstalledGame) {
    __imp__sub_92278F80(ctx, base);
    return;
  }

  const uint32_t title = nxe_game::LastTitleShown();
  const auto staged = title ? PackageForTitle(title) : std::filesystem::path{};
  std::error_code ec;
  const auto image = staged.empty() ? std::filesystem::path{} : ExecutableIn(staged);

  if (image.empty() || !std::filesystem::exists(image, ec)) {
    // Nothing of ours to run, so let the dashboard have its own say rather than
    // swallowing the press.
    REXKRNL_WARN("Play Game: nothing staged for title {:#010x}; deferring to the dashboard",
                 title);
    __imp__sub_92278F80(ctx, base);
    return;
  }

  REXKRNL_INFO("Play Game: launching title {:#010x} from '{}'", title, image.string());
  RunEmulator(image, {}, title);
  ctx.r3.u64 = 0;
}

// ExExpansionCall -- what the licence check actually asks.
//
// Guest 0x92278850 decides whether a game may be played:
//
//     if ( ExExpansionCall(0x504C4159 /* 'PLAY' */, 1, 0, 0, 0) >= 0 ) return 1;
//     ...
//     snprintf(v5, 0x40, "%08X%08X%08X%08X", media_id);
//     return strcmp(v5, file_name) == 0;
//
// and 0x92278F80, the Play Game handler, shows "You can only play this game on
// the console and storage device it was originally installed on" when it says
// no -- then asks a second time and launches if the answer changed. That second
// question is why the dialog appeared and the game still tried to boot.
//
// It ships as a bare REX_EXPORT_STUB, and REX_STUB does not assign r3, so the
// answer was an uninitialised register: sometimes non-negative and the game ran,
// sometimes negative and the dialog appeared. The same press genuinely did
// different things on different runs, which is what made this so hard to pin
// down from the outside.
//
// The fallback it guards is unsatisfiable here regardless. A disc-installed game
// is staged in a folder named after that disc's media id, and the comparison is
// against the media id of the disc in the tray. There is no disc, and the
// package is called "Halo 3", so the string compare can never match however the
// content is staged.
//
// Answered deterministically instead. Nothing else in this port calls
// ExExpansionCall -- it appears in the logs only on this path -- so a settled
// answer here costs nothing elsewhere, and any settled answer is better than the
// register lottery it replaces.
REX_HOOK_RAW(__imp__ExExpansionCall) {
  const uint32_t tag = ctx.r3.u32;
  static uint32_t s_logged = 0;
  if (s_logged != tag) {
    s_logged = tag;
    char fourcc[5] = {char(tag >> 24), char(tag >> 16), char(tag >> 8), char(tag), 0};
    REXKRNL_INFO("ExExpansionCall({:#010x} '{}') -> 0", tag, fourcc);
  }
  ctx.r3.u64 = 0;
}

// XamContentLaunchImageInternal(content_or_path, xex_name)
//
// The first argument is either a content record or a path string depending on
// how the caller resolved the item, so both are accepted rather than assuming
// one. Success is reported either way: the stub's undefined r3 is what produced
// the refusal dialog, and there is nothing further downstream that could honour
// a real title switch.
REX_HOOK_RAW(__imp__XamContentLaunchImageInternal) {
  const uint32_t first = ctx.r3.u32;
  const std::string name = GuestString(base, ctx.r4.u32, 64);

  // What is actually at r3.
  //
  // The first attempt read it as a content record and decoded nonsense --
  // title 0x0020004E, a name of "mpaign Complete:  Normal" -- because those are
  // UTF-16 code units, not record fields, and the ASCII test missed a wide
  // string for exactly the reason wide strings look empty to it: every other
  // byte is zero. Dump the head of the buffer so the shape is settled from what
  // is there rather than from what it was assumed to be.
  if (first) {
    const uint8_t* raw = base + first;
    std::string hex, ascii, wide;
    for (int i = 0; i < 48; ++i) {
      char b[4];
      std::snprintf(b, sizeof(b), "%02x ", raw[i]);
      hex += b;
      ascii += (raw[i] >= 0x20 && raw[i] < 0x7F) ? static_cast<char>(raw[i]) : '.';
    }
    for (int i = 0; i < 48; i += 2) {
      const uint16_t ch = (uint16_t(raw[i]) << 8) | raw[i + 1];
      wide += (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '.';
    }
    REXKRNL_INFO("Game launch: r3 bytes  {}", hex);
    REXKRNL_INFO("Game launch: r3 ascii  '{}'", ascii);
    REXKRNL_INFO("Game launch: r3 utf16  '{}'", wide);
  }

  std::filesystem::path image;
  std::string described;

  // A UTF-16 path, which is what the wide dump above showed it to be.
  std::string wide_text;
  if (first) {
    const auto* p16 = reinterpret_cast<const be<uint16_t>*>(base + first);
    for (size_t i = 0; i < 260; ++i) {
      const uint16_t ch = p16[i];
      if (!ch) break;
      if (ch < 0x20 || ch > 0x7E) { wide_text.clear(); break; }
      wide_text.push_back(static_cast<char>(ch));
    }
  }

  const std::string as_text = GuestString(base, first);
  if (!wide_text.empty()) {
    described = wide_text;
    image = ExecutableIn(wide_text);
  } else if (LooksLikeText(as_text)) {
    described = as_text;
    image = ExecutableIn(as_text);
  } else if (first) {
    const auto* content = reinterpret_cast<const XCONTENT_AGGREGATE_DATA*>(base + first);
    const auto dir = PackagePath(*content);
    described = dir.string();
    image = ExecutableIn(dir);
    REXKRNL_INFO("Game launch: title {:#010x} '{}'", uint32_t(content->title_id),
                 rex::string::to_utf8(content->display_name()));
  }

  REXKRNL_INFO("XamContentLaunchImageInternal: r3={:#x} name='{}' -> '{}'", first, name,
               image.string());

  // Fall back to the title the library is showing.
  //
  // Whatever r3 points at, it is not a content record and not an ASCII path, so
  // anything derived from it can be wrong without being obviously wrong -- the
  // first attempt built "Content\006E0020006F006E\..." out of UTF-16 code
  // units and looked plausible until the path was checked. The title the detail
  // page opened is not a guess, so it wins whenever the argument does not
  // resolve to something that exists.
  std::error_code ec;
  if (image.empty() || !std::filesystem::exists(image, ec)) {
    const uint32_t title = nxe_game::LastTitleShown();
    const auto staged = title ? PackageForTitle(title) : std::filesystem::path{};
    if (!staged.empty()) {
      image = ExecutableIn(staged);
      REXKRNL_INFO("Game launch: falling back to the shown title {:#010x} -> '{}'", title,
                   image.string());
    }
  }

  if (image.empty() || !std::filesystem::exists(image, ec)) {
    REXKRNL_WARN("Game launch: nothing to run for '{}' (last shown title {:#010x})", described,
                 nxe_game::LastTitleShown());
  } else {
    RunEmulator(image, {}, nxe_game::LastTitleShown());
  }

  ctx.r3.u64 = X_ERROR_SUCCESS;
}
