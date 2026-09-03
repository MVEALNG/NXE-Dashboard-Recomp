// Choosing a gamer picture.
//
// Change Gamer Picture is a Guide screen on a console -- XAM's own, which this
// port has no UI layer for, and no .xur for it ships in any dashboard package.
// The chooser is therefore one of the dashboard's own pages, built by
// tools/import_gamerpics.py out of the dumped shared resources, and picking a
// tile lands here.
//
// The gamercard reads its picture from tile_64.png / tile_32.png inside the
// signed-in profile's content package, which is exactly where this writes -- so
// the choice takes effect the way it would on hardware, and survives a restart.
#pragma once

#include <filesystem>
#include <string>

namespace nxe_tiles {

// Copy the picture named by `file_name` (as "64_<title><imageid>.png") out of
// `source_dir` and into the profile package. The 32px companion is taken from
// the same folder when it is there. False when there is no profile package to
// write to, or the files are missing.
bool SetGamerPicture(const std::filesystem::path& source_dir, const std::string& file_name);

}  // namespace nxe_tiles
