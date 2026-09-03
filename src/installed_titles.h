// Content actually staged on the storage device, across every title.
//
// Shared by the content enumerator and the Game Library, which need the same
// answer from two different XAM entry points.
#pragma once

#include <cstdint>
#include <vector>

#include <rex/system/xam/content_manager.h>

namespace nxe_content {

// Every content package on the HDD, for the signed-in user and the common tree.
std::vector<rex::system::xam::XCONTENT_AGGREGATE_DATA> AllInstalledContent();

// Is this one of the console's own titles rather than a game?
//
// 0 is "no title". 0xFFFE is the dashboard and the built-in applications;
// 0xFFFF is the rest of the reserved space. A game's high half is its publisher
// id -- 0x4D53 "MS" for Halo 3, 0x5443 for Ninja Gaiden II.
//
// This lives here because two places have to agree on it exactly. The Game
// Library list drops system titles, and the titles-played count the profile
// advertises has to describe that same list: the dashboard sizes the library
// from the count and then fills it from the enumerator, so a count that
// includes an entry the enumerator will not produce leaves the walk short of
// what was promised and the list is discarded -- the games appear for a moment
// and then vanish. They were counted separately, and diverged the moment the
// list started excluding the whole reserved range instead of just the
// dashboard's own id.
inline bool IsSystemTitleId(uint32_t title_id) {
  const uint32_t publisher = title_id >> 16;
  return title_id == 0 || publisher == 0xFFFEu || publisher == 0xFFFFu;
}

// How many distinct titles have content installed, excluding the console's own.
//
// Filesystem only -- no content manager, no kernel state -- because this is
// needed while the profile is loading, long before either exists.
uint32_t InstalledTitleCount();

// How many titles the enumerator will actually hand back.
//
// The number that matters, and the only one guaranteed to match the list: it
// is that list's size. Needs the kernel, so it is unavailable while the
// profile is first loading -- returns 0 then, and InstalledTitleCount falls
// back to counting the disk.
uint32_t EnumeratedTitleCount();

}  // namespace nxe_content
