// The setup that has to happen once, before any of this works.
//
// Most of what makes this look like a console is content it cannot ship -- a
// profile, themes, avatar items, games. Somebody running it for the first time
// has to say where those are, and until now the only way to find that out was
// to read the source. This asks.
#pragma once

#include <filesystem>
#include <memory>

namespace rex::ui {
class ImGuiDrawer;
class ImGuiDialog;
}  // namespace rex::ui

namespace nxe_setup {

// Is there anything still worth asking about?
//
// False once somebody has been through this and said so, whatever they chose --
// declining to set a folder is an answer, and being asked again every launch is
// not a helpful way to hear it.
bool NeedsFirstRun();

// The dialog, or nullptr when there is nothing to ask.
//
// Owned by the caller. config_path is where the answers are written, which is
// the same NXE.toml the dashboard rewrites for itself on exit -- going through
// the settings rather than the file is what makes the two agree.
std::unique_ptr<rex::ui::ImGuiDialog> MakeFirstRunDialog(rex::ui::ImGuiDrawer* drawer,
                                                         std::filesystem::path config_path);

// The same dialog, whether or not anything is missing.
//
// MakeFirstRunDialog answers nullptr once setup has been done, which is right
// for start-up and wrong for a key that is supposed to open it. Somebody
// pressing F7 wants the dialog, not a judgement about whether they need it.
std::unique_ptr<rex::ui::ImGuiDialog> MakeSetupDialog(rex::ui::ImGuiDrawer* drawer,
                                                      std::filesystem::path config_path);

}  // namespace nxe_setup
