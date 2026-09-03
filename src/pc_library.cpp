// Finding installed PC games, and starting them.
//
// Two shapes, and they want completely different treatment.
//
// A Steam library says what it holds. steamapps/appmanifest_<appid>.acf carries
// the app id, the name Steam shows and the folder it installed into, so nothing
// has to be guessed -- and starting a game is steam://rungameid/<appid>, which
// is also the only way to start one that expects Steam to be running. Hunting
// for an executable in a Steam game would be worse in every respect: Batman
// Arkham City keeps its at Binaries/Win32/BatmanAC.exe, three levels down and
// named nothing like the folder.
//
// A plain folder of games has none of that, so there the executable has to be
// found. The heuristic is deliberately conservative and says so when it fails,
// because silently picking UnityCrashHandler64.exe and calling it a game is
// worse than admitting the game could not be identified.

#include "pc_library.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "install_paths.h"
#include "storage_device.h"

REXCVAR_DECLARE(std::string, full_games_dir);
REXCVAR_DECLARE(std::string, full_games_dir_2);
REXCVAR_DECLARE(std::string, full_games_dir_3);
REXCVAR_DECLARE(std::string, full_games_dir_4);

namespace nxe_pc {
namespace {

// Programs that ship beside a game and are not the game.
//
// Launching one of these instead is the failure mode worth guarding: they sit
// in the same folder, they are executables, and some sort ahead of the real one
// alphabetically.
const char* kNotTheGame[] = {
    "unitycrashhandler", "crashhandler",  "crashreport",   "unins",
    "setup",             "install",       "redist",        "vcredist",
    "dxsetup",           "directx",       "launcher_",     "dotnetfx",
    "eossdk",            "steamerrorrep", "activationui",  "touchup",
};

std::string Lowered(std::string text) {
  for (char& ch : text) {
    ch = char(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

bool LooksLikeAGame(const std::filesystem::path& exe) {
  const std::string name = Lowered(exe.stem().string());
  for (const char* bad : kNotTheGame) {
    if (name.find(bad) != std::string::npos) {
      return false;
    }
  }
  return true;
}

// One value out of a Valve key-value file: "key" <tabs> "value".
std::string AcfValue(const std::string& text, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t at = text.find(needle);
  if (at == std::string::npos) {
    return {};
  }
  at = text.find('"', at + needle.size());
  if (at == std::string::npos) {
    return {};
  }
  const size_t end = text.find('"', at + 1);
  return end == std::string::npos ? std::string() : text.substr(at + 1, end - at - 1);
}

// The steamapps folder for a setting that might point at any level of one.
//
// People point at the library root, at steamapps, or at steamapps/common --
// all three are reasonable readings of "my Steam games", so all three work.
std::filesystem::path SteamAppsIn(const std::filesystem::path& dir) {
  std::error_code ec;
  if (std::filesystem::exists(dir / "steamapps", ec)) {
    return dir / "steamapps";
  }
  if (Lowered(dir.filename().string()) == "steamapps") {
    return dir;
  }
  if (Lowered(dir.filename().string()) == "common" &&
      Lowered(dir.parent_path().filename().string()) == "steamapps") {
    return dir.parent_path();
  }
  return {};
}

std::vector<Game> ScanSteam(const std::filesystem::path& steamapps) {
  std::vector<Game> found;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(steamapps, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind("appmanifest_", 0) != 0 || entry.path().extension() != ".acf") {
      continue;
    }
    std::ifstream file(entry.path());
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

    Game game;
    game.kind = Kind::kSteam;
    game.steam_appid = AcfValue(text, "appid");
    game.name = AcfValue(text, "name");
    const std::string install = AcfValue(text, "installdir");
    if (!install.empty()) {
      game.root = steamapps / "common" / install;
    }
    // A manifest with no app id cannot be started, and one with no name has
    // nothing to show; either way it is not a game this can offer.
    if (game.steam_appid.empty() || game.name.empty()) {
      continue;
    }
    // Steam keeps manifests for things that are not games -- redistributables,
    // the Steamworks runtime -- and they have no install folder on disk.
    if (!game.root.empty() && !std::filesystem::exists(game.root, ec)) {
      continue;
    }
    found.push_back(std::move(game));
  }
  return found;
}

// The most likely executable in a game's folder, or empty if none stands out.
std::filesystem::path ExecutableFor(const std::filesystem::path& folder) {
  std::error_code ec;
  const std::string wanted = Lowered(folder.filename().string());

  std::filesystem::path best;
  uintmax_t best_size = 0;
  bool best_named = false;

  // Two levels: a game is usually at the root or one folder in (bin/, Binaries/).
  // Deeper than that and the guessing is worse than useless.
  for (auto it = std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    if (it.depth() > 2) {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec) || Lowered(it->path().extension().string()) != ".exe") {
      continue;
    }
    if (!LooksLikeAGame(it->path())) {
      continue;
    }

    const std::string stem = Lowered(it->path().stem().string());
    // A name matching the folder beats anything larger: "BatmanAC.exe" in
    // "Batman Arkham City" is the game, whatever else is bigger.
    const bool named = wanted.find(stem) != std::string::npos ||
                       stem.find(wanted) != std::string::npos;
    const uintmax_t size = std::filesystem::file_size(it->path(), ec);
    if (named && !best_named) {
      best = it->path();
      best_size = size;
      best_named = true;
    } else if (named == best_named && size > best_size) {
      best = it->path();
      best_size = size;
    }
  }
  return best;
}

std::vector<Game> ScanFolder(const std::filesystem::path& root) {
  std::vector<Game> found;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (!entry.is_directory(ec)) {
      continue;
    }
    Game game;
    game.kind = Kind::kExecutable;
    game.name = entry.path().filename().string();
    game.root = entry.path();
    game.executable = ExecutableFor(entry.path());
    if (game.executable.empty()) {
      REXLOG_WARN("Full games: no obvious program in '{}'; skipped", game.name);
      continue;
    }
    found.push_back(std::move(game));
  }
  return found;
}

// A package this staged, and what it points at.
//
// pcgame.txt rather than rom.txt, and the difference is the whole point: one
// runs its own program, the other goes to the emulator. game_launch.cpp reads
// exactly this to tell them apart.
constexpr const char* kPointerName = "pcgame.txt";
constexpr uint32_t kInstalledGameType = 0x4000;
constexpr uint32_t kDeviceIdHdd = 1;
constexpr size_t kHeaderSize = 0x148;

std::filesystem::path ContentRootFor() {
  return nxe_storage::ContentRoot() / "0000000000000000";
}

void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// The same 0x148-byte XCONTENT_AGGREGATE_DATA a real package header is.
std::vector<uint8_t> BuildHeader(uint32_t title_id, const std::string& name) {
  std::vector<uint8_t> header(kHeaderSize, 0);
  PutBe32(&header[0x00], kDeviceIdHdd);
  PutBe32(&header[0x04], kInstalledGameType);
  size_t at = 0x08;
  for (unsigned char ch : name) {
    if (at + 2 > 0x108) break;
    header[at] = 0;
    header[at + 1] = ch < 0x80 ? ch : '?';
    at += 2;
  }
  for (size_t i = 0; i < name.size() && i < 42; ++i) {
    header[0x108 + i] = uint8_t(name[i]);
  }
  PutBe32(&header[0x140], title_id);
  return header;
}

// A Steam name is not a folder name: 'Mirror's Edge' and 'S.T.A.L.K.E.R.:'
// both need work before they can be one.
std::string Foldered(const std::string& name) {
  std::string out;
  for (unsigned char ch : name) {
    out.push_back(std::strchr("\/:*?\"<>|", ch) || ch < 0x20 ? '_' : char(ch));
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
    out.pop_back();
  }
  return out;
}

}  // namespace

std::vector<Game> Scan() {
  std::vector<Game> found;
  std::error_code ec;

  // Each folder decides for itself what it is. A Steam library and a plain
  // folder of games want different treatment, and which row somebody put
  // which in is not something they should have to think about.
  for (const std::string& setting :
       {REXCVAR_GET(full_games_dir), REXCVAR_GET(full_games_dir_2),
        REXCVAR_GET(full_games_dir_3), REXCVAR_GET(full_games_dir_4)}) {
    if (setting.empty()) {
      continue;
    }
    const auto root = nxe_paths::Resolve(setting);
    if (!std::filesystem::is_directory(root, ec)) {
      REXLOG_WARN("Full games: '{}' is not a folder", root.string());
      continue;
    }

    const auto steamapps = SteamAppsIn(root);
    auto here = steamapps.empty() ? ScanFolder(root) : ScanSteam(steamapps);
    REXLOG_INFO("Full games: {} game(s) in {}{}", here.size(), root.string(),
                steamapps.empty() ? "" : " (a Steam library)");

    // The same game can be reachable through two folders -- a Steam library
    // and the drive it sits on, say. Listing it twice is worse than either.
    for (auto& game : here) {
      const bool already =
          std::any_of(found.begin(), found.end(), [&](const Game& have) {
            return Lowered(have.name) == Lowered(game.name);
          });
      if (!already) {
        found.push_back(std::move(game));
      }
    }
  }

  std::sort(found.begin(), found.end(),
            [](const Game& a, const Game& b) { return Lowered(a.name) < Lowered(b.name); });
  return found;
}

bool Launch(const Game& game) {
  if (game.kind == Kind::kSteam) {
    // Through Steam, not around it: a Steam game started by its executable
    // usually relaunches itself through Steam anyway, and some refuse outright.
    const std::string url = "steam://rungameid/" + game.steam_appid;
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
      REXLOG_ERROR("Full games: Steam would not start '{}' ({})", game.name, result);
      return false;
    }
    REXLOG_INFO("Full games: asked Steam for '{}' (app {})", game.name, game.steam_appid);
    return true;
  }

  std::error_code ec;
  if (game.executable.empty() || !std::filesystem::exists(game.executable, ec)) {
    REXLOG_ERROR("Full games: nothing to run for '{}'", game.name);
    return false;
  }
  // Started in its own folder: a game that loads data by relative path finds
  // nothing when the working directory is the dashboard's.
  const std::string directory = game.executable.parent_path().string();
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteA(
      nullptr, "open", game.executable.string().c_str(), nullptr, directory.c_str(),
      SW_SHOWNORMAL));
  if (result <= 32) {
    REXLOG_ERROR("Full games: could not start '{}' ({})", game.name, result);
    return false;
  }
  REXLOG_INFO("Full games: started '{}' from {}", game.name, game.executable.string());
  return true;
}


uint32_t TitleIdFor(const std::string& name) {
  // FNV-1a over the lowercased name, folded to sixteen bits.
  //
  // Stable is the whole requirement: the same game has to come back with the
  // same id every run, or the library gains a duplicate every time somebody
  // starts the dashboard. Hashing the name gives that for free and needs
  // nothing stored anywhere.
  uint32_t hash = 2166136261u;
  for (unsigned char ch : Lowered(name)) {
    hash ^= ch;
    hash *= 16777619u;
  }
  const uint32_t low = ((hash >> 16) ^ hash) & 0xFFFFu;
  // 0xFE00 is not a real publisher, and stays clear of the 0xFFFE and 0xFFFF
  // that IsSystemTitleId treats as the console's own.
  return 0xFE000000u | (low ? low : 1u);
}

bool LaunchFromPackage(const std::filesystem::path& package) {
  std::error_code ec;
  const auto pointer = package / kPointerName;
  if (!std::filesystem::exists(pointer, ec)) {
    return false;  // not a PC game; the caller carries on to the emulator
  }

  std::ifstream file(pointer);
  std::string kind, detail, name;
  std::getline(file, kind);
  std::getline(file, detail);
  std::getline(file, name);

  Game game;
  game.name = name.empty() ? package.filename().string() : name;
  if (kind == "steam") {
    game.kind = Kind::kSteam;
    game.steam_appid = detail;
  } else {
    game.kind = Kind::kExecutable;
    game.executable = detail;
  }
  return Launch(game);
}

size_t StageAll() {
  std::error_code ec;
  size_t changed = 0;

  // Removals first, and by the same rule the ROM staging uses: only a package
  // holding nothing but our marker is ever deleted, so a real install that
  // happens to carry one cannot be destroyed by this.
  const auto root = ContentRootFor();
  if (std::filesystem::is_directory(root, ec)) {
    for (const auto& title_dir : std::filesystem::directory_iterator(root, ec)) {
      const auto type_dir = title_dir.path() / "00004000";
      if (!std::filesystem::is_directory(type_dir, ec)) continue;
      for (const auto& package : std::filesystem::directory_iterator(type_dir, ec)) {
        const auto pointer = package.path() / kPointerName;
        if (!std::filesystem::exists(pointer, ec)) continue;

        std::ifstream file(pointer);
        std::string kind, detail;
        std::getline(file, kind);
        std::getline(file, detail);
        // A Steam game is gone when its install folder is; a plain one when its
        // program is. Steam entries with no folder recorded are left alone.
        const bool still_there = detail.empty() || std::filesystem::exists(detail, ec);
        if (still_there) continue;

        size_t others = 0;
        for (const auto& file2 : std::filesystem::directory_iterator(package.path(), ec)) {
          if (file2.path().filename() != kPointerName) ++others;
        }
        if (others) continue;

        const auto name = package.path().filename().string();
        std::filesystem::remove_all(package.path(), ec);
        std::filesystem::remove(
            title_dir.path() / "Headers" / "00004000" / (name + ".header"), ec);
        REXLOG_INFO("Full games: '{}' has gone; removed from the library", name);
        ++changed;
      }
    }
  }

  for (const auto& game : Scan()) {
    const uint32_t title_id = TitleIdFor(game.name);
    char title_dir[9] = {};
    std::snprintf(title_dir, sizeof(title_dir), "%08X", title_id);
    const std::string folder = Foldered(game.name);
    if (folder.empty()) continue;

    const auto package = ContentRootFor() / title_dir / "00004000" / folder;
    const auto pointer = package / kPointerName;
    const auto header_dir = ContentRootFor() / title_dir / "Headers" / "00004000";

    const std::string detail =
        game.kind == Kind::kSteam ? game.root.string() : game.executable.string();
    // Four lines: what kind it is, how to start it, what to call it, and where
    // it lives. Compared whole on the next run, so a game that moved or changed
    // launcher is re-staged rather than left pointing somewhere stale.
    const std::string want = std::string(game.kind == Kind::kSteam ? "steam" : "exe") + "\n" +
                             (game.kind == Kind::kSteam ? game.steam_appid : detail) + "\n" +
                             game.name + "\n" + detail + "\n";
    if (std::filesystem::exists(pointer, ec)) {
      std::ifstream have(pointer);
      const std::string text((std::istreambuf_iterator<char>(have)),
                             std::istreambuf_iterator<char>());
      if (text == want) continue;  // already staged, and unchanged
    }
    // Never claim a package somebody else made.
    if (std::filesystem::exists(package, ec) && !std::filesystem::exists(pointer, ec) &&
        !std::filesystem::is_empty(package, ec)) {
      REXLOG_WARN("Full games: '{}' collides with a package already there; skipped",
                  game.name);
      continue;
    }

    ec.clear();
    std::filesystem::create_directories(package, ec);
    std::filesystem::create_directories(header_dir, ec);
    if (ec) {
      REXLOG_ERROR("Full games: could not make a package for '{}': {}", game.name,
                   ec.message());
      continue;
    }
    { std::ofstream out(pointer, std::ios::trunc); out << want; }
    {
      const auto bytes = BuildHeader(title_id, game.name);
      std::ofstream out(header_dir / (folder + ".header"), std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    REXLOG_INFO("Full games: staged '{}' as {:08X}", game.name, title_id);
    ++changed;
  }
  return changed;
}

}  // namespace nxe_pc
