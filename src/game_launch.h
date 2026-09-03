// The title the library is currently showing.
//
// The launch call's first argument has not proved trustworthy: read as a
// content record it decodes to nonsense, and its bytes are UTF-16 text rather
// than record fields. Rather than keep guessing at its shape, the title being
// launched is taken from what the library last opened -- the detail page asks
// for that title's achievements and its icon, both of which name it explicitly.
#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/ppc.h>

namespace nxe_game {

// Remember a title the library has just shown. Ignores system titles and 0.
void NoteTitleShown(uint32_t title_id);

// The most recent one, or 0 if the library has not shown anything yet.
uint32_t LastTitleShown();

// Where a title is staged, or empty when it is not installed.
std::filesystem::path PackagePathForTitle(uint32_t title_id);

// Run whatever is presented as the disc in the tray. False when there is no
// disc, or nothing staged for it.
bool LaunchDiscTitle(PPCContext& ctx, uint8_t* base);

// Run a title that is staged in storage, by id. False when it is not installed,
// has no executable, or there is no emulator configured.
//
// This is what a marketplace tile calls: the row knows a game's title id, and a
// title id is exactly what names its folder under the content root, so a game
// that is actually here can be played from the store page that advertises it.
bool LaunchStagedTitle(PPCContext& ctx, uint8_t* base, uint32_t title_id);

// Put the dashboard's UI back after a launch that returned.
//
// The disc tile tears its UI down before launching and never restores, because
// a console would be gone by then. Nothing comes back on its own.
void RestoreDashboardUi(PPCContext& ctx, uint8_t* base);

}  // namespace nxe_game
