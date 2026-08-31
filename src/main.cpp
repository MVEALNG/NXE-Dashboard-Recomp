// nxe_dash - ReXGlue Recompiled Project

#include "generated/$flash_dash/nxe_dash_init.h"

#include <rex/cvar.h>

#include "nxe_dash_app.h"

// Serve profile setting 0x63E80044, the avatar manifest.
//
// Off by default. Answering it drives the dashboard into the avatar renderer,
// which faults at guest 0x92471038 in mip-map generation -- see the note above
// kAvatarManifestSetting in src/profile_settings.cpp. The profile staged now
// carries a genuine manifest (male body, TOC-valid assets, its own XUID at
// 0x380) and real AvatarAssets, which is the condition xenia-canary PR #768
// says is needed to test avatars at all, so this exists to retest that fault
// rather than to leave it permanently unanswerable.
//
// Defined here rather than in a header because a cvar defined in a DLL
// registers after config parsing.
// On by default now. It used to be held back because answering it faulted the
// avatar renderer, but that was before the native avatar pipeline was ported in
// -- and withholding it was itself why the blade drew a featureless body: this
// setting is how the dashboard learns it has an avatar at all. With it off the
// dashboard called XamAvatarInitialize and nothing else, never asking for a
// single asset. With it on it builds the real one (40 assets, none missing).
// Which staged profile to sign in, by offline XUID -- the name of its
// directory under the content root. Empty takes the first one found, which is
// the only behaviour there was when a single profile was staged.
// Selecting a profile in the Sign In list restarts the dashboard as that
// profile. Off leaves the selection inert, which is what it was before.
// Report every profile in the Sign In list as an offline account.
//
// The guest buckets profiles by their online XUID and Xbox LIVE is
// Disconnected here, so a profile that claims to be a LIVE account may not be
// offerable for local sign-in. Off by default -- each profile reports what its
// own Account blob says, which is the truth -- but this forces the question if
// the list still comes up empty.
REXCVAR_DEFINE_BOOL(signin_profiles_offline, false, "Profile",
                    "Report every profile in the Sign In list as an offline (non-LIVE) "
                    "account, whatever its Account blob says.");

// What to tell the guest after switching profiles in place.
//
// Empty by default. 0xA is XN_SYS_SIGNINCHANGED, which is the notification a
// console raises when the signed-in user changes -- but raising it here had the
// dashboard re-enumerate and immediately ask to sign the other profile back in,
// switching back and forth. Left as a cvar to try again once the switch itself
// is settled. Never 0xE (profile setting changed): the dashboard answers that
// one by powering down -- see avatar_change_notify_ids.
// How long after startup to ignore sign-in requests. Zero by default.
//
// The dashboard logs a profile in for itself while booting, which is
// indistinguishable at XamUserLogon from someone choosing one -- same flags,
// same XUID array -- so this was set to 20s to tell them apart by timing. That
// was wrong: refusing the boot request leaves the dashboard not asking again,
// and a later selection then produces no XamUserLogon at all, so clicking a
// profile did nothing.
//
// Its logon *is* how a selection arrives, so all of them are honoured. The cost
// is that the dashboard restores its own last profile shortly after boot, which
// can override --profile_xuid. Raise this if that is more annoying than useful.
REXCVAR_DEFINE_INT32(profile_switch_grace_seconds, 0, "Profile",
                     "Ignore profile switch requests for this many seconds after startup.");

// 0xA is XN_SYS_SIGNINCHANGED, what a console raises when the signed-in user
// changes. It does make a switch take hold -- with it, settings go 1 -> 33, the
// library 2 titles/0G -> 39 titles/25,960G, and the avatar rebuilds for the new
// profile, none of which happens without it.
//
// It was off for a while because it made the dashboard alternate between
// profiles every few seconds. That turned out to be a symptom of booting
// already "signed in" as a profile the dashboard did not recognise: it re-read,
// found a stranger, and went looking for someone to sign in. Booting signed out
// removed the loop -- a signed-out boot makes no logon attempts at all -- and
// with it gone this is just what tells the dashboard to re-read after a
// genuine sign-in.
//
// Never 0xE (profile setting changed): the dashboard answers that one by
// powering down -- see avatar_change_notify_ids.
REXCVAR_DEFINE_STRING(profile_switch_notify_ids, "", "Profile",
                      "Notification ids raised after switching profiles, comma-separated.");

// Act on the sign-in requests the dashboard makes.
//
// Not optional, despite reading like a feature switch: the dashboard will not
// leave the Sign In screen until a logon it asked for is honoured. Turning this
// off leaves it sitting in the chooser forever with no gamertag, library or
// avatar -- which is exactly what it did while this was false.
REXCVAR_DEFINE_BOOL(profile_switch_restart, true, "Profile",
                    "Honour the dashboard's sign-in requests. Off leaves it stuck in the "
                    "Sign In screen.");

// Signed in at startup.
//
// Set to a XUID rather than left empty on purpose. Empty boots the console
// signed out, which is what a console really does and is the right model -- but
// signing in from the chooser is not finished: the sign-in itself takes (the
// profile, settings and library all switch) and the dashboard then does not
// repopulate its blade, so it looks signed out. Until that is understood, a
// profile at boot is the usable state.
//
// Change this to sign in as someone else; clear it to boot signed out and pick
// up that work again.
// Boot with nobody signed in, the way a console does.
//
// Off by default because it is unfinished, not because it is wrong. Signing in
// from the chooser does change profile -- settings, library, gamertag and
// avatar all follow -- but the dashboard never repopulates its blade
// afterwards, so it still looks signed out; and with nobody signed in at boot
// the dashboard sits in the chooser rather than reaching My Xbox.
//
// Turn it on to pick that work back up. What is missing is what the dashboard
// reads, after XN_SYS_SIGNINCHANGED, to decide a user is present.
REXCVAR_DEFINE_BOOL(boot_signed_out, false, "Profile",
                    "Boot with no profile signed in (unfinished; the dashboard will sit in "
                    "the Sign In screen).");

REXCVAR_DEFINE_STRING(profile_xuid, "E030000000A8C189", "Profile",
                      "Offline XUID of the profile signed in at startup. Empty boots signed "
                      "out (see the note above; the chooser cannot yet finish a sign-in).");

// Vertical sync, applied to the GPU plugin's own `vsync` cvar after it loads.
//
// Setting `vsync` directly in the config file does not survive a restart: it
// belongs to rexgpu-xenos, which is loaded after the config has already been
// parsed, so the value is read before the cvar it names exists and is dropped.
// This one lives in the executable, so the config file and command line reach
// it normally, and OnPostSetup copies it across once the plugin is up.
REXCVAR_DEFINE_BOOL(dash_vsync, false, "GPU",
                    "Vertical sync. Off by default; set dash_vsync=true to turn it on "
                    "(setting the plugin's own `vsync` will not persist).");

// Serve gamer tiles as decoded pixels instead of the image file.
//
// Off: the dashboard's image loader is given the PNG, which is what it wants.
// On restores the old behaviour, which drew an empty gamercard.
REXCVAR_DEFINE_BOOL(gamer_tile_decoded, false, "Profile",
                    "Hand gamer tiles to the dashboard as raw pixels rather than the image "
                    "file.");

// Gamercard values, for profiles whose GPD does not carry them.
//
// A negative value means "leave it alone" -- the gamercard then shows whatever
// the profile itself has, which for a freshly made account is nothing.
REXCVAR_DEFINE_INT32(profile_gamerscore, 25000, "Profile",
                     "Gamerscore shown on the gamercard. Negative leaves the profile's own.");

REXCVAR_DEFINE_DOUBLE(profile_rep, 4.5, "Profile",
                      "Reputation shown on the gamercard, 0 to 5 stars. Negative leaves the "
                      "profile's own.");

REXCVAR_DEFINE_INT32(profile_zone, 4, "Profile",
                     "Gamer zone: 1 recreation, 2 pro, 3 family, 4 underground. Negative "
                     "leaves the profile's own.");

REXCVAR_DEFINE_BOOL(avatar_manifest, true, "Avatar",
                    "Serve the profile's avatar manifest (0x63E80044), which is how the "
                    "dashboard discovers the signed-in user's avatar.");

REX_DEFINE_APP(nxe_dash, NxeDashApp::Create)
