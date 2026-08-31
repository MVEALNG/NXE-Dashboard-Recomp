// Images stored inside the profile's GPD files.
//
// A GPD is an XDBF container, and namespace 2 holds PNGs keyed by an id:
// achievement icons under the id each achievement record names in its
// image_id field, and the title's own icon under 0x8000. They are what
// XamReadTile is being asked for when the dashboard draws an achievement list
// or a game tile.
#pragma once

#include <cstdint>
#include <vector>

namespace nxe_profile {

// PNG bytes for one image out of <title_id>.gpd, or empty when the profile has
// no GPD for that title or no image under that id. Not every title ships icons:
// a GPD synced without them carries only 0x8000.
const std::vector<uint8_t>& GpdImage(uint32_t title_id, uint64_t image_id);

// The whole of <title_id>.gpd, or empty when the profile has none. Exposed so
// the achievement list can read namespace 1 out of the same file the images
// come from, rather than opening it a second way.
const std::vector<uint8_t>& ReadTitleGpd(uint32_t title_id);

// The id a title's own icon is stored under.
inline constexpr uint64_t kTitleIconImageId = 0x8000;

}  // namespace nxe_profile
