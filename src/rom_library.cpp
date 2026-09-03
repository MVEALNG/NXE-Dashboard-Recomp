// Staging games from the ROMs folder into the content tree.
//
// The Game Library is built from ContentAggregateCreateEnumerator, which walks
// the content packages staged under the storage root and groups them by title
// id. Nothing else reaches that screen -- listing a title without a package
// behind it was tried, and the guest opens the package to read its header, gets
// nothing, and dereferences it: a read of guest address 0 (see
// library_synthesize_content in content_enum.cpp). So a game gets on the shelf
// by being a package, and this makes one.
//
// What it does not do is move the game. A package here is a directory holding a
// single line of text -- where the game actually is -- and game_launch.cpp
// follows it. Copying would duplicate gigabytes; a junction or a symlink would
// want privileges this has no business asking for. A pointer costs nothing and
// deleting the package never touches the ROM.
//
// The title id comes out of the XEX rather than the filename, because the
// filename is whatever the person who uploaded it chose and the title id is the
// directory name the library looks the game up by. Getting it from the file
// means Batman works whether the folder is called "Batman Arkham Asylum",
// "batman_aa_ntsc" or "game (1)".

#include "rom_library.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "install_paths.h"
#include "storage_device.h"

REXCVAR_DECLARE(std::string, roms_dir);

namespace nxe_roms {
namespace {

// The XEX optional header that carries the title id.
//
//     0x00 media_id   0x08 base_version   0x10 platform / type / disc
//     0x04 version    0x0C title_id
//
// The key's low byte is the size in dwords -- 6 here, so 0x18 bytes -- and the
// value is the offset the block sits at.
constexpr uint32_t kExecutionInfoKey = 0x00040006;
constexpr uint32_t kTitleIdOffset = 0x0C;

// The content type installed games use. Both games staged by hand on this
// console sit under 00004000, and the library groups on it.
constexpr uint32_t kInstalledGameType = 0x4000;
constexpr uint32_t kDeviceIdHdd = 1;

// A package header is a serialised XCONTENT_AGGREGATE_DATA, the same 0x148
// bytes tools/import_themes.py writes for a theme.
constexpr size_t kHeaderSize = 0x148;
constexpr size_t kOffDeviceId = 0x00;
constexpr size_t kOffContentType = 0x04;
constexpr size_t kOffDisplayName = 0x08;
constexpr size_t kOffFileName = 0x108;
constexpr size_t kOffXuid = 0x138;
constexpr size_t kOffTitleId = 0x140;

// The file the package points at the real game with.
constexpr const char* kPointerName = "rom.txt";

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Everything a package needs to describe itself, minus the game.
std::vector<uint8_t> BuildHeader(uint32_t title_id, const std::string& display_name,
                                 const std::string& file_name) {
  std::vector<uint8_t> header(kHeaderSize, 0);
  PutBe32(&header[kOffDeviceId], kDeviceIdHdd);
  PutBe32(&header[kOffContentType], kInstalledGameType);

  // UTF-16BE, and bounded: the field holds 128 characters and the name is
  // whatever a folder was called.
  size_t at = kOffDisplayName;
  for (unsigned char ch : display_name) {
    if (at + 2 > kOffFileName) {
      break;
    }
    header[at] = 0;
    header[at + 1] = ch < 0x80 ? ch : '?';
    at += 2;
  }

  for (size_t i = 0; i < file_name.size() && i < 42; ++i) {
    header[kOffFileName + i] = uint8_t(file_name[i]);
  }
  PutBe32(&header[kOffTitleId], title_id);
  (void)kOffXuid;  // left zero: these are not anybody's in particular
  return header;
}

// A name a shelf can show, from a folder called anything.
std::string Clean(std::string text) {
  for (char& ch : text) {
    if (ch == '|' || ch == '\r' || ch == '\n') {
      ch = ' ';
    }
  }
  const size_t first = text.find_first_not_of(' ');
  const size_t last = text.find_last_not_of(' ');
  return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

std::filesystem::path ContentRootFor() { return nxe_storage::ContentRoot() / "0000000000000000"; }

}  // namespace

uint32_t TitleIdOf(const std::filesystem::path& xex) {
  std::ifstream file(xex, std::ios::binary);
  if (!file) {
    return 0;
  }

  uint8_t head[0x18] = {};
  file.read(reinterpret_cast<char*>(head), sizeof(head));
  if (file.gcount() < std::streamsize(sizeof(head)) || std::memcmp(head, "XEX2", 4) != 0) {
    return 0;
  }

  const uint32_t count = Be32(&head[0x14]);
  // A malformed or hostile file must not make this read for ever; no real XEX
  // carries anything like this many optional headers.
  if (count > 256) {
    return 0;
  }

  for (uint32_t i = 0; i < count; ++i) {
    uint8_t entry[8] = {};
    file.seekg(0x18 + std::streamoff(i) * 8);
    file.read(reinterpret_cast<char*>(entry), sizeof(entry));
    if (file.gcount() < std::streamsize(sizeof(entry))) {
      return 0;
    }
    if (Be32(entry) != kExecutionInfoKey) {
      continue;
    }
    uint8_t info[0x18] = {};
    file.seekg(Be32(&entry[4]));
    file.read(reinterpret_cast<char*>(info), sizeof(info));
    if (file.gcount() < std::streamsize(sizeof(info))) {
      return 0;
    }
    return Be32(&info[kTitleIdOffset]);
  }
  return 0;
}

std::vector<Rom> Scan() {
  std::vector<Rom> found;
  const std::string setting = REXCVAR_GET(roms_dir);
  if (setting.empty()) {
    return found;
  }
  const auto root = nxe_paths::Resolve(setting);
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    REXLOG_WARN("ROMs: '{}' is not a folder", root.string());
    return found;
  }

  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    Rom rom;
    if (entry.is_directory(ec)) {
      // A downloaded game is a folder with the executable at its root. The
      // whole folder is what the emulator needs, not just the XEX.
      for (const char* name : {"default.xex", "Default.xex"}) {
        const auto candidate = entry.path() / name;
        if (std::filesystem::exists(candidate, ec)) {
          rom.executable = candidate;
          break;
        }
      }
      rom.root = entry.path();
      rom.name = Clean(entry.path().filename().string());
    } else if (entry.is_regular_file(ec)) {
      auto ext = entry.path().extension().string();
      for (char& ch : ext) {
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
      }
      if (ext != ".xex") {
        continue;  // ISOs need XDVDFS to find the XEX inside; not yet
      }
      rom.executable = entry.path();
      rom.root = entry.path();
      rom.name = Clean(entry.path().stem().string());
    }

    if (rom.executable.empty() || rom.name.empty()) {
      continue;
    }
    rom.title_id = TitleIdOf(rom.executable);
    if (!rom.title_id) {
      REXLOG_WARN("ROMs: '{}' has no title id in its XEX; skipped",
                  rom.executable.string());
      continue;
    }
    found.push_back(std::move(rom));
  }
  // Said every time, not only when something changes: a game sitting in the
  // folder and not on the shelf is the question this answers.
  REXLOG_INFO("ROMs: {} game(s) in {}", found.size(), root.string());
  for (const auto& rom : found) {
    REXLOG_INFO("ROMs:   {:08X}  {}", rom.title_id, rom.name);
  }
  return found;
}

size_t Unstage() {
  std::error_code ec;
  const auto root = ContentRootFor();
  if (!std::filesystem::is_directory(root, ec)) {
    return 0;
  }

  size_t removed = 0;
  for (const auto& title_dir : std::filesystem::directory_iterator(root, ec)) {
    if (!title_dir.is_directory(ec)) {
      continue;
    }
    const auto type_dir = title_dir.path() / "00004000";
    if (!std::filesystem::is_directory(type_dir, ec)) {
      continue;
    }
    for (const auto& package : std::filesystem::directory_iterator(type_dir, ec)) {
      if (!package.is_directory(ec)) {
        continue;
      }
      const auto pointer = package.path() / kPointerName;
      if (!std::filesystem::exists(pointer, ec)) {
        continue;  // not ours: a real install, and none of this business
      }

      std::string target;
      {
        std::ifstream file(pointer);
        std::getline(file, target);
      }
      if (!target.empty() && std::filesystem::exists(target, ec)) {
        continue;  // the game is still where it said it was
      }

      // A package this made holds one file and nothing else. Anything more is
      // a real install that happens to carry the marker, and deleting it would
      // destroy a game nobody asked to remove.
      //
      // This is not hypothetical: an early version of StageAll wrote rom.txt
      // into the Halo 3 install already staged here, because the ROM folder had
      // the same name. The marker alone is not proof of ownership; being empty
      // apart from the marker is.
      size_t others = 0;
      for (const auto& file : std::filesystem::directory_iterator(package.path(), ec)) {
        if (file.path().filename() != kPointerName) {
          ++others;
        }
      }
      if (others) {
        REXLOG_WARN("ROMs: '{}' is missing, but its package holds {} other file(s) -- "
                    "that is a real install, so it stays",
                    package.path().filename().string(), others);
        continue;
      }

      const auto name = package.path().filename().string();
      const auto header =
          title_dir.path() / "Headers" / "00004000" / (name + ".header");
      std::filesystem::remove_all(package.path(), ec);
      std::filesystem::remove(header, ec);
      REXLOG_INFO("ROMs: '{}' is gone from the ROMs folder; removed from the library",
                  name);
      ++removed;
    }
  }
  return removed;
}

size_t StageAll() {
  // Removals first, and unconditionally: an empty ROMs folder means every
  // staged game has gone, which is exactly when the old code returned early
  // and left all of them on the shelf.
  const size_t removed = Unstage();

  const auto roms = Scan();
  if (roms.empty()) {
    return removed;
  }

  std::error_code ec;
  size_t added = 0;
  for (const auto& rom : roms) {
    char title_dir[9] = {};
    std::snprintf(title_dir, sizeof(title_dir), "%08X", rom.title_id);

    auto package = ContentRootFor() / title_dir / "00004000" / rom.name;
    auto pointer = package / kPointerName;
    const auto header_dir = ContentRootFor() / title_dir / "Headers" / "00004000";
    auto header = header_dir / (rom.name + ".header");

    // Already staged, and pointing at the same game: nothing to do. Checked by
    // content rather than existence so a game that moved is re-pointed instead
    // of quietly launching from where it used to be.
    if (std::filesystem::exists(pointer, ec)) {
      std::ifstream have(pointer);
      std::string line;
      std::getline(have, line);
      if (line == rom.root.string()) {
        continue;
      }
    }

    // Never claim a package that already exists with something else in it.
    //
    // A ROM folder can be named the same as a real install -- there is a
    // "Halo 3" in both the ROMs folder and the content tree right now. Writing
    // the pointer into that install would mark it as ours, and Unstage would
    // then delete a game this never staged.
    if (std::filesystem::exists(package, ec) &&
        !std::filesystem::exists(pointer, ec) &&
        !std::filesystem::is_empty(package, ec)) {
      // Something else already owns that name -- a game staged by hand, and
      // there is a "Halo 3" in both places right now. The install is left
      // exactly as it is; the ROM gets a package of its own beside it.
      //
      // Skipping instead was wrong once the library started showing only ROMs:
      // the install is filtered out and the ROM was never staged, so the game
      // vanished from a folder it was plainly sitting in.
      package = package.parent_path() / (rom.name + " (ROM)");
      pointer = package / kPointerName;
      header = header_dir / (rom.name + " (ROM).header");
      if (std::filesystem::exists(pointer, ec)) {
        std::ifstream have(pointer);
        std::string line;
        std::getline(have, line);
        if (line == rom.root.string()) {
          continue;
        }
      }
    }

    REXLOG_INFO("ROMs: staging '{}' into {}", rom.name, package.string());
    ec.clear();  // stale from the probes above; create_directories is what matters
    std::filesystem::create_directories(package, ec);
    std::filesystem::create_directories(header_dir, ec);
    if (ec) {
      REXLOG_ERROR("ROMs: could not make a package for '{}': {}", rom.name, ec.message());
      continue;
    }

    {
      std::ofstream out(pointer, std::ios::trunc);
      if (!out) {
        REXLOG_ERROR("ROMs: could not write {}", pointer.string());
        continue;
      }
      out << rom.root.string() << "\n";
    }
    {
      const auto bytes = BuildHeader(rom.title_id, rom.name, rom.name);
      std::ofstream out(header, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }

    REXLOG_INFO("ROMs: staged '{}' as {:08X} from '{}'", rom.name, rom.title_id,
                rom.root.string());
    ++added;
  }

  if (added || removed) {
    REXLOG_INFO("ROMs: {} added, {} removed", added, removed);
  }
  return added + removed;
}

}  // namespace nxe_roms
