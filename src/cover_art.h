// Box art for a title.
//
// Loaded from a folder of PNGs named by title id, because an extracted package
// carries no artwork of its own. Kept separate from the small game icon: this
// is the picture the game rows and the disc tile draw, and it has to fit the
// content metadata's thumbnail field.
#pragma once

#include <cstdint>
#include <vector>

namespace nxe_art {

// The largest available cover for this title that fits in `limit` bytes, or
// empty when there is none. A fitted "<title>.thumb.png" beside the full-size
// file is preferred when the original is too large.
const std::vector<uint8_t>& CoverFor(uint32_t title_id, size_t limit);

}  // namespace nxe_art
