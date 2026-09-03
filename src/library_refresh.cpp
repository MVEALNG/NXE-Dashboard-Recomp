// Re-read the storage device so a game installed just now can be seen.
//
// The Game Library is built from two things read off disk -- the profile's
// played-titles history and the content packages staged under the storage root
// -- and both are cached against the profile generation, because they are read
// once and then consulted constantly. Installing a game while the dashboard is
// running therefore changed nothing until it was restarted, which is a poor
// answer for the first thing a new user does.
//
// Bumping the generation is the whole mechanism. Everything profile-scoped
// rebuilds on its next use: the played-titles list, the GPD caches behind it,
// the merged LIVE history, and the counts derived from all of it. That is
// deliberately the same lever signing in a profile pulls, because it is the
// same question -- what is on this disk for this person.

#include "library_refresh.h"

#include <rex/logging.h>

#include "installed_titles.h"
#include "pc_library.h"
#include "rom_library.h"
#include "played_titles.h"
#include "profile_list.h"

namespace nxe_library {

Result Refresh() {
  Result result;

  // Counted before and after so the refresh can say what it did. Both calls go
  // through the same union the library and its count agree on, so a difference
  // here is a difference on screen.
  result.titles_before = nxe_content::InstalledTitleCount();

  // Stage anything new in the ROMs folder before counting again, so a game
  // dropped in since the last refresh is one of the things this finds.
  result.roms_staged = nxe_roms::StageAll();
  result.roms_staged += nxe_pc::StageAll();

  nxe_profile::InvalidateCaches();

  // Force the rebuild now rather than on whatever thread asks next: the read is
  // a directory walk and a GPD parse, and doing it here keeps it off the guest
  // thread that will be drawing the list.
  const auto& titles = nxe_profile::PlayedTitles();
  result.titles_after = nxe_content::InstalledTitleCount();

  // Tell the profile the new number.
  //
  // Without this the refresh finds the game and the guest never hears: the
  // count lives in profile setting 0x10040004, published once when the profile
  // loaded, while the enumerator is read fresh every time the library opens. A
  // game installed since startup makes the enumerator offer one more title than
  // the count promises, and a library whose walk does not match its size throws
  // the list away rather than showing it short.
  nxe_profile::RepublishDerivedSettings();

  if (result.changed()) {
    REXLOG_INFO("Library refresh: {} title(s), was {} ({} in the profile history)",
                result.titles_after, result.titles_before, titles.size());
  } else {
    REXLOG_INFO("Library refresh: {} title(s), unchanged", result.titles_after);
  }
  return result;
}

}  // namespace nxe_library
