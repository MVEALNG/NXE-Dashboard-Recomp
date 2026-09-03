// The navigator the gamer blade navigates with.
//
// The blade's dispatcher (guest 0x922E9108) has the blade in r3 and keeps its
// navigator at +0x10C, but the XamShow*UI functions it calls are given only a
// user index -- so an item that wants to navigate has to reach back for it.
// profile_ui.cpp already does this for Profile; Messages needs the same thing.
//
// Zero when no blade dispatch is running on this thread, which is the signal to
// leave a background caller's behaviour alone.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace nxe_blade {

uint32_t Navigator(uint8_t* base);

// The gamer a scene in gamer.xzp is about.
//
// dash_2a65's fourth argument is a descriptor whose first two words are the
// XUID -- 0xb13ebabe 0xbabebabe here, the same one XamProfileOpen reports. With
// it, GamerRootScene comes up as your own profile menu; with zero it comes up
// bound to nobody and offers Add Friend / Compare Games, the panel a stranger's
// gamercard gets. The dashboard builds one whenever it opens the scene itself,
// so rather than trying to construct the struct, the one it used is kept.
void NoteGamerParam(PPCContext& ctx, uint8_t* base, uint32_t param);
uint32_t GamerParam();

// A descriptor of our own, built from the signed-in XUID rather than caught in
// flight. Reproducible now that XamUserGetXUID reports the staged profile, so a
// scene can be opened bound without having watched the dashboard open one first.
uint32_t OwnGamerDescriptor(PPCContext& ctx, uint8_t* base);

}  // namespace nxe_blade
