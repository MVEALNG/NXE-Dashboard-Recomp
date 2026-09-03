// Games that are not Xbox 360 games, launched from the dashboard.
//
// A folder of installed PC games, or a Steam library. Separate from the ROMs
// folder throughout: those are Xbox 360 titles with title ids, run through an
// emulator; these are ordinary programs, run directly.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace nxe_pc {

// How a game is started, which is not the same question for both kinds.
enum class Kind {
  kSteam,       // launched through Steam by app id, not by finding an exe
  kExecutable,  // an ordinary program on disk
};

struct Game {
  std::string name;
  Kind kind = Kind::kExecutable;
  std::string steam_appid;         // kSteam only
  std::filesystem::path executable;  // kExecutable only
  std::filesystem::path root;      // where it lives, for showing
};

// Everything found under full_games_dir.
//
// A Steam library is recognised by its steamapps folder and read from the
// manifests there, which name every installed game and its app id. A plain
// folder is one game per subfolder, and the executable has to be guessed.
std::vector<Game> Scan();

// Start one. Returns false if it could not be started at all.
bool Launch(const Game& game);

// The title id this game is filed under in the library.
//
// Invented, because a PC game has none and the library is keyed by one.
// 0xFE00 plus a hash of the name: stable run to run, and outside the
// 0xFFFE/0xFFFF range the dashboard reserves for itself.
uint32_t TitleIdFor(const std::string& name);

// Stage every game found, and remove any whose folder has gone.
// Returns how many were added or removed.
size_t StageAll();

// If this package is a PC game, start it. False means it is not one.
//
// How game_launch.cpp tells the two apart: a package with pcgame.txt runs its
// own program, one with rom.txt goes to the emulator.
bool LaunchFromPackage(const std::filesystem::path& package);

}  // namespace nxe_pc
