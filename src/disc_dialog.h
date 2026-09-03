// Choosing what is in the disc drive, and which profiles exist.
//
// Both were settings you had to know the shape of: disc_title wants a title id
// in hex, and a profile arrives only by being copied into the right folder by
// hand. Neither is something to ask of somebody who just wants to put a game in.
#pragma once

#include <filesystem>
#include <memory>

namespace rex::ui {
class ImGuiDrawer;
class ImGuiDialog;
}  // namespace rex::ui

namespace nxe_disc {

// The dialog. Owned by the caller; safe to create and destroy repeatedly.
std::unique_ptr<rex::ui::ImGuiDialog> MakeDiscDialog(rex::ui::ImGuiDrawer* drawer,
                                                     std::filesystem::path config_path);

}  // namespace nxe_disc
