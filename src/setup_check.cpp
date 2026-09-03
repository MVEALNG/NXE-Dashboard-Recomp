// The settings a new install has to supply, and a startup check that says which
// are missing.
//
// Most of what makes this dashboard look like a console is content it cannot
// ship: themes, avatar items, game images, a profile. Each is a directory
// somebody has to point at, and until now nothing said so -- an unset theme
// directory and a broken theme importer look exactly alike from the sofa.
//
// Three of these are defined here because nothing else needed them yet. The
// rest already exist elsewhere and are only listed, so setup asks for them in
// one place without owning them.

#include "setup_check.h"

#include <filesystem>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "install_paths.h"

// Where Xbox 360 game images are kept.
//
// Not the emulator -- that is game_emulator -- and not the storage tree either.
// A game reaches the library today by being staged as content; this is the
// folder of discs and installs it would be staged from, and what the library
// refresh will scan.
REXCVAR_DEFINE_STRING(roms_dir, "", "Games",
                      "Folder holding Xbox 360 game images (ISO, XEX, GOD). Used to find "
                      "games for the library; empty disables the scan.");

// Where Xbox 360 theme packages are kept.
//
// tools/import_themes.py unpacks a signed STFS theme into the content tree,
// because ContentManager::GetPackages skips anything that is not a directory.
// This is the folder it imports *from*, which has to be supplied: themes are
// the console's, not ours to distribute.
// Where games that are not Xbox 360 games are installed.
//
// Either a Steam library -- the folder holding steamapps -- or a plain folder
// with one game per subfolder. Steam is read from its own manifests, which name
// every installed game and its app id; a plain folder has to be guessed at.
// Four of them, because one is not how anybody actually installs games.
//
// Steam in one, EA in another, Ubisoft in a third, and whatever was installed
// outside a launcher in the fourth. Each is scanned independently and works out
// for itself whether it is a Steam library -- there is no ordering to get right
// and no reason a particular launcher has to go in a particular row.
REXCVAR_DEFINE_STRING(full_games_dir, "", "Games",
                      "Folder of installed PC games: a Steam library, or a folder with "
                      "one game per subfolder. Empty disables it.");
REXCVAR_DEFINE_STRING(full_games_dir_2, "", "Games",
                      "A second folder of installed PC games.");
REXCVAR_DEFINE_STRING(full_games_dir_3, "", "Games",
                      "A third folder of installed PC games.");
REXCVAR_DEFINE_STRING(full_games_dir_4, "", "Games",
                      "A fourth folder of installed PC games.");

REXCVAR_DEFINE_STRING(themes_dir, "", "Dashboard",
                      "Folder holding Xbox 360 theme packages to import. Themes cannot be "
                      "shipped with the dashboard, so this is yours to supply.");

// The Movie Database key, for the Video Marketplace row.
//
// Held here so setup can ask for it once and write it where the fetch tool
// looks (tools/.tmdb_key). The dashboard never calls TMDB itself -- it reads
// the file that tool writes -- so this is a setup value rather than a runtime
// one.
//
// TMDB, not IMDb: IMDb has no public API, and the video row has always come
// from themoviedb.org. See tools/fetch_video.py.
REXCVAR_DEFINE_STRING(tmdb_key, "", "Dashboard",
                      "themoviedb.org API key, used by tools/fetch_video.py to fill the "
                      "Video Marketplace. Free from themoviedb.org/settings/api.");

REXCVAR_DECLARE(std::string, storage_root);
REXCVAR_DECLARE(std::string, game_art_dir);
REXCVAR_DECLARE(std::string, game_icon_dir);
REXCVAR_DECLARE(std::string, game_emulator);
REXCVAR_DECLARE(std::string, avatar_asset_pack_dir);
REXCVAR_DECLARE(std::string, boot_video);
REXCVAR_DECLARE(std::string, avatar_closet_dir);

namespace nxe_setup {
namespace {

// Does this setting point at something that is actually there?
//
// A path is resolved the way the rest of the dashboard resolves one, so a
// relative setting is judged against the same root it will be read from later
// rather than the working directory, which is not always the same place.
bool Exists(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::exists(nxe_paths::Resolve(value), ec);
}

Requirement Make(const char* setting, const char* label, const char* note, bool required,
                 Pick pick = Pick::kFolder) {
  Requirement out;
  out.setting = setting;
  out.label = label;
  out.note = note;
  out.required = required;
  out.pick = pick;
  out.value = setting ? rex::cvar::GetFlagByName(setting) : std::string();
  out.present = Exists(out.value);
  return out;
}

}  // namespace

std::vector<Requirement> Check() {
  std::vector<Requirement> out;

  // Required: without these the dashboard has nowhere to keep anything, or
  // nothing to draw a shelf with.
  out.push_back(Make("storage_root", "Xbox 360 storage",
                     "Where profiles, saves, themes and installed games are kept.", true));

  // Wanted: each one turns a part of the dashboard on. None of them stops it
  // starting, and saying so matters -- a first run with none of this set should
  // still reach the blade.
  out.push_back(Make("roms_dir", "Xbox 360 games",
                     "Folder of game images, so the library can find and launch them.",
                     false));
  out.push_back(Make("full_games_dir", "Installed games 1",
                     "A Steam library, or a folder with one game per subfolder. These "
                     "are not Xbox 360 games and are launched directly.", false));
  out.push_back(Make("full_games_dir_2", "Installed games 2",
                     "Another games folder -- EA, Ubisoft, or anything installed outside "
                     "a launcher. Each row is scanned the same way.", false));
  out.push_back(Make("full_games_dir_3", "Installed games 3",
                     "A third games folder.", false));
  out.push_back(Make("full_games_dir_4", "Installed games 4",
                     "A fourth games folder.", false));
  out.push_back(Make("game_emulator", "Emulator",
                     "The emulator a game is launched with, e.g. xenia_canary.exe.", false,
                     Pick::kFile));
  out.push_back(Make("themes_dir", "Xbox 360 themes",
                     "Folder of theme packages. Themes cannot be shipped, so this is yours "
                     "to supply.", false));
  // Two different things, and calling both "avatar items" is why they got
  // confused. The pack is the base bodies, faces and starter clothing every
  // avatar is built from; the closet is what somebody bought or was awarded.
  out.push_back(Make("avatar_asset_pack_dir", "Avatar asset pack",
                     "The folder holding AvatarAssetPack.toc and "
                     "AvatarAssetPackLegacyV1.toc, extracted from the FFFE07DF00000002 "
                     "package. Base bodies, faces and the starter clothing.", false));

  // Closet items are referenced by GUID from a saved outfit, so an avatar
  // wearing something this machine has never imported simply loses it from the
  // editor's grids -- which looks like the outfit was never saved.
  out.push_back(Make("avatar_closet_dir", "Avatar closet",
                     "Imported marketplace and award items -- props, outfits, shirts, "
                     "trousers, shoes, accessories -- as <guid>.bin plus "
                     "closet_index.tsv. Blank uses a 'closet' folder inside the asset "
                     "pack.", false));
  out.push_back(Make("avatar_editor_exe", "Avatar editor",
                     "The avatar editor, run when Customize Avatar is chosen.", false,
                     Pick::kFile));

  // The animation that plays over the dashboard while it loads. Somebody
  // rebuilding a console will want their own, and there is not one to ship.
  out.push_back(Make("boot_video", "Boot animation",
                     "A video played while the dashboard loads. Leave blank for none.",
                     false, Pick::kFile));
  out.push_back(Make("game_art_dir", "Game covers",
                     "Where cover art is kept. tools/fetch_covers.py fills it from your "
                     "Xbox LIVE play history.", false));
  out.push_back(Make("game_icon_dir", "Game icons",
                     "Where title icons are kept, alongside the covers.", false));

  // Not a path: a key, and the only one of these that is somebody else's
  // service rather than a folder on this disk.
  Requirement key = Make("tmdb_key", "TMDB API key",
                         "Fills the Video Marketplace. Free from themoviedb.org/settings/api.",
                         false, Pick::kText);
  key.present = !key.value.empty();
  out.push_back(key);

  return out;
}

void Report() {
  const auto items = Check();

  size_t missing_required = 0;
  size_t missing_optional = 0;
  for (const auto& item : items) {
    if (item.present) {
      continue;
    }
    if (item.required) {
      ++missing_required;
    } else {
      ++missing_optional;
    }
  }

  if (!missing_required && !missing_optional) {
    REXLOG_INFO("Setup: everything is pointed at something");
    return;
  }

  // Required first and loudest. The rest is a list of what is switched off, not
  // a list of faults -- a dashboard with no themes directory is configured, it
  // simply has no themes.
  for (const auto& item : items) {
    if (item.present || !item.required) {
      continue;
    }
    REXLOG_ERROR("Setup: {} is not set up. {}  (setting: {} = '{}')", item.label, item.note,
                 item.setting, item.value);
  }
  if (missing_optional) {
    REXLOG_WARN("Setup: {} optional item(s) not set; those parts of the dashboard stay empty",
                missing_optional);
    for (const auto& item : items) {
      if (item.present || item.required) {
        continue;
      }
      REXLOG_WARN("Setup:   {:<18} {}  (setting: {})", item.label,
                  item.value.empty() ? "not set" : "points at nothing that is there",
                  item.setting);
    }
  }
}

}  // namespace nxe_setup
