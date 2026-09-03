// Games dropped into the ROMs folder, staged so the Game Library can see them.
//
// Download a game, put it in roms_dir, press refresh, and it is on the shelf.
// That is the whole feature, and everything below exists because the library
// will not look anywhere else: it is built from the content packages staged
// under the storage root, so a game reaches it by being one of those.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nxe_roms {

// One game found in the ROMs folder.
struct Rom {
  uint32_t title_id = 0;
  std::string name;                 // what it will be called on the shelf
  std::filesystem::path executable; // the default.xex itself
  std::filesystem::path root;       // the folder the emulator has to be given
};

// The title id a XEX declares, or 0 if the file is not one.
//
// From the execution-info optional header, which is where a title id actually
// lives -- not the filename, and not anything a download site chose to call it.
uint32_t TitleIdOf(const std::filesystem::path& xex);

// Every game in roms_dir, whether or not it is staged yet.
std::vector<Rom> Scan();

// Remove staged games whose ROM has gone. Returns how many were removed.
//
// Only ever touches a package holding rom.txt -- the marker for something this
// staged. A real install has no such file and cannot be reached from here.
size_t Unstage();

// Stage anything found that is not staged already. Returns how many were added.
//
// Nothing is copied and nothing is linked: the package holds a pointer to where
// the game really is, and the launcher follows it. A folder of games stays one
// folder of games.
size_t StageAll();

}  // namespace nxe_roms
