#include "install_paths.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Where everything else is measured from.
//
// Empty means "the directory the executable is in", which is what a checkout
// wants: gamedir, storage and assets all sit beside the binary. Point it
// somewhere else to keep the data apart from the build.
REXCVAR_DEFINE_STRING(nxe_root, "", "Paths",
                      "Installation root. Empty means the executable's own directory.");

namespace nxe_paths {

const std::filesystem::path& Root() {
  static const std::filesystem::path root = [] {
    const std::string configured = REXCVAR_GET(nxe_root);
    if (!configured.empty()) {
      return std::filesystem::path(configured);
    }

    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
      // Nothing better to offer than the working directory.
      return std::filesystem::current_path();
    }
    return std::filesystem::path(buffer).parent_path();
  }();
  return root;
}

std::filesystem::path Resolve(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  std::filesystem::path candidate(path);
  if (candidate.is_absolute()) {
    return candidate;
  }
  return Root() / candidate;
}

}  // namespace nxe_paths
