// Picking a path, and running a tool, for the setup dialogs.
//
// Moved out of first_run.cpp when the disc dialog needed the same three. The
// quoting in RunTool is the reason: one copy of it can be got right, two copies
// drift.

#include "setup_tools.h"

#include <windows.h>
#include <shobjidl.h>

#include <vector>

#include <rex/logging.h>

namespace nxe_setup {


// The Windows file or folder picker, or empty if it was cancelled.
//
// IFileDialog rather than SHBrowseForFolder: the old one gives a tree with no
// path box and no way to paste, which is precisely what somebody with a path
// already on their clipboard wants to do. The same dialog does both, so which
// one is offered is one flag rather than two code paths -- and asking for a
// folder when the answer is an executable is its own small confusion.
std::string PickPath(const std::string& title, bool folder) {
  std::string chosen;
  IFileDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    return chosen;
  }

  DWORD options = 0;
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    options |= FOS_FORCEFILESYSTEM;
    if (folder) {
      options |= FOS_PICKFOLDERS;
    }
    dialog->SetOptions(options);
  }
  const std::wstring wide(title.begin(), title.end());
  dialog->SetTitle(wide.c_str());

  if (SUCCEEDED(dialog->Show(nullptr))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        const int size =
            WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
          chosen.resize(size_t(size) - 1);
          WideCharToMultiByte(CP_UTF8, 0, path, -1, chosen.data(), size, nullptr, nullptr);
        }
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return chosen;
}

// Where the Python tools are, from wherever the dashboard was started.
//
// Beside the executable in a release, and four levels up in a build tree, so
// this looks for the folder rather than assuming either. xbl_auth.py is the
// marker because every tool that talks to Xbox LIVE imports it -- a "tools"
// folder without it is somebody else's.
std::filesystem::path ToolsDir() {
  std::error_code ec;
  auto here = std::filesystem::current_path(ec);
  for (int up = 0; up < 6 && !here.empty(); ++up) {
    const auto candidate = here / "tools";
    if (std::filesystem::exists(candidate / "xbl_auth.py", ec)) {
      return candidate;
    }
    if (!here.has_parent_path() || here.parent_path() == here) {
      break;
    }
    here = here.parent_path();
  }
  return {};
}

// Run one of the tools, in a console the person can see.
//
// Visible on purpose. Signing in is a device-code flow: fetch_profile.py prints
// an eight-character code and a link to microsoft.com/link and waits for it to
// be approved. A hidden window would hang for ever with the code no one can
// read. cmd /k rather than /c so the window stays up afterwards and the result
// -- or the Python that is not installed -- can be read.
bool RunTool(const std::string& script, const std::string& args) {
  const auto tools = ToolsDir();
  if (tools.empty()) {
    REXLOG_ERROR("Setup: cannot find the tools folder; run {} yourself", script);
    return false;
  }

  // py -3 is the Windows launcher and is what is there on a machine with
  // Python installed from python.org; python is the fallback for a PATH that
  // has one but not the other. Quoted because the path holds spaces.
  std::string command = "cmd.exe /k \"cd /d \"" + tools.parent_path().string() +
                        "\" && (py -3 \"tools\\" + script + "\" " + args +
                        " || python \"tools\\" + script + "\" " + args + ")\"";

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back(0);

  const BOOL ok = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                                 CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process);
  if (!ok) {
    REXLOG_ERROR("Setup: could not start {} (error {})", script, GetLastError());
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  REXLOG_INFO("Setup: started {} in its own window", script);
  return true;
}

}  // namespace nxe_setup
