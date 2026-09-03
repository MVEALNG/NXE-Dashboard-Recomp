// Pick up games that appeared while the dashboard was running.
//
// Everything the Game Library is built from is cached against the profile
// generation, because it is all read off disk once and would otherwise be read
// on every draw. That is right until somebody installs a game, at which point
// the only way to see it was to restart.
#pragma once

#include <cstdint>
#include <string>

namespace nxe_library {

// What a refresh found, so it can be reported rather than done silently.
struct Result {
  size_t roms_staged = 0;   // games newly staged out of the ROMs folder
  uint32_t titles_before = 0;
  uint32_t titles_after = 0;
  bool changed() const { return titles_before != titles_after; }
};

// Re-read the storage device and the profile's history.
//
// Safe to call at any time and from any thread. The guest is not told: it asks
// for the library when it opens the screen, so the new list appears the next
// time that screen is opened rather than underneath somebody looking at it.
Result Refresh();

}  // namespace nxe_library
