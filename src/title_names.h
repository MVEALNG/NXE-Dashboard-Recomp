// The name a title is known by.
//
// Answered from the profile's played-game history first and the installed
// package header second -- the same two sources the Game Library list is built
// from, so a row, its list entry and its icon cannot disagree about which game
// they belong to.
#pragma once

#include <cstdint>
#include <string>

namespace nxe_title {

// Empty when nothing on this console knows the title.
std::u16string NameFor(uint32_t title_id);

}  // namespace nxe_title
