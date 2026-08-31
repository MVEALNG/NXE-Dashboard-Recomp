// Titles the signed-in profile has played, with their achievement totals.
//
// Read from the profile's own dashboard GPD rather than from what happens to be
// installed, because those are different questions: a profile's history covers
// games whose discs are long gone, and that history is where the achievements
// and gamerscore live.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nxe_profile {

// One entry of the profile's played-games history. The field names match the
// XAM played-title record they end up in (see X_TITLE_PLAYED in
// src/title_library.cpp) because the GPD record has the same shape.
struct PlayedTitle {
  uint32_t title_id = 0;
  uint32_t achievements_possible = 0;
  uint32_t achievements_earned = 0;
  uint32_t gamerscore_total = 0;
  uint32_t gamerscore_earned = 0;
  uint64_t last_played = 0;  // FILETIME, 0 when the GPD does not record one
  std::u16string name;
};

// The profile's played-games history, parsed once on first use. Empty when no
// profile is staged or its dashboard GPD carries no title records.
const std::vector<PlayedTitle>& PlayedTitles();

}  // namespace nxe_profile
