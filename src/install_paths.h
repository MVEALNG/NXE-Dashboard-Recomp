// Where this installation keeps its files.
//
// Every path in this project used to be written out in full, against one
// particular machine -- "C:/Desktop/NXE Dashboard/nxe_dash_gamedir",
// "A:/Xbox360Storage", a Xenia build under C:/Desktop/xenia_manager. That is
// fine for the machine it was written on and useless anywhere else, and it is
// the one thing that stops somebody else building this and having it run.
//
// So paths are relative now, and resolved against an installation root: the
// directory the executable sits in, unless nxe_root says otherwise. A relative
// default therefore means "beside the dashboard", which is what a checkout
// looks like, while an absolute value in a config file or on the command line
// is still taken exactly as given.
#pragma once

#include <filesystem>
#include <string>

namespace nxe_paths {

// The installation root: nxe_root if set, else the executable's directory.
const std::filesystem::path& Root();

// An absolute path from a setting that may be either.
//
// Absolute values pass through untouched, so anyone who prefers to keep their
// game data somewhere else says so once and nothing here argues.
std::filesystem::path Resolve(const std::string& path);

}  // namespace nxe_paths
