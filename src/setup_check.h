// What a new install still needs pointing at.
//
// The dashboard cannot ship themes, avatar items, game images or a profile, so
// every one of those is a directory the person running it has to supply. Until
// there is a setup dialog to ask for them, the least this can do is say plainly
// which are missing rather than failing quietly later -- a theme that never
// applies and a cover that never appears look identical to a bug.
#pragma once

#include <string>
#include <vector>

namespace nxe_setup {

// What answering this looks like, so setup can offer the right picker.
enum class Pick {
  kFolder,  // a directory
  kFile,    // one file: an executable, a video
  kText,    // typed, not browsed for -- a key
};

// One thing a new install needs, and whether it has it.
struct Requirement {
  const char* setting;      // the cvar to set, or nullptr for a file
  const char* label;        // what it is, in the words a person would use
  std::string value;        // what it is set to now
  bool required = false;    // the dashboard is not usable without it
  bool present = false;     // set, and something is actually there
  const char* note;         // why it is wanted, shown when it is missing
  Pick pick = Pick::kFolder;
};

// Every requirement with its current state, in the order a setup dialog should
// ask for them.
std::vector<Requirement> Check();

// Log the result once, at startup. Says nothing when everything required is
// present, so a configured install stays quiet.
void Report();

}  // namespace nxe_setup
