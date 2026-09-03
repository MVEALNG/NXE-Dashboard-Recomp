// Pages the dashboard opens over a container of its own.
//
// channel_slots.cpp builds a container per category -- a marketplace genre, the
// recent players, the inbox -- and pushes a second homepage.xur over it when one
// is opened. Tiles reach that through EcNavToLocalEpixManifest, but the gamer
// blade's Messages item does not: it arrives at XamShowMessagesUI with a user
// index and no slot behind it, so it needs a way in from outside.
#pragma once

#include <cstdint>
#include <string>

#include <rex/ppc.h>

namespace nxe_channels {

// Open the page built for "<kind>:<name>", e.g. ("messages", "Messages").
//
// False when no page was built for that key, when there is no navigator to push
// onto, or when the push itself fails -- in every case nothing has been shown
// and the caller should fall back to whatever it did before.
bool OpenCategoryPage(PPCContext& ctx, uint8_t* base, const std::string& kind,
                      const std::string& name);

// Navigate to a scene the way the dashboard's own items do.
//
// a4 parameterises the scene -- for gamer.xzp it is the gamer being shown, which
// blade_nav.h keeps. a5 is unused by everything seen so far.
int32_t NavigatePackageScene(PPCContext& ctx, uint8_t* base, uint32_t nav,
                             const std::string& package, const std::string& scene,
                             uint32_t a4, uint32_t a5);

// Push a scene straight out of a package, by name.
//
// For the scenes the dashboard ships but never navigates to itself -- the real
// gamer blade in gamer.xzp, for one. Returns the shell's own result.
int32_t PushPackageScene(PPCContext& ctx, uint8_t* base, uint32_t nav,
                         const std::string& package, const std::string& scene);

// Open a page on a navigator the caller already has.
//
// The gamer blade navigates with its own navigator (see blade_nav.h), not the
// one the dashboard's tiles use, and a page pushed onto the wrong one is
// refused with 0x8030000B.
int32_t OpenCategoryPageOn(PPCContext& ctx, uint8_t* base, uint32_t nav,
                           const std::string& kind, const std::string& name);

// As above, but reporting the shell's own result rather than just success.
//
// 0x8030000B means a scene transition is in progress and the push was refused;
// a caller that is not driven by a tile has to wait that out and ask again.
int32_t OpenCategoryPageResult(PPCContext& ctx, uint8_t* base, const std::string& kind,
                               const std::string& name);

// Whether a page was built for "<kind>:<name>". Lets a caller keep its old
// behaviour instead of swallowing a button press that would do nothing.
bool HasCategoryPage(const std::string& kind, const std::string& name);

// Remember the navigator the shell is pushing scenes onto.
//
// OpenCategoryPage has to push onto the same one, and asking 0x92141108 for a
// navigator hands back a different one -- a page opened on that comes up fine
// and then faults on B. Every navigation passes through dash_280e carrying the
// right handle, so it is recorded there rather than derived.
void NoteNavigator(uint32_t nav);

}  // namespace nxe_channels
