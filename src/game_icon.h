// The icon a game is shown with.
//
// Loaded from a folder of PNGs (see game_icon_dir) because a console keeps a
// game's icon in that title's own GPD, written when the game is first played,
// and nothing here has ever run these titles.
#pragma once

#include <cstdint>
#include <vector>

namespace nxe_tile {

// The icon file's bytes, or empty when there is no icon for this title.
const std::vector<uint8_t>& GameIconPng(uint32_t title_id);

}  // namespace nxe_tile
