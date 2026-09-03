// Asking, once, for the things this cannot ship.
//
// The list is nxe_setup::Check(), so this stays a way of asking and the answer
// to "what does a new install need" lives in one place. Adding a requirement
// there puts it on this dialog with no work here.
//
// It writes through the settings rather than to the file. NXE.toml is rewritten
// by the dashboard on exit out of its own cvar state -- a value hand-edited into
// it survives exactly one run, which is how two evenings went missing chasing a
// setting that had already been wiped. SetFlagByName puts the value where that
// rewrite will find it, and SaveConfig writes it now so a crash cannot lose it.

#include "first_run.h"

#include <windows.h>
#include <shobjidl.h>

#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "setup_check.h"
#include "setup_tools.h"

// Has somebody been through setup? Not "is setup complete" -- declining to set
// a folder is a legitimate answer, and this records that they were asked.
REXCVAR_DEFINE_BOOL(setup_completed, false, "Dashboard",
                    "Whether first-run setup has been shown. Set false to see it again.");

namespace nxe_setup {
namespace {

// The TMDB key belongs in two places.
//
// The dashboard keeps it as a setting so setup can ask for it once; the tool
// that actually calls TMDB reads tools/.tmdb_key, because it is a Python script
// run separately and knows nothing about cvars. Writing the file here is what
// joins them, so nobody has to be told to copy a key into a dotfile.
void WriteTmdbKey(const std::string& key) {
  if (key.empty()) {
    return;
  }
  std::error_code ec;
  const auto tools = ToolsDir();
  const auto path = tools / ".tmdb_key";
  if (tools.empty()) {
    REXLOG_INFO("Setup: no tools folder beside the dashboard; TMDB key kept as a setting only");
    return;
  }
  std::ofstream out(path, std::ios::trunc);
  if (out) {
    out << key << "\n";
    REXLOG_INFO("Setup: wrote the TMDB key to {}", path.string());
  }
}

class FirstRunDialog : public rex::ui::ImGuiDialog {
 public:
  FirstRunDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
      : ImGuiDialog(drawer), config_path_(std::move(config_path)) {
    for (const auto& item : Check()) {
      Field field;
      field.item = item;
      // Start from whatever is already set, so this is an edit rather than a
      // form to fill in twice on a half-configured install.
      std::snprintf(field.buffer, sizeof(field.buffer), "%s", item.value.c_str());
      fields_.push_back(field);
    }
  }

  void OnDraw(ImGuiIO& io) override {
    if (done_) {
      return;
    }
    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Set up the dashboard", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    ImGui::TextWrapped(
        "This dashboard cannot ship themes, avatar items, games or a profile, so it needs "
        "to know where yours are. Nothing here is required except storage -- anything you "
        "leave blank simply stays empty, and you can change all of it later with F4.");
    ImGui::Separator();

    for (size_t i = 0; i < fields_.size(); ++i) {
      auto& field = fields_[i];
      ImGui::PushID(int(i));

      ImGui::TextUnformatted(field.item.label);
      if (field.item.required) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "(required)");
      }
      ImGui::TextDisabled("%s", field.item.note);

      ImGui::SetNextItemWidth(560.0f);
      ImGui::InputText("##value", field.buffer, sizeof(field.buffer));

      // A key is typed; everything else is browsed for, as the right kind.
      if (field.item.pick != Pick::kText) {
        const bool folder = field.item.pick == Pick::kFolder;
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
          const auto picked =
              PickPath(std::string(folder ? "Choose the folder for " : "Choose the file for ") +
                           field.item.label,
                       folder);
          if (!picked.empty()) {
            std::snprintf(field.buffer, sizeof(field.buffer), "%s", picked.c_str());
          }
        }
      }
      ImGui::Spacing();
      ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Your Xbox profile");
    ImGui::TextDisabled(
        "If you already have a profile -- from a real console or from Xenia -- import it "
        "and the dashboard signs in as you with no Microsoft account involved. A console "
        "profile is a single file named for its XUID; a Xenia one is a folder.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(430.0f);
    ImGui::InputText("##profile", profile_buffer_, sizeof(profile_buffer_));
    ImGui::SameLine();
    if (ImGui::Button("File...")) {
      const auto picked = PickPath("Choose the profile package", false);
      if (!picked.empty()) {
        std::snprintf(profile_buffer_, sizeof(profile_buffer_), "%s", picked.c_str());
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Folder...")) {
      const auto picked = PickPath("Choose the profile folder", true);
      if (!picked.empty()) {
        std::snprintf(profile_buffer_, sizeof(profile_buffer_), "%s", picked.c_str());
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import")) {
      const std::string chosen(profile_buffer_);
      if (chosen.empty()) {
        REXLOG_WARN("Setup: choose a profile to import first");
      } else {
        // The storage root goes with it: the tool defaults to one beside the
        // repo, and the dashboard's may be anywhere.
        std::string storage = rex::cvar::GetFlagByName("storage_root");
        if (storage.empty()) {
          storage = "storage";
        }
        launched_ = RunTool("import_profile.py",
                            "\"" + chosen + "\" --storage \"" + storage + "\"");
      }
    }
    ImGui::Spacing();

    ImGui::Separator();
    ImGui::TextUnformatted("Xbox LIVE");
    ImGui::TextDisabled(
        "Optional, and it is what fills the profile, friends, games, covers and "
        "achievements. A console window opens and shows an eight-character code to "
        "type at microsoft.com/link -- approve it there and the rest runs on its own.");
    ImGui::Spacing();

    if (ImGui::Button("Sign in to Xbox LIVE", ImVec2(200, 0))) {
      launched_ = RunTool("fetch_profile.py");
    }
    ImGui::SameLine();
    if (ImGui::Button("Sign in and sync everything", ImVec2(240, 0))) {
      launched_ = RunTool("sync_all.py");
    }
    if (launched_) {
      ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                         "Started. Follow it in the console window, then press F6 here.");
    }
    ImGui::Spacing();

    if (ImGui::Button("Save", ImVec2(120, 0))) {
      Apply();
      Finish(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Not now", ImVec2(120, 0))) {
      // Asked and declined. Not recorded as done, so it comes back next launch;
      // somebody who has not found their folders yet should be asked again.
      Finish(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't ask again", ImVec2(150, 0))) {
      Finish(true);
    }

    ImGui::End();
  }

 private:
  struct Field {
    Requirement item;
    char buffer[512] = {};
  };

  void Apply() {
    for (const auto& field : fields_) {
      const std::string value(field.buffer);
      if (value == field.item.value) {
        continue;
      }
      rex::cvar::SetFlagByName(field.item.setting, value);
      REXLOG_INFO("Setup: {} = '{}'", field.item.setting, value);
      if (std::string(field.item.setting) == "tmdb_key") {
        WriteTmdbKey(value);
      }
    }
  }

  void Finish(bool remember) {
    if (remember) {
      rex::cvar::SetFlagByName("setup_completed", "true");
    }
    // Written now rather than left to the rewrite on exit: a crash between here
    // and shutdown would otherwise ask for all of it again.
    rex::cvar::SaveConfig(config_path_);
    done_ = true;
  }

  std::filesystem::path config_path_;
  std::vector<Field> fields_;
  bool done_ = false;
  bool launched_ = false;  // something was started, so say so rather than nothing
  char profile_buffer_[512] = {};
};

}  // namespace

bool NeedsFirstRun() {
  if (REXCVAR_GET(setup_completed)) {
    return false;
  }
  for (const auto& item : Check()) {
    if (!item.present) {
      return true;
    }
  }
  return false;
}

std::unique_ptr<rex::ui::ImGuiDialog> MakeFirstRunDialog(rex::ui::ImGuiDrawer* drawer,
                                                         std::filesystem::path config_path) {
  if (!drawer || !NeedsFirstRun()) {
    return nullptr;
  }
  REXLOG_INFO("Setup: first run, asking where things are");
  return std::make_unique<FirstRunDialog>(drawer, std::move(config_path));
}

std::unique_ptr<rex::ui::ImGuiDialog> MakeSetupDialog(rex::ui::ImGuiDrawer* drawer,
                                                      std::filesystem::path config_path) {
  if (!drawer) {
    return nullptr;
  }
  return std::make_unique<FirstRunDialog>(drawer, std::move(config_path));
}

}  // namespace nxe_setup
