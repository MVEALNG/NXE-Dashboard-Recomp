// "Customize Avatar" -> the recompiled Avatar Editor.
//
// On a console that menu item is a title switch: the dashboard hands off to the
// Avatar Editor (its own XEX, title 584D07D1) and exits.
//
// It does not do that through XamLoaderLaunchTitle, which is the obvious guess
// and the wrong one. sub_922E8F98 is the handler, and its launch path is:
//
//     XamPackageManagerGetExperienceMode(&mask)     ; needs bit 0x2 set
//     XamPackageManagerFindPackageContainingIndexedXEX(name, buf, 260)
//     XamLoaderSetLaunchData(...)
//     XamContentLaunchImageFromFileInternal(buf, name)
//
// with an "this needs a system update" message box on the else branch. The gate
// is implemented in the SDK (xam_misc.cpp); this file handles the last line.
//
// When the requested image is the Avatar Editor, the recompiled editor is
// started as its own process and the dashboard is left running, so coming back
// is instant rather than a cold boot. Anything else falls through to the SDK's
// own implementation unchanged.
//
// The two XamLoaderLaunchTitle entry points are hooked as well. Nothing has been
// observed reaching them, but they are the other way a title switch can be
// spelled, and the SDK's version of the first one ends in TerminateTitle -- so
// if some other path does use them, the dashboard should survive it.
//
// Why a separate process rather than one image: the two titles occupy the same
// guest addresses. The dashboard's own Customize Avatar handler is sub_922E8F98
// and the editor's navigation state sits at 0x922E7604 -- both 0x922Exxxx, two
// different binaries mapped over each other. One address space cannot hold both
// without regenerating one of the recompilations at a different base. They also
// build against different SDKs (the editor bundles 0.8.0 with the native
// videonative renderer; this project uses 0.9.0's emulated GPU), so they cannot
// even share a runtime DLL.
//
// So the handoff stays a process launch -- which is what the console does
// semantically anyway, one title handing off to another -- and the seam is
// closed at the window level instead: the editor's window is reparented into
// the dashboard's own window and sized to fill it, so it reads as one
// application making one transition. When the editor exits, its window goes
// with it and the dashboard is simply there again, still running, no reboot.

#include <cstdint>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include <thread>
#include <vector>

#include "game_launch.h"
#include "storage_device.h"
#include "profile_list.h"

using namespace rex;

// Where the recompiled editor lives. Empty disables the redirect, which leaves
// the console behaviour (terminate) in place.
// Which notifications to raise when the saved avatar changes.
//
// Empty by default, because the obvious candidate is wrong. 0xE
// (XN_SYS_PROFILESETTINGCHANGED, in the numbering KernelState uses for the
// startup notifications it sends) does reach the dashboard -- and the dashboard
// answers it by shutting down:
//
//     BroadcastNotification(id=0xe, data=0) to 5 listeners
//     XamGetDashContext / XamSetDashContext
//     HalRegisterPowerDownCallback           <- on its way out
//
// which is a reasonable thing for a console to do about a profile that changed
// underneath it (reboot and re-read everything) and a useless thing here.
//
// Nothing is needed in its place: GetAssets adopts the new manifest on the next
// avatar rebuild, and the dashboard rebuilds whenever the blade is redrawn --
// which is what returning from the editor and moving around does. Left as a
// cvar so another id can be tried without a rebuild if a case turns up that
// really does sit still.
REXCVAR_DEFINE_STRING(avatar_change_notify_ids, "0xA", "Avatar",
                      "Notification ids broadcast when the saved avatar manifest changes. "
                      "Comma-separated; empty (the default) broadcasts nothing. Note 0xE "
                      "makes the dashboard power down.");

// Reparent the editor into this window rather than letting it open its own.
// Off gives you two ordinary windows, which is the thing to try first if the
// editor ever comes up blank or refuses input.
REXCVAR_DEFINE_BOOL(avatar_editor_embed, true, "Avatar",
                    "Host the Avatar Editor inside the dashboard window so the switch reads "
                    "as one application rather than a second one opening.");

// Empty unless someone has the avatar editor recompilation built; it is a
// separate project, not part of this one.
REXCVAR_DEFINE_STRING(
    avatar_editor_exe, "", "Avatar",
    "Executable launched when the dashboard asks to launch the Avatar Editor. Empty "
    "leaves the button inert.");

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

// The SDK's own implementation, out of the runtime DLL.
//
// Not FindPPCFuncByName: the hooks below define these symbols strongly, which
// is what makes them win, and a name lookup would find this file's hook rather
// than the implementation it means to fall through to.
GuestFunc RuntimeFunc(const char* name) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  HMODULE module = GetModuleHandleA("rexruntimed.dll");
  if (module == nullptr) {
    module = GetModuleHandleA("rexruntime.dll");
  }
  return module ? reinterpret_cast<GuestFunc>(
                      reinterpret_cast<void*>(GetProcAddress(module, name)))
                : nullptr;
}

// A NUL-terminated guest string, bounded so a bad pointer cannot run away.
std::string GuestString(uint8_t* base, uint32_t ptr) {
  if (!ptr) {
    return {};
  }
  const char* p = reinterpret_cast<const char*>(base + ptr);
  std::string out;
  for (size_t i = 0; i < 260 && p[i]; ++i) {
    out.push_back(p[i]);
  }
  return out;
}

// Is this launch request the Avatar Editor?
//
// The dashboard names it by path rather than title id, and which path depends
// on how the console it came from stored it, so match on the name rather than
// pinning one string. 584D07D1 is the editor's title id and appears in content
// paths; "avatar" covers $flash_avatareditor.xex and avatareditor.xex.
bool IsAvatarEditor(const std::string& path) {
  std::string lower;
  lower.reserve(path.size());
  for (char c : path) {
    lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(c))));
  }
  return lower.find("avatar") != std::string::npos ||
         lower.find("584d07d1") != std::string::npos;
}

// The one top-level, visible window belonging to a process.
//
// Both processes are asked the same way. A recompiled title puts up exactly one
// such window, so the first match is the right one -- but it does not exist the
// instant CreateProcess returns, which is what the retry in HostEditorWindow is
// for.
struct FindWindowState {
  DWORD pid = 0;
  HWND found = nullptr;
};

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM param) {
  auto* state = reinterpret_cast<FindWindowState*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != state->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
    return TRUE;
  }
  state->found = hwnd;
  return FALSE;
}

HWND MainWindowOf(DWORD pid) {
  FindWindowState state;
  state.pid = pid;
  EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&state));
  return state.found;
}

// Pull the editor's window into ours and size it to the client area.
//
// Cross-process SetParent is allowed and is what makes the switch look like one
// application: the editor keeps its own message loop and swap chain, it just
// draws inside our frame. Its own title bar and borders have to go, or a second
// chrome appears floating inside the dashboard.
//
// Best effort throughout. Every failure here leaves a working editor in its own
// window, which is worse-looking but not broken, so nothing below is fatal.
void HostEditorWindow(const PROCESS_INFORMATION& process) {
  if (!REXCVAR_GET(avatar_editor_embed)) {
    return;
  }
  HWND host = MainWindowOf(GetCurrentProcessId());
  if (!host) {
    REXKRNL_WARN("Avatar editor: no dashboard window to host it in; leaving it standalone");
    return;
  }

  // The window is created some way into the editor's startup, well after the
  // process exists. WaitForInputIdle covers most of it; the loop covers a cold
  // start where it is still loading its asset pack.
  WaitForInputIdle(process.hProcess, 10000);
  HWND guest = nullptr;
  for (int attempt = 0; attempt < 100 && !guest; ++attempt) {
    DWORD status = 0;
    if (GetExitCodeProcess(process.hProcess, &status) && status != STILL_ACTIVE) {
      REXKRNL_ERROR("Avatar editor: exited during startup (code {})", status);
      return;
    }
    guest = MainWindowOf(process.dwProcessId);
    if (!guest) {
      Sleep(100);
    }
  }
  if (!guest) {
    REXKRNL_WARN("Avatar editor: its window never appeared; leaving it standalone");
    return;
  }

  RECT client{};
  GetClientRect(host, &client);

  SetWindowLongPtrA(guest, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
  SetWindowLongPtrA(guest, GWL_EXSTYLE, 0);
  if (!SetParent(guest, host)) {
    REXKRNL_WARN("Avatar editor: SetParent failed ({}); leaving it standalone", GetLastError());
    return;
  }
  SetWindowPos(guest, HWND_TOP, 0, 0, client.right - client.left, client.bottom - client.top,
               SWP_SHOWWINDOW | SWP_FRAMECHANGED);
  SetForegroundWindow(host);
  SetFocus(guest);
  REXKRNL_INFO("Avatar editor: hosted in the dashboard window ({}x{})",
               client.right - client.left, client.bottom - client.top);
}

// Start the editor, once. A second press while it is already up should focus
// the existing window rather than spawn a duplicate; there is no way to focus
// it from here, so this at least does not stack processes.
bool LaunchEditor() {
  static std::mutex mutex;
  static PROCESS_INFORMATION s_process{};
  std::lock_guard<std::mutex> lock(mutex);

  if (s_process.hProcess) {
    DWORD status = 0;
    if (GetExitCodeProcess(s_process.hProcess, &status) && status == STILL_ACTIVE) {
      REXKRNL_INFO("Avatar editor: already running (pid {})", s_process.dwProcessId);
      return true;
    }
    CloseHandle(s_process.hProcess);
    CloseHandle(s_process.hThread);
    s_process = {};
  }

  const std::string exe = REXCVAR_GET(avatar_editor_exe);
  if (exe.empty()) {
    REXKRNL_WARN("Avatar editor: avatar_editor_exe is empty; not launching");
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::exists(exe, ec)) {
    REXKRNL_ERROR("Avatar editor: {} does not exist; build AvatarEditorRecomp or set "
                  "avatar_editor_exe",
                  exe);
    return false;
  }

  // Start it in its own directory: it resolves assets, config and its shader
  // pack relative to the working directory, so launching it from here would
  // leave it looking for them beside the dashboard.
  const std::string directory = std::filesystem::path(exe).parent_path().string();

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::string command = "\"" + exe + "\"";

  // Point the editor at the profile that is actually signed in.
  //
  // Its avatareditor.toml carries an avatar_data_dir of its own, but that is a
  // fixed path and cannot know which profile the dashboard staged. Passing it
  // on the command line means the editor always loads and saves the current
  // profile's avatar, and each profile keeps its own.
  const std::string data_dir = rex::cvar::GetFlagByName("avatar_data_dir");
  if (!data_dir.empty()) {
    command += " --avatar_data_dir \"" + data_dir + "\"";
  }

  if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      directory.c_str(), &startup, &process)) {
    REXKRNL_ERROR("Avatar editor: CreateProcess failed ({}) for {}", GetLastError(), exe);
    return false;
  }

  s_process = process;
  REXKRNL_WARN("Avatar editor: launched {} (pid {})", exe, process.dwProcessId);
  HostEditorWindow(process);
  return true;
}

// Shared body for both loader entry points.
//
// Returning without forwarding is deliberate: the SDK's implementation ends in
// TerminateTitle, and the dashboard should stay up. The guest treats the call
// as one that does not return, so it does not inspect a result.
void HandleLaunch(const char* who, const char* sdk_symbol, PPCContext& ctx, uint8_t* base) {
  const std::string path = GuestString(base, ctx.r3.u32);
  REXKRNL_INFO("{}: path='{}' flags={:#x}", who, path, ctx.r4.u32);

  if (IsAvatarEditor(path) && LaunchEditor()) {
    ctx.r3.u64 = X_ERROR_SUCCESS;
    return;
  }

  // Booting what is in the tray.
  //
  // The disc tile activates through dash_2948 -> dash_2947 -> here, and the
  // launch it asks for names no image: the disc is implied. An empty path, or
  // one naming the optical device, is therefore the disc being started, and
  // this port runs it the same way the Game Library does.
  if ((path.empty() || path.find("Cdrom0") != std::string::npos) &&
      nxe_game::LaunchDiscTitle(ctx, base)) {
    // The UI restore happens inside, while the screen is still covered: the
    // tile tore the dashboard down before calling this and, unlike the
    // library's wrapper at 0x921F6808, has no restore of its own.
    ctx.r3.u64 = X_ERROR_SUCCESS;
    return;
  }

  if (auto* sdk = RuntimeFunc(sdk_symbol)) {
    sdk(ctx, base);
    return;
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// The shared manifest, the same file the SDK's AvatarManifestPath() resolves to
// and the same one the editor's avatar_data_dir now points at.
std::filesystem::path ManifestPath() {
  const std::string dir = rex::cvar::GetFlagByName("avatar_data_dir");
  if (!dir.empty()) {
    return std::filesystem::path(dir) / "avatar_manifest.bin";
  }
  return nxe_storage::ContentRoot() / "avatars" / "avatar_manifest.bin";
}

std::vector<uint32_t> NotifyIds() {
  std::vector<uint32_t> ids;
  const std::string spec = REXCVAR_GET(avatar_change_notify_ids);
  size_t at = 0;
  while (at < spec.size()) {
    size_t end = spec.find(',', at);
    if (end == std::string::npos) {
      end = spec.size();
    }
    std::string one = spec.substr(at, end - at);
    try {
      if (!one.empty()) {
        ids.push_back(static_cast<uint32_t>(std::stoul(one, nullptr, 0)));
      }
    } catch (const std::exception&) {
      REXKRNL_WARN("Avatar: ignoring unparsable notification id '{}'", one);
    }
    at = end + 1;
  }
  return ids;
}

// Poll the manifest and broadcast when it changes.
//
// Polling rather than a directory watch: it is one stat a second against a file
// that is written at most a few times a session, and it stays correct when the
// file is replaced rather than modified in place -- which is what a save that
// writes to a temporary and renames would look like.
void WatchManifest() {
  // Resolved every iteration, not once.
  //
  // This thread starts at load, before the app has had a chance to point
  // avatar_data_dir at the signed-in profile's folder, so resolving the path up
  // front watched the shared avatars folder instead -- a file the dashboard no
  // longer writes. The watch never fired, and an avatar saved in the editor was
  // only picked up if something else happened to rebuild it.
  std::filesystem::path path;
  std::filesystem::file_time_type last{};
  bool primed = false;

  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    const auto current = ManifestPath();
    if (current != path) {
      path = current;   // profile changed, or it is now known at all
      primed = false;
    }

    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
      continue;
    }
    if (!primed) {
      // Whatever is on disk at startup is what the dashboard already read.
      last = stamp;
      primed = true;
      continue;
    }
    if (stamp == last) {
      continue;
    }
    last = stamp;

    auto* kernel = REX_KERNEL_STATE();
    if (!kernel) {
      continue;
    }

    // The blade renders the avatar from profile setting 0x63E80044, which is
    // filled at startup and otherwise never changes -- so telling the guest to
    // redraw is not enough on its own; the value behind it has to be updated
    // first, from the same file the editor just wrote.
    nxe_profile::RefreshAvatarSetting();

    const auto ids = NotifyIds();
    REXKRNL_WARN("Avatar: the saved avatar changed; it will be picked up on the next avatar "
                 "rebuild ({} notification(s) to raise)",
                 ids.size());
    for (uint32_t id : ids) {
      kernel->BroadcastNotification(id, 0);
    }
  }
}

// Started detached at load. It does nothing but sleep until the file moves, and
// it has to outlive any particular title state, so there is nothing to join.
struct ManifestWatchStarter {
  ManifestWatchStarter() { std::thread(WatchManifest).detach(); }
};
ManifestWatchStarter g_manifest_watch;

}  // namespace

REX_HOOK_RAW(__imp__XamLoaderLaunchTitle) {
  HandleLaunch("XamLoaderLaunchTitle", "__imp__XamLoaderLaunchTitle", ctx, base);
}

REX_HOOK_RAW(__imp__XamLoaderLaunchTitleEx) {
  HandleLaunch("XamLoaderLaunchTitleEx", "__imp__XamLoaderLaunchTitleEx", ctx, base);
}

// XamContentLaunchImageFromFileInternal(image_path, xex_name)
//
// This is the one Customize Avatar reaches. Both arguments are checked: the
// path is what the package manager resolved, the name is what the dashboard
// asked for, and either can carry the identifying string depending on how the
// package lookup answered.
//
// It is a bare REX_EXPORT_STUB in the SDK, so falling through does nothing at
// all -- which is why this returns success rather than forwarding when it does
// not recognise the image. Nothing downstream can honour the launch either way,
// and an undefined r3 from a stub is what produced the last round of confusion.
REX_HOOK_RAW(__imp__XamContentLaunchImageFromFileInternal) {
  const std::string path = GuestString(base, ctx.r3.u32);
  const std::string name = GuestString(base, ctx.r4.u32);
  REXKRNL_INFO("XamContentLaunchImageFromFileInternal: image='{}' name='{}'", path, name);

  if ((IsAvatarEditor(path) || IsAvatarEditor(name)) && LaunchEditor()) {
    ctx.r3.u64 = X_ERROR_SUCCESS;
    return;
  }

  REXKRNL_WARN("XamContentLaunchImageFromFileInternal: nothing to launch for '{}' / '{}'", path,
               name);
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
