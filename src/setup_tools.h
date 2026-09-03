// Bits every setup dialog needs: picking a path, and running a tool.
//
// Shared rather than copied. The first-run dialog needed them first; the disc
// dialog needs the same three, and a second copy of RunTool would be a second
// place for the quoting to be wrong.
#pragma once

#include <filesystem>
#include <string>

namespace nxe_setup {

// Where the Python tools are, or empty if they cannot be found.
std::filesystem::path ToolsDir();

// A Windows file or folder picker. Empty when cancelled.
std::string PickPath(const std::string& title, bool folder);

// Run one of the tools in a console window of its own.
//
// Visible on purpose: signing in prints a code to type at microsoft.com/link
// and waits, and importing prints what it found. A hidden window would leave
// both invisible.
bool RunTool(const std::string& script, const std::string& args = {});

}  // namespace nxe_setup
