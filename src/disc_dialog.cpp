// Putting a game in the drive, and adding profiles, without knowing any ids.
//
// disc_title is a title id in hex. That is what the guest wants -- the disc tile
// asks XamLoaderGetMediaInfo and the tray is described by an id, not a name --
// but it is a poor thing to ask a person for. The library already knows every
// title it has and what each is called, so this offers that list and writes the
// id behind it.
//
// Profiles are here for the same reason. A profile becomes available by being
// unpacked into <storage>/Content/<XUID>/FFFE07D1/00010000/<XUID>/, which is not
// a thing anybody should be told to do with a file manager. tools/import_profile
// .py already does it for a console package or a Xenia folder; this is a button
// for it, and a list of what that has produced so far.

#include "disc_dialog.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "game_launch.h"
#include "pc_library.h"
#include "played_titles.h"
#include "profile_list.h"
#include "setup_tools.h"

REXCVAR_DECLARE(std::string, disc_title);
REXCVAR_DECLARE(std::string, storage_root);

namespace nxe_disc {
namespace {

std::string HexOf(uint32_t title_id) {
  char text[9] = {};
  std::snprintf(text, sizeof(text), "%08X", title_id);
  return text;
}

// A title's name is UTF-16 in the record and ASCII is all ImGui is being given
// here; anything else becomes a question mark rather than half a character.
std::string Narrow(const std::u16string& text) {
  std::string out;
  out.reserve(text.size());
  for (char16_t ch : text) {
    out.push_back(ch && ch < 0x80 ? char(ch) : '?');
  }
  return out;
}

class DiscDialog : public rex::ui::ImGuiDialog {
 public:
  DiscDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
      : ImGuiDialog(drawer), config_path_(std::move(config_path)) {
    Reload();
  }

  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Disc drive and profiles", nullptr)) {
      ImGui::End();
      return;
    }

    DrawDisc();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    DrawFullGames();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    DrawProfiles();

    ImGui::End();
  }

 private:
  struct Title {
    uint32_t id = 0;
    std::string name;
    std::string hex;
    bool playable = false;  // there is a package staged for it
  };

  void Reload() {
    titles_.clear();
    for (const auto& played : nxe_profile::PlayedTitles()) {
      if (!played.title_id || played.name.empty()) {
        continue;
      }
      // Only a title with a package staged can go in the drive. The rest are
      // history: the account played them, this machine has no copy, and
      // choosing one gives 'No disc mounted' and a tile that says nothing.
      const bool playable = !nxe_game::PackagePathForTitle(played.title_id).empty();
      titles_.push_back(
          {played.title_id, Narrow(played.name), HexOf(played.title_id), playable});
    }
    missing_ = 0;
    for (const auto& title : titles_) {
      if (!title.playable) {
        ++missing_;
      }
    }
    std::sort(titles_.begin(), titles_.end(),
              [](const Title& a, const Title& b) { return a.name < b.name; });

    profiles_ = nxe_profile::StagedProfiles();
    full_games_ = nxe_pc::Scan();
  }

  void SetDisc(const std::string& hex) {
    rex::cvar::SetFlagByName("disc_title", hex);
    rex::cvar::SaveConfig(config_path_);
    REXLOG_INFO("Disc: {}", hex.empty() ? std::string("tray empty") : hex);
  }

  void DrawDisc() {
    const std::string current = rex::cvar::GetFlagByName("disc_title");
    ImGui::TextUnformatted("In the drive");
    ImGui::TextDisabled(
        "What the disc tile offers to play. The guest wants a title id, so this writes "
        "one -- you pick the game. It takes effect on the next start.");
    if (missing_) {
      ImGui::TextDisabled(
          "%zu more title(s) are in your library but have no game on this machine, so "
          "they cannot go in the drive. Put them in your ROMs folder and press F6.",
          missing_);
    }
    ImGui::Spacing();

    std::string label = "Nothing in the drive";
    bool current_playable = true;
    for (const auto& title : titles_) {
      if (title.hex == current) {
        label = title.name + "  (" + title.hex + ")";
        // A title set before this list started filtering, or one whose game
        // has since gone. Naming it without saying that would explain the
        // empty tile as a bug rather than as a missing game.
        current_playable = title.playable;
        break;
      }
    }
    if (label == "Nothing in the drive" && !current.empty()) {
      // Set to something the library does not know about -- a hand-edited id, or
      // a game that has since gone. Worth showing as-is rather than as "empty".
      label = current + "  (not in the library)";
    }
    ImGui::Text("Currently: %s", label.c_str());
    if (!current_playable) {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                         "That game is not on this machine, so the drive stays empty. "
                         "Choose one below, or eject.");
    }
    ImGui::Spacing();

    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputTextWithHint("##search", "Search the library", search_, sizeof(search_));
    ImGui::SameLine();
    if (ImGui::Button("Eject")) {
      SetDisc("");
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
      Reload();
    }

    const std::string needle = Lowered(search_);
    if (ImGui::BeginChild("titles", ImVec2(0, 220), true)) {
      for (const auto& title : titles_) {
        if (!title.playable) {
          continue;  // it would mount nothing; offering it is offering a dead end
        }
        if (!needle.empty() && Lowered(title.name).find(needle) == std::string::npos &&
            Lowered(title.hex).find(needle) == std::string::npos) {
          continue;
        }
        const bool selected = title.hex == current;
        ImGui::PushID(int(title.id));
        if (ImGui::Selectable(title.name.c_str(), selected)) {
          SetDisc(title.hex);
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        ImGui::TextDisabled("%s", title.hex.c_str());
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  }

  // Kept apart from the disc drive above and the profiles below, because these
  // are not Xbox 360 games at all: no title id, no emulator, no disc.
  void DrawFullGames() {
    ImGui::TextUnformatted("Installed games");
    ImGui::TextDisabled(
        "Games that are not Xbox 360 games -- a Steam library, or a folder with one "
        "game per subfolder. Steam games start through Steam; the rest start "
        "directly. Set the folder with F7.");
    ImGui::Spacing();

    if (ImGui::BeginChild("fullgames", ImVec2(0, 150), true)) {
      if (full_games_.empty()) {
        ImGui::TextDisabled("None found. Point 'Installed games' at a folder in F7.");
      }
      for (size_t i = 0; i < full_games_.size(); ++i) {
        const auto& game = full_games_[i];
        ImGui::PushID(int(i));
        if (ImGui::Button("Play")) {
          nxe_pc::Launch(game);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(game.name.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        ImGui::TextDisabled("%s", game.kind == nxe_pc::Kind::kSteam ? "Steam" : "exe");
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    if (ImGui::Button("Rescan installed games")) {
      full_games_ = nxe_pc::Scan();
    }
  }

  void DrawProfiles() {
    ImGui::TextUnformatted("Profiles");
    ImGui::TextDisabled(
        "Everything here appears in Switch Profile. Add one from a real console -- a "
        "single file named for its XUID -- or from Xenia, which keeps them as folders.");
    ImGui::Spacing();

    if (ImGui::BeginChild("profiles", ImVec2(0, 130), true)) {
      if (profiles_.empty()) {
        ImGui::TextDisabled("None imported yet.");
      }
      for (const auto& profile : profiles_) {
        ImGui::Text("%s", profile.gamertag.empty() ? "(no gamertag)" : profile.gamertag.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0f);
        ImGui::TextDisabled("%s", profile.xuid_text.c_str());
      }
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(340.0f);
    ImGui::InputTextWithHint("##profile", "A profile package or folder", profile_,
                             sizeof(profile_));
    ImGui::SameLine();
    if (ImGui::Button("File...")) {
      const auto picked = nxe_setup::PickPath("Choose the profile package", false);
      if (!picked.empty()) {
        std::snprintf(profile_, sizeof(profile_), "%s", picked.c_str());
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Folder...")) {
      const auto picked = nxe_setup::PickPath("Choose the profile folder", true);
      if (!picked.empty()) {
        std::snprintf(profile_, sizeof(profile_), "%s", picked.c_str());
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import")) {
      const std::string chosen(profile_);
      if (chosen.empty()) {
        REXLOG_WARN("Profiles: choose a profile to import first");
      } else {
        std::string storage = rex::cvar::GetFlagByName("storage_root");
        if (storage.empty()) {
          storage = "storage";
        }
        imported_ = nxe_setup::RunTool(
            "import_profile.py", "\"" + chosen + "\" --storage \"" + storage + "\"");
      }
    }

    if (imported_) {
      ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                         "Importing in the console window. Press Rescan when it finishes.");
    }
    ImGui::Spacing();
    if (ImGui::Button("Rescan profiles")) {
      // The import runs as a separate process, so there is nothing to wait on --
      // this is how the list hears about what it produced. Invalidating is what
      // makes Switch Profile see it too, without a restart.
      nxe_profile::InvalidateCaches();
      Reload();
      REXLOG_INFO("Profiles: rescanned, {} staged", profiles_.size());
    }
  }

  static std::string Lowered(const std::string& text) {
    std::string out = text;
    for (char& ch : out) {
      ch = char(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
  }

  std::filesystem::path config_path_;
  std::vector<Title> titles_;
  std::vector<nxe_profile::StagedProfile> profiles_;
  std::vector<nxe_pc::Game> full_games_;
  size_t missing_ = 0;  // library titles with no game staged
  char search_[128] = {};
  char profile_[512] = {};
  bool imported_ = false;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> MakeDiscDialog(rex::ui::ImGuiDrawer* drawer,
                                                     std::filesystem::path config_path) {
  if (!drawer) {
    return nullptr;
  }
  return std::make_unique<DiscDialog>(drawer, std::move(config_path));
}

}  // namespace nxe_disc
