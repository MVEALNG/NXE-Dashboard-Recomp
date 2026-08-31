// What Discord shows about this dashboard.
//
// Nothing here blocks the caller. The state is handed to a worker thread, and
// whether Discord is running, connected, or absent entirely makes no difference
// to the dashboard -- with no Discord on the machine every call here is a
// couple of string copies and a condition-variable signal.
#pragma once

#include <cstdint>
#include <string>

namespace nxe_discord {

// Connect and start publishing. Safe to call when Discord is not running: the
// worker keeps trying quietly in the background.
void Start();

// Stop publishing and disconnect. Safe to call when Start never ran.
void Stop();

// Sitting in the dashboard, on whichever screen was last navigated to.
void SetDashboard();

// Moved to a screen. Takes the scene file the shell navigated to, e.g.
// "TitleDetailsRomeScene.xur"; the readable name is worked out from it.
// Ignored while a game is running.
void SetScene(const std::string& scene_file);

// Backed out of the current screen, to whatever was underneath.
void PopScene();

// Playing a title. An empty name falls back to the title id.
void SetPlaying(uint32_t title_id, const std::string& name);

}  // namespace nxe_discord
