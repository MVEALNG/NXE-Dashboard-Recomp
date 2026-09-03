// Add slots to a channel after its definition has been parsed.
//
// The offline "upsell" channels each define exactly one slot -- the Game
// Marketplace one is the orange "Let us Play" card -- and that is all the
// content there is. On a live console these channels are replaced wholesale by
// definitions downloaded from Xbox LIVE, which is where the row of real tiles
// comes from. Offline there is nothing to download, so the tiles have to be
// built here.
//
// Editing XML is not an option. The manifest is compiled into the executable
// rather than read from homepage.xzp (deleting that package changes nothing),
// it is not served through the epix resource loader at 0x922D6C48 (hooked;
// never called), and it is not resident in guest memory when the "Epix Homepage
// Def" thread starts (swept; not found). So rather than feed the parser
// different text, this calls the same functions the parser calls.
//
// The seam is 0x922DAEF0, the channeldef element handler. When it has just
// consumed the closing channeldef tag its state word at +5208 is 8, and the
// channel object is at +5220 -- the same object the manifest parser created,
// with its id at +24 and its slot list at +48/+52 counted at +56 (capped at
// 0x40). At that point the definition is complete and nothing has consumed it
// yet, so slots appended here are indistinguishable from parsed ones.
//
// Everything below mirrors what 0x922DAEF0 does for real XML, using the same
// constructors and the same field offsets:
//
//   slot    = 0x922D6380(channel)      232 bytes, linked and counted
//             +20 description   (0x922CA4A8, resolves %EvResStr(...)%)
//             +24 description2  (0x922CA4A8)
//             +28 description3  (0x922CA4A8)
//             +32 name          (0x922F18B8)
//             +60 epixid        (0x922F18B8)
//             +68 shallowimg    (0x922F18B8, a .jpg/.png/.jpeg path)
//             +80..+92          up to four onclick pointers
//             +104 visible      0 hides the slot
//   onclick = 0x922CCD88 over a 48-byte allocation
//             +0  action        "EpixCmd"
//             +4  cmd           "EcNavTo..."
//             +8..+32           param1..param7
//             +36 helptext      (0x922CA4A8)
//             +40 button        0x5800 == A
//             +44 postaction
//   epix    = 0x922D4408(channel)      60 bytes, on the channel's own list at
//                                      +60/+64 counted at +68
//             +12 format        1 == EpixScene, 6 == IMG
//             +16 id            (0x922F18B8)
//             +20 path          "Es..." or an image path
//             +28 style         "nogradient" for an IMG epix
//
// The two string setters are not interchangeable, and picking the wrong one is
// silent:
//
//     sub_922F18B8(int *dst, u8 *src)   // src is UTF-8; converts (cp 65001)
//     sub_922F1848(int *dst, int src)   // src is ALREADY UTF-16; wcslen+copy
//
// This handed the epix id to 0x922F1848 as a raw ANSI buffer, so the id was
// stored as whatever those bytes read as wide characters. The slot's own epixid
// went through 0x922F18B8 and came out correct, and 0x922D55B0 wcscmps the two
// to bind them -- so nothing ever matched, no epix was ever bound, and every
// tile drew as an empty card no matter what the epix pointed at.
//
// The format, action and button numbers are the ids the parser itself looks up,
// from the tables at 0x927F2778, 0x927F27C8 and 0x927F2728.
//
// A slot's epixid has to name an epix declared on the same channel -- 0x922D55B0
// rejects duplicates per channel -- so each new slot brings its own epix entry.
//
// That epix does not have to point at a scene compiled into the package. The
// format table at 0x927F2778 has ten entries, and format 6 (IMG) draws a plain
// image file instead, which is how the downloaded channels drew tiles nobody
// had compiled a scene for. The tiles below use it.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "channel_pages.h"
#include "gamer_picture.h"
#include "game_launch.h"
#include "install_paths.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

using namespace rex;

extern "C" {
void __imp__sub_922DAEF0(PPCContext& __restrict ctx, uint8_t* base);  // channeldef handler
void __imp__sub_922D6380(PPCContext& __restrict ctx, uint8_t* base);  // append slot
void __imp__sub_922D4408(PPCContext& __restrict ctx, uint8_t* base);  // append epix
void __imp__sub_922CEB00(PPCContext& __restrict ctx, uint8_t* base);  // create channel
void __imp__sub_922D55B0(PPCContext& __restrict ctx, uint8_t* base);  // epixid -> epix
void __imp__sub_922CCD88(PPCContext& __restrict ctx, uint8_t* base);  // construct onclick
void __imp__sub_92144098(PPCContext& __restrict ctx, uint8_t* base);  // guest allocator
void __imp__sub_921F6190(PPCContext& __restrict ctx, uint8_t* base);  // dash_2a65 navigate
void __imp__sub_922F18B8(PPCContext& __restrict ctx, uint8_t* base);  // set string field
void __imp__sub_922F1848(PPCContext& __restrict ctx, uint8_t* base);  // set epix id
void __imp__sub_922CA4A8(PPCContext& __restrict ctx, uint8_t* base);  // set display string
void __imp__sub_921C2850(PPCContext& __restrict ctx, uint8_t* base);  // dash_28b9, load an image
void __imp__sub_922D5B70(PPCContext& __restrict ctx, uint8_t* base);  // prepare one slot
void __imp__sub_922D48B8(PPCContext& __restrict ctx, uint8_t* base);  // load a slot's image epix
void __imp__sub_9220BBF8(PPCContext& __restrict ctx, uint8_t* base);  // wrap a URL for the cache
void __imp__sub_922DAC08(PPCContext& __restrict ctx, uint8_t* base);  // load a channel definition
void __imp__sub_922D60B8(PPCContext& __restrict ctx, uint8_t* base);  // prepare a channel's slots
void __imp__sub_922C5BF8(PPCContext& __restrict ctx, uint8_t* base);  // EcNavToLocalEpixManifest
void __imp__sub_922CD1D8(PPCContext& __restrict ctx, uint8_t* base);  // find or make a container
void sub_92141108(PPCContext& __restrict ctx, uint8_t* base);         // dash_2968, a navigator
void __imp__sub_922D30C8(PPCContext& __restrict ctx, uint8_t* base);  // command dispatcher
void __imp__sub_922C5580(PPCContext& __restrict ctx, uint8_t* base);  // push a scene
void __imp__sub_922CED88(PPCContext& __restrict ctx, uint8_t* base);  // wait on a container's load
void __imp__sub_922CF3F8(PPCContext& __restrict ctx, uint8_t* base);  // select a channel by id
void __imp__sub_922CC270(PPCContext& __restrict ctx, uint8_t* base);  // find a container by url
void __imp__sub_922CEA50(PPCContext& __restrict ctx, uint8_t* base);  // container addref
}

// The epix manager, and the scene the manifest commands open.
//
// 0x922C5580(nav, "homepage.xzp", "homepage.xur", params, 1) pushes a second
// instance of the homepage itself, bound to whichever container the params name.
// That is the whole drill-down: the page a category opens is the same shell as
// the one it was opened from, showing a different container's channels, and B
// pops it off the navigation stack for free.
constexpr uint32_t kEpixManager = 0x92828B10;
constexpr uint32_t kHomepagePackage = 0x920278B4;  // L"homepage.xzp"
constexpr uint32_t kHomepageScene = 0x920278D0;    // L"homepage.xur"

REXCVAR_DEFINE_BOOL(channel_extra_slots, true, "Dashboard",
                    "Add tiles to the offline Game Marketplace channel, which otherwise carries "
                    "only its single upsell card.");

REXCVAR_DEFINE_BOOL(channel_community_slots, false, "Dashboard",
                    "Fill in the COMMUNITY channel from the definition the dash ships but fails to "
                    "load. Without this the tab appears with nothing in it.");

// Where the friends list comes from.
//
// A plain file rather than anything live. The dashboard has no business holding
// Microsoft account tokens or making network calls mid-frame, and a snapshot
// fails honestly: a stale list, never a hang. It also means the feature works
// for somebody who has no intention of signing in to anything -- the file can be
// written by hand, or copied from someone else -- and tools/fetch_friends.py is
// only one way of filling it in.
//
// The Xbox PC app was the obvious source and turned out not to be one: its
// AsyncCache.db holds 7,942 rows of store, library and achievement data and no
// social graph at all. The app fetches people from peoplehub.xboxlive.com on
// demand and never persists them, so there is nothing local to read.
REXCVAR_DEFINE_STRING(friends_list, "gamedir/friends.txt", "Dashboard",
                      "Pipe-delimited friends list to show on the Friends channel: "
                      "gamertag|presence|image|xuid, one per line, # for comments. Empty "
                      "disables the feature.");

// The Xbox 360 storefront was shut down on 29 July 2024, so the Game Marketplace
// has no service left to fetch from: every EDS host (marketplace.xboxlive.com,
// catalog.xboxlive.com, eds.xboxlive.com) answers 404, and the modern catalogue
// holds no 360 entries -- looking up Halo 3's title id as an XboxTitleId returns
// zero products. The catalogue survives only as preservation work, so the tiles
// come from a file that tools/fetch_marketplace.py builds from x360db plus the
// real box art Microsoft's download.xbox.com still serves.
REXCVAR_DEFINE_STRING(marketplace_list, "gamedir/marketplace.txt", "Dashboard",
                      "Pipe-delimited Xbox 360 Marketplace titles to show on the Game "
                      "Marketplace channel: name|subtitle|image|titleid, one per line, "
                      "# for comments. Empty disables the feature. "
                      "tools/fetch_marketplace.py writes it.");

// How the Game Marketplace row is laid out.
//
// Categories alone is what the real storefront showed: a row of genres, and the
// games behind whichever one you opened. That is the default here.
REXCVAR_DEFINE_BOOL(channel_marketplace_categories_only, true, "Dashboard",
                    "Show the Game Marketplace as a row of categories. Off lays the row out "
                    "inline instead, with each category followed by a few of its games.");

REXCVAR_DEFINE_INT32(channel_marketplace_row_games, 2, "Dashboard",
                     "Games shown after each category when the row is laid out inline, which "
                     "only happens with channel_marketplace_categories_only off. A channel "
                     "holds 64 slots in total, categories included.");

// Opening a category, on both marketplace rows.
//
// 0x922C5BF8 builds a container for a URL and pushes a second homepage.xur over
// it: the page a category opens is the same shell showing a different
// container's channels, and B pops it off the navigation stack for free.
//
// What used to stop this was the manager's own lookup. 0x922CC270 never matched
// our URLs -- asking it for a container already built handed back a second,
// empty one -- so the page missed in the same way, was given an empty container,
// and waited on a fetch that was never coming. Answering that lookup is what
// makes the page find the container with the titles in it.
REXCVAR_DEFINE_BOOL(channel_marketplace_pages, true, "Dashboard",
                    "Let a category tile open a page of its titles, on both the Game and the "
                    "Video marketplace. Off leaves the categories inert.");

// Nothing preserved the Xbox 360's video catalogue. The marketplace archive
// that covers the game side -- 72,337 rows of add-ons, themes, demos, avatar
// items and gamer pictures -- has no movie or TV content type in it at all, and
// Microsoft's surviving catalogue will resolve a film by id but refuses to be
// browsed: search comes back empty and the movie and TV listings are capped at
// four titles each. So this row is filled from a film database rather than from
// anything that was ever on Xbox, which is a real difference from the Game
// Marketplace and the reason it reads a file of its own.
REXCVAR_DEFINE_STRING(video_list, "gamedir/video.txt", "Dashboard",
                      "Pipe-delimited films and television for the Video & Music Marketplace "
                      "channel: name|subtitle|image|id|kind, one per line, # for comments. "
                      "Empty disables the feature. tools/fetch_video.py writes it.");

// How a title draws on a category page.
//
// 0x922D5B70 copies a slot's <boxstyle> to its image epix's +28 and 0x922D3A80
// turns that into a scene name, "<style>.xur", so how a tile looks is data and
// not code.
//
// Only four of those scenes are actually in the packages -- nogradient,
// Transnograd, OrangeBox and imgdest, all in homepage.xzp. The longer list that
// used to be written here (picture, advert, Transpic, GreenBox, romechan) came
// from names the shell knows rather than scenes it ships, and naming one of them
// does not fall back to the default: the shell goes looking for a scene that is
// not there and the page stops half open, which is exactly what romechan did.
// Anything set here has to be one of the four.
//
// The shell's own default, "picture", is not among them either and still works,
// so it is reached some other way; leaving this empty gets it.
//
// This only touches the pages behind a category. The category row keeps the
// default, so the shelf of genre cards is unaffected whatever this is set to.
REXCVAR_DEFINE_STRING(channel_marketplace_tile_style, "nogradient", "Dashboard",
                      "<boxstyle> for the titles on a category page. Only nogradient, "
                      "Transnograd, OrangeBox and imgdest exist in the packages; any other "
                      "name hangs the shell when the page opens. Empty uses the default.");

// The people recently played with, from peoplehub's recentplayers view -- the
// same list the 360 kept beside Friends, and the service still serves it.
//
// It is a category rather than tiles on the row because there are far more of
// them than a channel can hold: 0x922D6018 refuses any slot past the 64th, and
// Friends already spends 18 on Add Friend and the friends themselves. So the row
// gets one tile and the list lives on the page behind it.
REXCVAR_DEFINE_STRING(recent_list, "gamedir/recent.txt", "Dashboard",
                      "Pipe-delimited recent players for the Friends channel: "
                      "name|subtitle|image|xuid|kind, one per line, # for comments. Empty "
                      "disables the feature. tools/fetch_recent_players.py writes it.");

// The two sides of the social graph, from peoplehub's social and followers
// views. The profile page has always known these numbers -- "16 following, 18
// followers" comes free with the profile summary -- and had nothing behind them,
// so the line was there to read and not to press. These are the lists.
//
// Following covers the same people friends.txt does, and is not redundant with
// it: that is a row of tiles on the channel and spends a slot on each, while this
// is the whole list behind a heading. Same reason Recent Players is a page.
REXCVAR_DEFINE_STRING(following_list, "gamedir/following.txt", "Dashboard",
                      "Pipe-delimited list of the people you follow: "
                      "name|subtitle|image|xuid|kind, one per line, # for comments. Empty "
                      "disables the feature. tools/fetch_social.py writes it.");

REXCVAR_DEFINE_STRING(followers_list, "gamedir/followers.txt", "Dashboard",
                      "Pipe-delimited list of the people who follow you: "
                      "name|subtitle|image|xuid|kind, one per line, # for comments. Empty "
                      "disables the feature. tools/fetch_social.py writes it.");

// A page per person, so selecting somebody shows who they are.
//
// The tile used to call EcShowGamerCard, and that is XAM's own system gamercard
// -- the same thing the gamer blade's Profile item called, and the same reason
// that became a page of ours instead (see profile_ui.cpp). It cannot be drawn
// here, so pressing a friend did nothing.
//
// Keyed by gamertag rather than xuid: the page key is the name the tile already
// carries, so no field has to be spent pointing at it and friends.txt keeps the
// xuid for the gamercard fallback.
// The games this account has played, and a page of detail for each.
//
// games.txt has been written since before there was anywhere to put it and was
// never read: there is no games_list setting and never was. These are the same
// history in the shape the dashboard actually consumes -- one category with the
// titles under it, exactly as Recent Players is built.
//
// Separate from the Game Marketplace row beside it on purpose. That row is a
// shop, filled from the preserved catalogue; this is a shelf, filled from what
// you have played.
REXCVAR_DEFINE_STRING(titles_list, "gamedir/titles.txt", "Dashboard",
                      "Pipe-delimited games you have played, as a row: "
                      "name|subtitle|image|titleid|kind, one per line, # for comments. "
                      "Empty disables the row. tools/fetch_title_stats.py writes it.");

// The achievement list behind each game, from achievements.xboxlive.com.
//
// Its own file rather than more rows on the game's page, because ParseListFile
// stops at 1024 entries and 47 games at 50 achievements apiece is twice that.
REXCVAR_DEFINE_STRING(achievements_list, "gamedir/achievements.txt", "Dashboard",
                      "Pipe-delimited achievement lists, one category per game: "
                      "name|description|image|id|kind, one per line, # for comments. "
                      "Empty disables them. tools/fetch_achievements.py writes it.");

REXCVAR_DEFINE_STRING(gamestats_list, "gamedir/gamestats.txt", "Dashboard",
                      "Pipe-delimited per-game detail pages: achievements, gamerscore "
                      "and time played. A row marked 'category' starts a game. Empty "
                      "disables the pages. tools/fetch_title_stats.py writes it.");

REXCVAR_DEFINE_STRING(people_list, "gamedir/people.txt", "Dashboard",
                      "Pipe-delimited per-person detail pages: "
                      "label|value|image|id|kind, one per line, # for comments. A row "
                      "marked 'category' starts a person. Empty disables the feature. "
                      "tools/fetch_social.py writes it.");

// The Xbox Live inbox, from msg.xboxlive.com.
//
// The gamercard draws a hard 0 beside its envelope and the Messages blade lived
// in the Guide, which this port cannot open -- but the service behind it is
// still up, so the messages can be shown without it. A category for the same
// reason as Recent Players: one tile on the row, the inbox on the page behind.
REXCVAR_DEFINE_STRING(messages_list, "gamedir/messages.txt", "Dashboard",
                      "Pipe-delimited inbox for the Friends channel: "
                      "sender|preview|image|id|kind, one per line, # for comments. Empty "
                      "disables the feature. tools/fetch_messages.py writes it.");

// Your Xbox profile, from profile.xboxlive.com.
//
// Profile on the gamer blade calls XamShowGamerCardUI -- XAM's own system
// gamercard, which this port has no UI layer to draw. The service behind it is
// still up, so the gamertag, score, reputation, location and bio can be shown
// as a page of the dashboard's own instead. A category for the same reason as
// the inbox: it is opened from the blade rather than from a row.
REXCVAR_DEFINE_STRING(profile_list, "gamedir/profile.txt", "Dashboard",
                      "Pipe-delimited profile for the gamer blade's Profile item: "
                      "label|value|image|id|kind, one per line, # for comments. Empty "
                      "disables the feature. tools/fetch_profile.py writes it.");

// The gamer pictures Change Gamer Picture offers.
//
// No .xur for that screen ships in any package -- on a console it is one of
// XAM's Guide screens -- so it is a page of ours, built from the default set in
// the dumped shared resources. Choosing one writes it into the profile.
REXCVAR_DEFINE_STRING(gamerpics_list, "gamedir/gamerpics.txt", "Dashboard",
                      "Pipe-delimited gamer pictures: name|subtitle|image|source file|"
                      "kind, one per line, # for comments. Empty disables the feature. "
                      "tools/import_gamerpics.py writes it.");

REXCVAR_DEFINE_STRING(gamerpics_dir, "gamedir/gamerpics_src", "Dashboard",
                      "Folder of the original gamer pictures, named as the tile API "
                      "keys them: <size>_<title id><image id>.png.");

REXCVAR_DEFINE_BOOL(channel_video_categories_only, true, "Dashboard",
                    "Show the Video & Music Marketplace as a row of genres. Off lays the row "
                    "out inline, with each genre followed by a few of its titles.");

REXCVAR_DEFINE_BOOL(channel_welcome_tab, true, "Dashboard",
                    "Add the kiosk's 'Welcome to Xbox 360' channel as a top-level tab of its own. "
                    "Leaves every channel the dashboard already defines alone.");

// The file loader at 0x922D6F10 hands the path straight to NtOpenFile, so this
// is a device path rather than a URL. The game directory is mounted at
// \Device\Harddisk0\Partition1 -- the boot log says so -- and the images sit
// under images/ inside it.
REXCVAR_DEFINE_STRING(channel_slot_image_root, "\\Device\\Harddisk0\\Partition1", "Dashboard",
                      "Root for tile images; the path from the table is appended. A value "
                      "containing '://' is treated as a URL instead and probed against the image "
                      "loader; anything else is an NtOpenFile path.");

// A picture is not a tile on its own.
//
// With only <shallowimg> set, 0x922D5B70 builds the IMG epix and parks it at
// slot+72 exactly as it does for the kiosk -- the trace confirms format 6 and
// the right path on every tile -- and nothing draws. A four-minute session with
// both tabs open asked the image loader for 42 distinct pictures and never once
// for one of these, so the picture is not being rejected, it is never requested.
//
// What those slots do not have is a scene: +76 is zero, and the dashboard's own
// slots all carry one (EsOfflineGames, EsWhatsHot, and so on). The epix supplies
// a picture *to* a scene; it is not a visual by itself. EsAdvert is the scene in
// the table at 0x92028400 whose whole job is a slot that is just an image.
//
// Both can be set at once. 0x922D5B70 builds the shallowimg epix under
// "if ( !a1[19] )" -- no scene bound *yet* -- and only resolves the epixid
// afterwards at LABEL_13, so the picture lands at +72 and the scene at +76 in
// the same pass.
// Empty, and deliberately so. EsAdvert was tried here and is worse than
// nothing: the scene resolves and takes the tile over completely, drawing
// neither the picture nor the captions, so the tiles lost the text they already
// had. That is a useful negative -- a scene named through the epixid clearly
// does reach the renderer -- but it is not the way to a picture, and a tile with
// no scene at least keeps its text.
REXCVAR_DEFINE_STRING(channel_slot_image_scene, "", "Dashboard",
                      "Scene used to draw a tile whose visual is a picture, from the Es* list at "
                      "0x92028400. Empty means no scene, which keeps the stock card and captions. "
                      "EsAdvert draws an empty tile -- see the note above this cvar.");

// A single URL forced onto every picture tile, for testing where the limit is.
//
// Everything the shell's scenes draw comes from a package scheme -- common://,
// section://, controlpack://, sharedres:// -- while the only file:// loads in a
// whole run are made by code, not scenes (dash_2a8c for the disc cover,
// 0x922E7D00 for wallpapers). So a scene-side image element may simply not
// accept file://, which would explain a URL that loads when handed straight to
// dash_28b9 and is never requested by the tile.
//
// Setting this to a package URL the shell does not otherwise load answers it: if
// that URL then shows up in the theme trace, the tile does read shallowimg and
// the scheme is the problem.

// Which loader the fetch is routed to.
//
// 0x922D75E0 picks a handler from the request's mode bits, and the mode is
// sub_922D2D18(epix) OR'd with whatever is passed down from 0x922D48B8:
//
//     if ( (v8  & 4)    != 0 ) ... sub_922D6C48 ...   // a name inside homepage.xzp
//     if ( (v10 & 0x10) != 0 ) ... sub_922D6F10 ...   // NtOpenFile on a path
//     if ( (v10 & 1)    == 0 ) return v6;             // else give up
//     ... dash_2a5f, an HTTP download ...
//
// sub_922D2D18 only ever returns bit 4 (when the channel's +96 is set) or bits
// 0|1, so an image on a channel like ours is classed as something to download
// over HTTP. There is no Xbox LIVE to download it from, the request is queued
// and never completes, and the state at epix+48 stays at 1 -- below the 2 that
// 0x922D3AE8 requires before it will draw anything. That is the whole reason
// these tiles were blank.
//
// Bit 0x10 is never set by sub_922D2D18, so 0x922D6F10 is dead code on this
// path -- and it is exactly the loader wanted: it opens the path with NtOpenFile,
// reads it, and raises the state to 2 itself. 0x90 is that bit plus the 128 the
// caller passes normally.
REXCVAR_DEFINE_UINT32(channel_slot_load_flags, 0x90, "Dashboard",
                   "Mode bits for a tile image's fetch. 0x10 routes it to the file loader at "
                   "0x922D6F10, 4 to the homepage.xzp resource loader, 1 to an HTTP download.");


namespace {

// off_927F2778, the same table the parser resolves <format> against:
//
//     1 EpixScene   2 XZP    3 SCRIPTXZP   4 LUAXZP   5 SWF
//     6 IMG         7 VIDEO  8 BDE         9 PRIMETIME
//
// so a tile is not limited to a scene compiled into the package -- format 6
// draws a plain image file.
constexpr uint32_t kFormatEpixScene = 1;
constexpr uint32_t kFormatImg = 6;
constexpr uint32_t kButtonA = 0x5800;

// One tile.
struct SlotSpec {
  const char* name;
  // Either literal text, or a %EvResStr(IDS_...)% reference to one of the
  // shell's own strings. Guest 0x922CA4A8 decides which by the first character:
  //
  //     if ( a2 && *a2 != 37 ) return sub_922F18B8(a1, a2);   // 37 == '%'
  //     if ( sub_922CA3E0(a2, v5, 0x400, 0) < 0 )
  //         return sub_922F18B8(a1, a2);                      // macro failed
  //     return sub_922F1848(a1, v5);                          // resolved
  //
  // so text that does not start with '%' is taken verbatim, and a macro that
  // fails to resolve is displayed as written -- which is the only reason the
  // invented names below were visible on screen rather than silently blank.
  const char* description;
  // The smaller second line under the title, slot+24. Null leaves it unset,
  // which is what the Game Marketplace tiles do.
  const char* description2;
  const char* epix_id;
  const char* scene;        // an Es* scene compiled into the package
  // A .jpg/.png/.jpeg under the image root, or null to use `scene` instead.
  // When set the epix is built as format IMG rather than EpixScene, which is
  // what a <shallowimg> slot compiles down to -- see AddSlot.
  const char* image;
  // EcNavTo... command, or null for a tile that does nothing when pressed. A
  // slot with no onclick is legal -- the kiosk's own XBOX360 channel ships one.
  const char* cmd;
  const char* helptext;
  // <boxstyle>, slot+40. 0x922D5B70 copies it to the image epix's +28, and
  // 0x922D3A80 makes the scene name "<style>.xur" out of it, defaulting to
  // "picture" when it is null. homepage.xzp carries picture, nogradient, advert,
  // Transpic, Transnograd, GreenBox, OrangeBox, imgdest and romechan. Last in
  // the struct so the tables above need no edit.
  const char* style;
  // onclick+8, the command's first parameter. 0x922D30C8 loads param1..param6
  // from +8, +0xC, +0x10, +0x14, +0x18 and +0x1C before dispatching, and hands
  // param1 in r31 to whichever handler the jump table at 0x92028AD0 selects.
  // EcNavToPicture is the one that wants it: a .jpg/.png/.jpeg to show full
  // screen. Relative paths go through the same root as a tile picture.
  const char* param1;
};

// Game Marketplace. Every command and scene here is one the dashboard already
// uses elsewhere, so nothing depends on content that only exists online.
// The shell has exactly twenty-five resource names, listed at 0x920279B8 and
// installed into the lookup table at 0x927F25F0:
//
//     IDS_ADD_FRIEND    IDS_CHANNELNAME_WELCOME  IDS_CHANNELNAME_FRIENDS
//     IDS_CHANNELNAME_XBOX360  IDS_TELLMEMORE    IDS_INSIDEXBOX
//     IDS_GAMES         IDS_FRIENDS              IDS_VIDEO
//     IDS_PRIMETIME     IDS_PROMOTIONS           IDS_HDDVD
//     IDS_SOLUTIONS_DESC  IDS_SOLUTIONS          IDS_SETTINGS
//     IDS_MEDIACENTER_LINE2  IDS_MEDIACENTER     IDS_PICTURELIBRARY
//     IDS_MUSICLIBRARY  IDS_VIDEOLIBRARY         IDS_GAMESLIBRARY
//     IDS_GAMERCARD     IDS_DISKINTRAY           IDS_SELECTSLOT
//     IDS_SELECT
//
// A name outside that list resolves to nothing and the tile draws the macro
// text itself, so anything not on it has to be written as literal text, which
// the description field takes as-is.
//
// The Game Marketplace's own four navigation cards -- Games Library, What's
// New, Xbox Basics and Storage -- used to be defined here and led the row. They
// are gone: the channel shows the store now, and with 0x922D6018 refusing any
// slot past the 64th, each one they held was a game that could not be shelved.
// The kiosk art they drew (images/games/04.jpg and the downloads/ pair) is still
// in the game directory, and the scenes they opened -- EsGameLibrary,
// EsWhatsHot, EsXboxBasics, EsStorageUpsell -- are still compiled into the
// package, so this is a slot-table change and nothing more.

// "Welcome to Xbox 360", the kiosk's own second channel, from the same
// homepage.xml. Text and art are the disc's, in the order the kiosk showed
// them. No onclick: every one of these navigated with EcNavChannelAsRome into a
// further kiosk manifest (Xbox360EssentialsManifest.xml and friends) that this
// port has nothing to serve, so the tiles are display-only rather than pointed
// at a scene that does not exist.
const SlotSpec kWelcomeSlots[] = {
    {"Xbox 360 Highlights", "Xbox 360 Highlights", "Catch the most exciting features.",
     "SLOT_W_HIGHLIGHTS", nullptr, "images/welcome/00.jpg", nullptr, nullptr},
    {"Family Fun", "Family Fun", "Play smarter. Play safer.", "SLOT_W_FAMILYFUN", nullptr,
     "images/welcome/01.jpg", nullptr, nullptr},
    {"About Xbox LIVE", "About Xbox LIVE", "Learn how to connect.", "SLOT_W_ABOUTLIVE", nullptr,
     "images/welcome/02.jpg", nullptr, nullptr},
    {"More Than Games", "More Than Games", "The fun continues.", "SLOT_W_MORETHANGAMES", nullptr,
     "images/welcome/03.jpg", nullptr, nullptr},
    // The manifest writes this one "Hardware &amp; Accessories"; nothing here
    // goes through an XML parser, so it is the literal text that is wanted.
    {"Hardware and Accessories", "Hardware & Accessories", "The ultimate experience.",
     "SLOT_W_HARDWARE", nullptr, "images/welcome/04.jpg", nullptr, nullptr},
};



// COMMUNITY, from the definition the dash ships and cannot load.
//
// communitychannel.xml is in homepage.xzp beside welcome.xml and
// xbox360channel.xml, and emb_homepage.xml declares the channel for it. Its
// whole static content is one tile:
//
//     <slot>
//         <epixid>ADDFRIEND</epixid>
//         <reflection>no</reflection>
//         <boxstyle>transnograd</boxstyle>
//         <shallowimg>AddFriend.png</shallowimg>
//         <onclick><cmd>EcAddFriend</cmd>
//                  <helptext>%EvComResStr(IDS_ADD_FRIEND)%</helptext></onclick>
//     </slot>
//
// -- the rest of the row was filled in at runtime by livepack.xex through
// <epixapp>, which needs Xbox LIVE. So this is the channel as a disconnected
// console would have had it, and the tile is the dash's own art: AddFriend.png
// is a 420x320 PNG inside homepage.xzp, carved out to disk because the fetch
// this port can actually serve is the file loader.
//
// boxstyle transnograd is the manifest's own choice and resolves to
// Transnograd.xur, which is in that package too.
//
// The helptext macro is written %EvResStr% rather than the manifest's
// %EvComResStr%: IDS_ADD_FRIEND is one of the twenty-five names the shell
// installs into its own lookup table at 0x927F25F0, and a macro that does not
// resolve is drawn as its own text.
// It goes on the stock Friends channel rather than on a revived COMMUNITY tab.
// COMMUNITY draws as a second tab also called Friends -- its description is
// %EvResStr(IDS_CHANNELNAME_FRIENDS)% -- and showing it at all means clearing a
// condition the console meant to honour. Friends is already visible, already
// named right, and already the place this tile belongs.
// The style is the default rather than the manifest's transnograd.
//
// transnograd is "transparent, no gradient" -- Transnograd.xur draws the picture
// and nothing behind it, which is right on a channel where every tile is a
// floating cutout, and wrong next to the solid Meet and Greet card: the Add
// Friend avatar reads as an unbacked cutout dropped on the background. A null
// style falls through to picture.xur, the same card the Welcome and Game
// Marketplace tiles use, and AddFriend.png being transparent means the card
// shows through behind the silhouette exactly as it should.
// The text uses %EvComResStr%, which is what communitychannel.xml writes.
//
// This was changed to %EvResStr% on the reasoning that IDS_ADD_FRIEND is one of
// the twenty-five names the shell installs at 0x927F25F0. It is, and it still
// resolved wrong: the tile came up labelled "Welcome", which is
// IDS_CHANNELNAME_WELCOME, the entry next to it in that table. The two macros
// read different resource tables and only the common one has the string this
// name means. The manifest had it right.
const SlotSpec kAddFriendSlots[] = {
    {"Add Friend", "%EvComResStr(IDS_ADD_FRIEND)%", nullptr, "SLOT_F_ADDFRIEND", nullptr,
     "images/friends/AddFriend.png", "EcAddFriend", "%EvComResStr(IDS_ADD_FRIEND)%", nullptr},
};

// Spotlight, on the channel the 9199 dash calls Inside Xbox.
//
// The dash has no Spotlight channel of its own. A sweep of the whole dump finds
// the name exactly twice: EsSpotlight, a scene identifier in the table at
// 0x92028528 that nothing in any package resolves to, and IDS_SPOTLIGHT in
// mediasite.xzp, which is the Live marketplace app's string table rather than
// the shell's. Like Inside Xbox, Spotlight was a channel Xbox LIVE filled in;
// the dash ships the renderer and the identifier and none of the content.
//
// The Experience Disc does have one, though, and a real one: disc 5.4's
// homepage.xml declares a SPOTLIGHT channel with these seven slots, their text
// and their art, all authored by Microsoft. The descriptions below are that
// document's, verbatim.
//
// Alan Wake carries no description on purpose -- the manifest writes
// <description /> empty, because that tile's art already has the title on it.
//
// The onclicks are not the manifest's, because none of the manifest's work here.
//
// The dispatcher is 0x922D30C8: it resolves the cmd string against the 37-entry
// table at 0x920288A0 and jumps through the offsets at 0x92028AD0. Reading that
// table settles what is available:
//
//   EcNavChannelAsRome (0x1C)  jumps straight to the failure return -- it is
//                              unimplemented in this build, so the Gold Pack
//                              tile could never have worked
//   EcNavToEpixManifest (0xD)  dash_2a6f, which needs the epix manifest system
//                              to fetch games.xml; that is the same subsystem
//                              that fails on communitychannel.xml, and it is
//                              gated behind loading homepage.xml through a path
//                              this port does not serve
//   EcNavToLiveUpsell (0x16)   loads homepage.xzp#LiveUpsellRootScene.xur, which
//                              comes straight back out with no Live to sell
//
// So the tiles point at destinations that do exist. EcNavToPicture (0x19) is the
// kiosk's own idea -- disc 6.0 uses it with a full-screen JPEG -- and takes its
// picture in param1, validated against the same .jpg/.png/.jpeg list as a tile
// image and fetched the same way.
const SlotSpec kSpotlightSlots[] = {
    {"Featured Games", "Featured Games", "Play the Demos Now!", "SLOT_S_FEATURED", nullptr,
     "images/spotlight/featured.jpg", "EcNavToGamesLibrary", "%EvResStr(IDS_SELECTSLOT)%", nullptr,
     nullptr},
    {"Only on Xbox 360", "Only on Xbox 360", "Check out the exclusive fun", "SLOT_S_ONLYON",
     nullptr, "images/spotlight/onlyon.jpg", "EcNavToLiveUpsell", "%EvResStr(IDS_TELLMEMORE)%",
     nullptr},
    {"Project Natal", "\"Project Natal\"", "Watch the Video.", "SLOT_S_NATAL", nullptr,
     "images/spotlight/natal.jpg", "EcPlayMigrationVideo", "%EvResStr(IDS_SELECTSLOT)%", nullptr,
     nullptr},
    {"Alan Wake", "", nullptr, "SLOT_S_ALANWAKE", nullptr, "images/spotlight/alanwake.jpg",
     "EcNavToLiveUpsell", "%EvResStr(IDS_TELLMEMORE)%", nullptr},
    {"Start Your Engines", "Start Your Engines", "Discover Great Racing Action!", "SLOT_S_RACING",
     nullptr, "images/spotlight/racing.jpg", "EcNavToLiveUpsell", "%EvResStr(IDS_TELLMEMORE)%",
     nullptr},
    {"Xbox 360 Elite", "Xbox 360 Elite", "Total Entertainment Value, in One Box.", "SLOT_S_ELITE",
     nullptr, "images/spotlight/elite.jpg", "EcNavToPicture", "%EvResStr(IDS_SELECTSLOT)%", nullptr,
     "images/spotlight/elite_full.jpg"},
    {"Gold Pack", "12 Month Messenger Gold Pack", "The best of Xbox LIVE at the best value.",
     "SLOT_S_GOLDPACK", nullptr, "images/spotlight/goldpack.jpg", "EcNavToLiveUpsell",
     "%EvResStr(IDS_TELLMEMORE)%", nullptr},
};

// One line of a list file, kept alive for the run.
//
// SlotSpec holds raw pointers, so whatever it points at has to outlive the slot
// it builds. These strings are owned here and the specs borrow them, which is
// why each list is a function-local static rather than anything built per call.
//
// Friends and the marketplace are the same fields with different meanings:
//
//   friends.txt      gamertag | presence         | image | xuid | online
//   marketplace.txt  name     | genre and year   | image | title id | kind
//                    | developer | publisher | released | rating | description
//
// so one parser serves both, and either file can be written by hand. The fifth
// field is optional, and what it means depends on the file: "category" marks a
// marketplace row as a heading for the games that follow rather than a game of
// its own, and "online" marks a friend who was online when the list was written.
//
// Both live in `kind` because it is the only spare field a friend can safely
// use. The ones after it belong to the marketplace detail page, and putting
// anything in `developer` would make has_details() true, giving every friend a
// detail page built out of nothing.
struct ListEntry {
  std::string title;    // the big line
  std::string subtitle; // the small line under it
  std::string image;
  std::string extra;    // xuid for a friend, title id for a game
  std::string kind;     // empty, or "category" for a marketplace heading
  // What a detail page is built from. Only the marketplace fills these in; a
  // hand-written list can stop after the fifth field and simply not have one.
  std::string developer;
  std::string publisher;
  std::string released;
  std::string rating;
  std::string description;
  std::string epix_id;

  bool is_category() const { return kind == "category"; }
  bool has_details() const {
    return !(developer.empty() && publisher.empty() && released.empty() &&
             rating.empty() && description.empty());
  }
};

// Blank lines and lines starting with # are skipped, and fields may be omitted
// from the right. Only the first field is required.
void ParseListFile(const std::string& setting, const char* id_prefix, const char* what,
                   std::vector<ListEntry>& out) {
  if (setting.empty()) return;
  const std::filesystem::path path = nxe_paths::Resolve(setting);
  std::ifstream f(path);
  if (!f) {
    REXKRNL_INFO("[slots] no {} at {}", what, path.string());
    return;
  }

  std::string line;
  while (std::getline(f, line)) {
    // Tolerate CRLF, since these files are meant to be edited by hand.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') continue;

    std::string field[10];
    size_t at = 0;
    for (int i = 0; i < 10 && at <= line.size(); ++i) {
      const size_t bar = line.find('|', at);
      field[i] = line.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
      const size_t b = field[i].find_first_not_of(" \t");
      const size_t e = field[i].find_last_not_of(" \t");
      field[i] = (b == std::string::npos) ? std::string() : field[i].substr(b, e - b + 1);
      if (bar == std::string::npos) break;
      at = bar + 1;
    }
    if (field[0].empty()) continue;

    ListEntry entry;
    entry.title = field[0];
    entry.subtitle = field[1];
    entry.image = field[2];
    entry.extra = field[3];
    entry.kind = field[4];
    entry.developer = field[5];
    entry.publisher = field[6];
    entry.released = field[7];
    entry.rating = field[8];
    entry.description = field[9];
    // Unique per channel: 0x922D55B0 rejects duplicate epix ids on one channel.
    entry.epix_id = id_prefix + std::to_string(out.size());
    out.push_back(std::move(entry));
    // Well past what one channel can hold, on purpose. 0x922D6018 refuses any
    // slot past the 64th, but the marketplace file is no longer one channel's
    // worth: it feeds the row its categories and each category page its own
    // games, so the 64 applies per channel and not to the file. Every AddSlot
    // is checked, so hitting the ceiling anywhere shows up in the log rather
    // than silently dropping tiles.
    if (out.size() >= 1024) break;
  }
  REXKRNL_WARN("[slots] {}: {} entr(ies) from {}", what, out.size(), path.string());
}

const std::vector<ListEntry>& Friends() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(friends_list), "SLOT_FRIEND_", "friends list", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& MessagesList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(messages_list), "SLOT_MSG_", "messages", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& ProfileList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(profile_list), "SLOT_PROF_", "profile", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& GamerpicsList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(gamerpics_list), "SLOT_GPIC_", "gamer pictures", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& RecentPlayers() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(recent_list), "SLOT_RECENT_", "recent players", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& FollowingList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(following_list), "SLOT_FOLLOWING_", "following", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& FollowersList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(followers_list), "SLOT_FOLLOWERS_", "followers", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& PeopleList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(people_list), "SLOT_PERSON_", "people", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& TitlesList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(titles_list), "SLOT_TITLE_", "my games", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& GameStatsList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(gamestats_list), "SLOT_GSTAT_", "game stats", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& AchievementsList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(achievements_list), "SLOT_ACH_", "achievements", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& VideoList() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(video_list), "SLOT_VID_", "video marketplace", s_list);
  }
  return s_list;
}

const std::vector<ListEntry>& Marketplace() {
  static std::vector<ListEntry> s_list;
  static bool s_loaded = false;
  if (!s_loaded) {
    s_loaded = true;
    ParseListFile(REXCVAR_GET(marketplace_list), "SLOT_MKT_", "marketplace", s_list);
  }
  return s_list;
}

uint32_t Be32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreBe32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

using GuestFn = void (*)(PPCContext& __restrict, uint8_t*);

// Call a guest function and put the context back as it was. The callee runs on
// the guest stack this hook was entered with and balances it; restoring the
// saved context afterwards also restores r1, so consecutive calls do not drift.
uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFn fn, uint32_t a, uint32_t b = 0) {
  PPCContext saved = ctx;
  ctx.r3.u32 = a;
  ctx.r4.u32 = b;
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

uint32_t Call5(PPCContext& ctx, uint8_t* base, GuestFn fn, uint32_t a, uint32_t b, uint32_t c,
               uint32_t d = 0, uint32_t e = 0) {
  PPCContext saved = ctx;
  ctx.r3.u32 = a;
  ctx.r4.u32 = b;
  ctx.r5.u32 = c;
  ctx.r6.u32 = d;
  ctx.r7.u32 = e;
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

// The string setters take a guest pointer, so the text has to live in guest
// memory. They copy it, but the block is small and allocated once per boot.
uint32_t GuestString(PPCContext& ctx, uint8_t* base, const char* s) {
  const uint32_t n = uint32_t(std::strlen(s)) + 1;
  const uint32_t p = Call(ctx, base, __imp__sub_92144098, n);
  if (p) std::memcpy(base + p, s, n);
  return p;
}

void SetString(PPCContext& ctx, uint8_t* base, GuestFn setter, uint32_t field, const char* text) {
  const uint32_t s = GuestString(ctx, base, text);
  if (s) Call(ctx, base, setter, field, s);
}

// Read once: the tiles are built in a single pass at channeldef close, and a
// prefix that changed underneath them would leave the row half-rebased.
const std::string& ImageRoot() {
  static const std::string s_root = REXCVAR_GET(channel_slot_image_root);
  return s_root;
}

// "images/welcome/00.jpg" -> "file://media:<sep>images<sep>welcome<sep>00.jpg".
// The scheme's own "//" is inside ImageRoot and is not touched.
std::string JoinUrl(const char* rel, char sep) {
  std::string out = ImageRoot();
  out.push_back(sep);
  for (const char* p = rel; *p; ++p) {
    out.push_back(*p == '/' ? sep : *p);
  }
  return out;
}

// Ask the shell's own image loader whether a URL resolves.
//
// Guest 0x921C2850 is dash_28b9 -- r3 a UTF-16 URL, r4 an out handle, HRESULT
// back in r3 -- the same call every [theme] image line in the log comes from.
// __imp__ rather than the plain name so this goes straight to the guest body and
// not back through theme_trace.cpp's hook, which rewrites disc artwork.
//
// Both separator styles are attested in a single boot and neither is obviously
// the rule: 'file://theme:\WallPaper1' and 'file://media:/disc.png' both return
// 0x0, and both of those are files at a device root. Nothing in the log settles
// what a subdirectory wants, and a tile only asks for its picture once the tab
// is focused -- so rather than guess and wait to be told, this asks.
bool ImageLoads(PPCContext& ctx, uint8_t* base, const std::string& url) {
  const uint32_t chars = uint32_t(url.size()) + 1;
  const uint32_t buf = Call(ctx, base, __imp__sub_92144098, chars * 2);
  const uint32_t out = Call(ctx, base, __imp__sub_92144098, 4);
  if (!buf || !out) return false;
  auto* text = reinterpret_cast<be<uint16_t>*>(base + buf);
  for (size_t i = 0; i < url.size(); ++i) {
    text[i] = uint16_t(uint8_t(url[i]));
  }
  text[url.size()] = 0;
  StoreBe32(base, out, 0);
  return int32_t(Call(ctx, base, __imp__sub_921C2850, buf, out)) >= 0;
}

// Settled once, on the first tile, and reused for the rest.
char ImageSeparator(PPCContext& ctx, uint8_t* base, const char* probe_rel) {
  static char s_sep = 0;
  if (s_sep) return s_sep;
  s_sep = '\\';
  // A root with no scheme is a device path for NtOpenFile, not a URL, and
  // dash_28b9 would refuse it. Backslashes, and nothing to probe.
  if (ImageRoot().find("://") == std::string::npos) return s_sep;
  if (ImageLoads(ctx, base, JoinUrl(probe_rel, '\\'))) {
    REXKRNL_WARN("[slots] image URLs resolve with '\\': {}", JoinUrl(probe_rel, '\\'));
  } else if (ImageLoads(ctx, base, JoinUrl(probe_rel, '/'))) {
    s_sep = '/';
    REXKRNL_WARN("[slots] '\\' was rejected; image URLs resolve with '/': {}",
                 JoinUrl(probe_rel, '/'));
  } else {
    REXKRNL_WARN("[slots] neither '{}' nor '{}' loads -- tiles will draw blank; check that the "
                 "files are under the game directory and that channel_slot_image_root names the "
                 "right device",
                 JoinUrl(probe_rel, '\\'), JoinUrl(probe_rel, '/'));
  }
  return s_sep;
}

// A NUL-terminated UTF-16BE guest string, for reading a field back after it has
// been written. Non-ASCII shows as '?' -- this is for checking that a string
// round-tripped, not for display.
// Write text as the UTF-16 the scene loader reads, NUL-terminated, and report
// where the next string may start. ASCII only, which every scene name is.
uint32_t StoreWide(uint8_t* base, uint32_t at, const std::string& text);

// One guest block holding a package name and a scene name back to back, for the
// callers that name a scene rather than pointing at a string in the image.
uint32_t SceneNameBlock(PPCContext& ctx, uint8_t* base, const std::string& package,
                        const std::string& scene) {
  static uint32_t s_block = 0;
  if (!s_block) s_block = Call(ctx, base, __imp__sub_92144098, 256);
  if (!s_block || (package.size() + scene.size() + 2) * 2 > 256) return 0;
  StoreWide(base, StoreWide(base, s_block, package), scene);
  return s_block;
}

uint32_t StoreWide(uint8_t* base, uint32_t at, const std::string& text) {
  uint32_t p = at;
  for (const char c : text) {
    base[p++] = 0;
    base[p++] = uint8_t(c);
  }
  base[p++] = 0;
  base[p++] = 0;
  return p;
}

std::string WideAt(uint8_t* base, uint32_t str) {
  if (!str) return "(null)";
  std::string out;
  for (int i = 0; i < 128; ++i) {
    const uint16_t ch = (uint16_t(base[str + i * 2]) << 8) | base[str + i * 2 + 1];
    if (!ch) break;
    out.push_back(ch < 0x80 ? char(ch) : '?');
  }
  return out;
}

// The parser keeps the current element name at +1052 and its text at +1084, both
// as plain bytes rather than wide characters.
std::string Ansi(uint8_t* base, uint32_t addr, size_t limit) {
  std::string out;
  for (size_t i = 0; i < limit; ++i) {
    const char c = static_cast<char>(base[addr + i]);
    if (!c) break;
    out.push_back(c >= 0x20 && c < 0x7f ? c : '?');
  }
  return out;
}

std::string ChannelId(uint8_t* base, uint32_t channel) {
  const uint32_t str = Be32(base, channel + 24);
  if (!str) return {};
  std::string out;
  for (int i = 0; i < 64; ++i) {
    const uint16_t ch = (uint16_t(base[str + i * 2]) << 8) | base[str + i * 2 + 1];
    if (!ch) break;
    out.push_back(ch < 0x80 ? char(ch) : '?');
  }
  return out;
}

// A whole new tab, built the way the manifest parser builds one.
//
// 0x922DBB88 is the container-level state machine; 0x922DAEF0 only ever fills in
// a channel it was handed. The creation itself is three steps there:
//
//     v22 = sub_922CEB00(*(a1 + 5236));      // container; 100 bytes, capped at 32
//     *(a1 + 5240) = v22;                    // becomes "the channel being parsed"
//     ...
//     v10 = *(a1 + 5240) + 24;               // <id>
//     v10 = *(a1 + 5240) + 32;               // <definitionpath>
//     sub_922F18B8(v10, a1 + 1084);
//
// and 0x922DAEF0 finishes it at </channeldef> by writing 1 to +16, which is what
// marks the definition complete -- a channel without it is carried in the list
// but never treated as defined.
//
// The container is not passed to the channeldef handler, but every channel keeps
// a pointer back to it: 0x922CEB00 writes v2[5] = a1, so channel+20 is the
// container that made it. That is where this gets it from.
//
// Removal is by pointer -- 0x922CF148(container, channel) unlinks that exact
// channel -- so a tab appended here is not at risk of being taken away in place
// of the one a later </channel> means to discard.
// Put a channel at the front of the container's list.
//
// The tabs are drawn bottom-up: the list order Games, WelcomeToXbox360, Video,
// Friends, Inside Xbox comes out on screen as Inside Xbox at the top and Game
// Marketplace at the bottom. So a tab that should sit *below* Game Marketplace
// has to be first in the list, and 0x922CEB00 only ever appends -- it links the
// new channel in at the container's tail (+76) with its next pointing back at
// the sentinel.
//
// The list is circular and doubly linked through the node at channel+4, next at
// +0 and prev at +4, exactly as channel_trace.cpp walks it. The container's own
// +72/+76 pair is the sentinel node, so a neighbour being the container rather
// than a channel needs no special case: writing "prev's next" lands on +72 and
// "next's prev" lands on +76, which is what those fields already mean.
void MoveChannelToFront(uint8_t* base, uint32_t container, uint32_t channel) {
  const uint32_t node = channel + 4;
  const uint32_t sentinel = container + 72;

  const uint32_t next = Be32(base, node);
  const uint32_t prev = Be32(base, node + 4);
  if (!next || !prev) return;
  if (Be32(base, sentinel) == node) return;  // already first

  StoreBe32(base, prev, next);
  StoreBe32(base, next + 4, prev);

  const uint32_t first = Be32(base, sentinel);
  StoreBe32(base, node, first);
  StoreBe32(base, node + 4, sentinel);
  StoreBe32(base, sentinel, node);
  StoreBe32(base, first + 4, node);
}

uint32_t AddChannel(PPCContext& ctx, uint8_t* base, uint32_t container, const char* id,
                    const char* description) {
  const uint32_t channel = Call(ctx, base, __imp__sub_922CEB00, container);
  if (!channel) return 0;
  SetString(ctx, base, __imp__sub_922F18B8, channel + 24, id);
  SetString(ctx, base, __imp__sub_922CA4A8, channel + 28, description);
  StoreBe32(base, channel + 16, 1);
  return channel;
}

// Hide the single upsell card an offline channel ships with.
//
// The parser writes 0 to a slot's +104 for <visible>no</visible>, so the same
// field hides it -- no list surgery, and the slot stays allocated and linked
// exactly as it was. 0x922D6380 appends, so the stock slot is always the head
// of the list and anything added here comes after it.
//
// Only touch it if it currently reads as visible. If the default turns out not
// to be 1 then +104 does not mean what this assumes, and blanking it would risk
// hiding everything; in that case leave it alone and say so.
void HideStockUpsell(uint8_t* base, uint32_t channel, const std::string& id) {
  const uint32_t head = Be32(base, channel + 48);
  if (!head) return;
  const uint32_t upsell = head - 4;
  const uint32_t visible = Be32(base, upsell + 104);
  if (visible == 1) {
    StoreBe32(base, upsell + 104, 0);
    REXKRNL_WARN("[slots] '{}': hid the stock upsell slot at {:#x}", id, upsell);
  } else {
    REXKRNL_WARN("[slots] '{}': upsell slot at {:#x} has visible={:#x}, not the expected 1; "
                 "leaving it",
                 id, upsell, visible);
  }
}

bool AddSlot(PPCContext& ctx, uint8_t* base, uint32_t channel, const SlotSpec& spec) {
  // A picture tile and a scene tile are bound through different fields, and the
  // difference decides whether anything is drawn at all.
  //
  // 0x922D5B70 prepares each slot and sorts what it finds by where it came from:
  //
  //     if ( !a1[19] ) {                                  // +76, no scene bound
  //       v2 = a1[17];                                    // +68, shallowimg
  //       if ( sub_922D9F40(v2, L".jpg", L".png", L".jpeg") )
  //         a1[18] = sub_922D4408(a1[4]);                 // -> slot+72
  //       *(v3 + 12) = 6;                                 // format IMG
  //       ...
  //     }
  //     v6 = sub_922D55B0(a1[4], a1[15]);                 // +60, epixid
  //     if ( *(v6 + 12) == 1 ) a1[19] = v6;               // scene -> slot+76
  //     else                   a1[16] = v6;               // else  -> slot+64
  //
  // so a picture reaches the tile at +72 and a scene at +76. +64 is where a
  // named epix that is neither ends up, and nothing draws it -- which is what
  // building the IMG epix here with an epixid of its own did: the trace showed
  // image=<ptr> scene=0x0 on every tile and every one of them still came out
  // blank. The guard above is "no scene bound", not "no epixid", so naming the
  // epix bought nothing and cost the only field that is read.
  //
  // A picture tile therefore just sets +68 and lets the guest build its own
  // epix, exactly as the kiosk's <shallowimg> slots do.
  // A picture tile still needs a scene to draw it, so both are set: the scene
  // through the epixid, the picture through shallowimg.
  // A spec that names its own scene keeps it; the cvar only fills in for a tile
  // that has none. That leaves the Game Marketplace row on the dashboard's own
  // scenes and the Welcome row on the picture scene, which is the comparison
  // that says whether scene binding reaches the renderer at all: if the Games
  // tiles now draw their stock art (EsXboxBasics fetches common://XboxBasicsSlot.jpg)
  // then scenes work and only the picture source is unresolved, and if they are
  // blank too then nothing bound here is reaching the draw path.
  static const std::string s_image_scene = REXCVAR_GET(channel_slot_image_scene);
  const char* scene = spec.scene;
  if (!scene && spec.image && !s_image_scene.empty()) scene = s_image_scene.c_str();

  uint32_t epix = 0;
  if (scene) {
    epix = Call(ctx, base, __imp__sub_922D4408, channel);
    if (!epix) return false;
    StoreBe32(base, epix + 12, kFormatEpixScene);
    SetString(ctx, base, __imp__sub_922F18B8, epix + 16, spec.epix_id);
    SetString(ctx, base, __imp__sub_922F18B8, epix + 20, scene);
  }

  const uint32_t slot = Call(ctx, base, __imp__sub_922D6380, channel);
  if (!slot) return false;
  SetString(ctx, base, __imp__sub_922CA4A8, slot + 20, spec.description);
  if (spec.description2) {
    SetString(ctx, base, __imp__sub_922CA4A8, slot + 24, spec.description2);
  }
  SetString(ctx, base, __imp__sub_922F18B8, slot + 32, spec.name);
  // The two fields the parser writes, and the only two that matter here.
  // 0x922DAEF0 is explicit about which is which:
  //
  //     if ( sub_9214C3B0(a1 + 1052, "shallowimg") ) {      // not shallowimg
  //       if ( sub_9214C3B0(a1 + 1052, "epixid") ) { ... }  // not epixid
  //       v18 = *(_DWORD *)(a1 + 5212) + 60;                // epixid     -> +60
  //     } else {
  //       v18 = *(_DWORD *)(a1 + 5212) + 68;                // shallowimg -> +68
  //     }
  //     sub_922F18B8(v18, a1 + 1084);
  //
  // Originally the epix id went to +68, landing it in the shallowimg field,
  // where it named a file that did not exist and bound nothing.
  // <boxstyle>. The guest carries it from here to the image epix's +28 and turns
  // it into the scene name, so setting it is all that is needed to pick a
  // different .xur out of homepage.xzp.
  if (spec.style) {
    SetString(ctx, base, __imp__sub_922F18B8, slot + 40, spec.style);
  }
  if (spec.image) {
    const std::string url = JoinUrl(spec.image, ImageSeparator(ctx, base, spec.image));
    SetString(ctx, base, __imp__sub_922F18B8, slot + 68, url.c_str());
  }
  if (scene) {
    SetString(ctx, base, __imp__sub_922F18B8, slot + 60, spec.epix_id);
  }
  // 0x922D55B0 is what the shell itself uses to turn a slot's epixid into an
  // epix -- a wcscmp down the channel's epix list -- so putting the slot's own
  // id through it says outright whether the two strings match.
  const uint32_t resolved =
      scene ? Call(ctx, base, __imp__sub_922D55B0, channel, Be32(base, slot + 60)) : 0;
  REXKRNL_WARN("[slots]   '{}' scene='{}' shallowimg='{}' epixid='{}' -> epix {:#x}{}", spec.name,
               scene ? scene : "(none)", WideAt(base, Be32(base, slot + 68)),
               WideAt(base, Be32(base, slot + 60)), resolved,
               (!scene || resolved == epix) ? "" : "  *** NOT THE EPIX JUST BUILT ***");
  // Read the captions back out of guest memory too. If a character is on screen
  // as a gap it is either absent from the string or absent from the font, and
  // these two lines are what tells those apart.
  REXKRNL_WARN("[slots]     slot {:#x} desc='{}' desc2='{}'", slot,
               WideAt(base, Be32(base, slot + 20)), WideAt(base, Be32(base, slot + 24)));

  if (!spec.cmd) return true;

  const uint32_t onclick_mem = Call(ctx, base, __imp__sub_92144098, 48);
  if (onclick_mem) {
    const uint32_t onclick = Call(ctx, base, __imp__sub_922CCD88, onclick_mem);
    if (onclick) {
      SetString(ctx, base, __imp__sub_922F18B8, onclick + 0, "EpixCmd");
      SetString(ctx, base, __imp__sub_922F18B8, onclick + 4, spec.cmd);
      SetString(ctx, base, __imp__sub_922CA4A8, onclick + 36, spec.helptext);
      if (spec.param1) {
        // A picture path gets the image root in front of it; anything else --
        // EcShowGamerCard wants a xuid -- is passed exactly as written. The
        // extension is the same test 0x922D9F40 uses to decide what is an image.
        const std::string p = spec.param1;
        const bool is_image = p.size() > 4 && (p.rfind(".jpg") == p.size() - 4 ||
                                               p.rfind(".png") == p.size() - 4 ||
                                               p.rfind(".jpeg") == p.size() - 5);
        // Held in a named string: the ternary's temporary would be gone before
        // SetString ever read it.
        const std::string value =
            is_image ? JoinUrl(spec.param1, ImageSeparator(ctx, base, spec.param1)) : p;
        SetString(ctx, base, __imp__sub_922F18B8, onclick + 8, value.c_str());
      }
      StoreBe32(base, onclick + 40, kButtonA);
      StoreBe32(base, slot + 80, onclick);  // first of the four onclick pointers
    }
  }
  return true;
}

// The marketplace regrouped the way the drill-down needs it: the categories in
// row order, each holding the games filed under it. The pointers are into the
// Marketplace() static, which is built once and never changes after.
struct MarketCategory {
  std::string name;
  std::string subtitle;
  std::string image;  // the category's own card, a grid of the covers under it
  std::vector<const ListEntry*> games;
};

std::vector<MarketCategory> GroupByCategory(const std::vector<ListEntry>& list) {
  std::vector<MarketCategory> cats;
  for (const auto& entry : list) {
    if (entry.is_category()) {
      cats.push_back(MarketCategory{entry.title, entry.subtitle, entry.image, {}});
    } else if (!cats.empty()) {
      cats.back().games.push_back(&entry);
    }
  }
  return cats;
}

const std::vector<MarketCategory>& Categories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(Marketplace());
  return s_cats;
}

const std::vector<MarketCategory>& MessageCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(MessagesList());
  return s_cats;
}

const std::vector<MarketCategory>& ProfileCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(ProfileList());
  return s_cats;
}

const std::vector<MarketCategory>& GamerpicCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(GamerpicsList());
  return s_cats;
}

const std::vector<MarketCategory>& RecentCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(RecentPlayers());
  return s_cats;
}

const std::vector<MarketCategory>& FollowingCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(FollowingList());
  return s_cats;
}

const std::vector<MarketCategory>& FollowersCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(FollowersList());
  return s_cats;
}

const std::vector<MarketCategory>& PeopleCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(PeopleList());
  return s_cats;
}

const std::vector<MarketCategory>& TitleCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(TitlesList());
  return s_cats;
}

const std::vector<MarketCategory>& GameStatsCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(GameStatsList());
  return s_cats;
}

const std::vector<MarketCategory>& AchievementCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(AchievementsList());
  return s_cats;
}

const std::vector<MarketCategory>& VideoCategories() {
  static const std::vector<MarketCategory> s_cats = GroupByCategory(VideoList());
  return s_cats;
}

// What a category tile passes as param1.
//
// Both manifest commands take a URL, hand it to the manager, and let it find or
// build a container for it. Nothing parses the string beyond that, so a scheme
// of our own is enough to name a container -- one per category, each holding a
// single channel of that category's games.
constexpr const char* kCategoryUrl = "nxemkt:";

// "nxemkt:<marketplace>:<category>". The marketplace has to be in there: both
// rows have a Family, a Music and a Fantasy, and keying on the genre alone would
// have the Game Marketplace's Family open the video one or the other way about,
// depending on which was built first.
std::string CategoryKey(const std::string& kind, const std::string& name) {
  return kind + ":" + name;
}

std::string CategoryUrl(const std::string& kind, const std::string& name) {
  return kCategoryUrl + CategoryKey(kind, name);
}

// One guest scratch block: a dword for the container the manager hands back,
// then the seven-dword parameter block 0x922C5580 reads.
uint32_t CategoryScratch(PPCContext& ctx, uint8_t* base) {
  static uint32_t s_block = 0;
  if (!s_block) s_block = Call(ctx, base, __imp__sub_92144098, 64);
  return s_block;
}

uint32_t FillCategoryChannel(PPCContext& ctx, uint8_t* base, uint32_t container,
                             const std::string& kind, const MarketCategory& cat) {
  const uint32_t channel = AddChannel(ctx, base, container, cat.name.c_str(), cat.name.c_str());
  if (!channel) return 0;

  // channel+96 is deliberately left clear, which is the opposite of what it
  // looks like it should be.
  //
  // 0x922D2D18 turns that flag into mode bit 4, and 0x922D75E0 tries its
  // branches in order: bit 4 looks the picture up inside a package first, bit
  // 0x10 reads it off disk, bit 1 downloads it. With bit 4 set the package
  // lookup answered for these tiles and returned before the file read ever ran,
  // so the picture stayed at state 1 and the tile drew as text on a blank card.
  // Leaving the flag clear drops bit 4, and the read -- which is synchronous --
  // completes on the spot.

  int added = 0;
  for (const ListEntry* game : cat.games) {
    SlotSpec spec{};
    spec.name = game->title.c_str();
    spec.description = game->title.c_str();
    spec.description2 = game->subtitle.empty() ? nullptr : game->subtitle.c_str();
    spec.epix_id = game->epix_id.c_str();
    spec.image = game->image.empty() ? nullptr : game->image.c_str();
    const std::string style = REXCVAR_GET(channel_marketplace_tile_style);
    if (!style.empty()) spec.style = style.c_str();

    // What the tile does when it is chosen.
    //
    // Most are a title and open its page. Two kinds act instead: a gamerpic
    // sets the profile's picture, and a link opens another page of ours by
    // "<kind>:<name>" -- which is how Profile reaches Change Gamer Picture.
    std::string durl;
    if (game->kind == "gamerpic" && !game->extra.empty()) {
      // "setpic:", not "gamerpic:" -- see the handler in sub_922C5BF8.
      durl = kCategoryUrl + std::string("setpic:") + game->extra;
      spec.cmd = "EcNavToLocalEpixManifest";
      spec.param1 = durl.c_str();
    } else if (game->kind == "link" && !game->extra.empty()) {
      durl = kCategoryUrl + game->extra;
      spec.cmd = "EcNavToLocalEpixManifest";
      spec.param1 = durl.c_str();
    } else if (kind == "mygames" && nxe_channels::HasCategoryPage("game", game->title)) {
      // A game on the My Games page opens its own detail: achievements,
      // gamerscore and time played.
      durl = CategoryUrl("game", game->title);
      spec.cmd = "EcNavToLocalEpixManifest";
      spec.param1 = durl.c_str();
    } else if ((kind == "following" || kind == "followers" || kind == "friends") &&
               nxe_channels::HasCategoryPage("person", game->title)) {
      // Somebody on one of the people pages opens their own page, the same as a
      // friend tile on the channel. Restricted to those three kinds rather than
      // asked for every tile: the key is a gamertag, and a game whose name
      // happened to match one would otherwise open a stranger's page.
      durl = CategoryUrl("person", game->title);
      spec.cmd = "EcNavToLocalEpixManifest";
      spec.param1 = durl.c_str();
    } else if (game->has_details()) {
      durl = kCategoryUrl + CategoryKey(kind + ":title", game->extra);
      spec.cmd = "EcNavToLocalEpixManifest";
      spec.param1 = durl.c_str();
    } else {
      spec.cmd = "EcNavToGamesLibrary";
    }
    spec.helptext = "%EvResStr(IDS_SELECTSLOT)%";
    // An entry with no picture is skipped, and that is load-bearing.
    //
    // It looks like over-caution and is not: a pictureless tile on one of these
    // pages takes the dashboard down when the page is opened. Letting the
    // Messages page through with one crashed on open every time --
    //
    //     opening 'messages:Messages'
    //     0xC0000005, read of 0x100000350
    //     sub_92439D58 <- sub_92428920 <- sub_92428ED8
    //
    // -- and giving that same message a picture fixed it outright. These pages
    // are built and prepared by hand rather than by the shell, and something on
    // the draw path expects the image epix to exist. So every entry that reaches
    // here needs one; the fetch tools are what guarantee it.
    if (!spec.image) continue;
    if (AddSlot(ctx, base, channel, spec)) ++added;
  }
  REXKRNL_WARN("[slots] category page '{}': channel {:#x}, {} of {} entr(ies)", cat.name, channel,
               added, cat.games.size());
  return channel;
}

void PrebuildCategoryPages(PPCContext& ctx, uint8_t* base, const std::string& kind,
                           const std::vector<MarketCategory>& cats);  // below

// A row of categories, with or without their contents laid out after them.
//
// Both marketplaces are the same shape -- a genre, then the things filed under
// it -- so they share this. Categories alone is what the storefront showed; the
// inline form is the fallback while opening a category does not work.
//
// Every AddSlot is checked because 0x922D6018 returns null rather than failing
// once a channel holds 64 slots, so a row built past that would quietly lose its
// tail. Hitting the ceiling stops the row and is visible in the count logged.
int BuildCategoryRow(PPCContext& ctx, uint8_t* base, uint32_t channel, const std::string& id,
                     const std::string& kind, const std::vector<MarketCategory>& cats,
                     bool categories_only, int per_row, bool open_pages) {
  int added_cats = 0, added_items = 0;
  for (const auto& cat : cats) {
    const std::string url = CategoryUrl(kind, cat.name);
    SlotSpec spec{};
    spec.name = cat.name.c_str();
    spec.description = cat.name.c_str();
    spec.description2 = cat.subtitle.empty() ? nullptr : cat.subtitle.c_str();
    spec.epix_id = cat.name.c_str();
    spec.helptext = "%EvResStr(IDS_SELECTSLOT)%";
    // The category's own card: a grid of the covers filed under it, built by the
    // fetch tools. Without one the slot falls back to drawing its own blank card
    // with the genre name on it, which is what it did before there was art.
    spec.image = cat.image.empty() ? nullptr : cat.image.c_str();
    if (open_pages) {
      spec.cmd = "EcNavToLocalEpixManifest";
      spec.param1 = url.c_str();
    }
    if (!AddSlot(ctx, base, channel, spec)) break;
    ++added_cats;

    if (categories_only) continue;  // the titles belong to the category, not the row

    int shown = 0;
    for (const ListEntry* item : cat.games) {
      if (shown >= per_row) break;
      if (item->image.empty()) continue;  // a tile with no picture is a blank card
      SlotSpec t{};
      t.name = item->title.c_str();
      t.description = item->title.c_str();
      t.description2 = item->subtitle.empty() ? nullptr : item->subtitle.c_str();
      t.epix_id = item->epix_id.c_str();
      t.image = item->image.c_str();
      // A title with metadata opens its own page. Without any -- a hand-written
      // list, or a fetch run with --no-details -- there is nothing to show, and
      // the Games Library is a better destination than a page of blank cards.
      std::string durl;
      if (open_pages && item->has_details()) {
        durl = kCategoryUrl + CategoryKey(kind + ":title", item->extra);
        t.cmd = "EcNavToLocalEpixManifest";
        t.param1 = durl.c_str();
      } else {
        t.cmd = "EcNavToGamesLibrary";
      }
      t.helptext = "%EvResStr(IDS_SELECTSLOT)%";
      if (!AddSlot(ctx, base, channel, t)) break;  // 64-slot ceiling
      ++shown;
      ++added_items;
    }
  }
  if (added_cats) {
    REXKRNL_WARN("[slots] '{}': {} of {} categor(ies), {} item(s) in the row", id, added_cats,
                 cats.size(), added_items);
    if (open_pages) PrebuildCategoryPages(ctx, base, kind, cats);
  }
  return added_cats + added_items;
}

// The title id a marketplace row carries, as a number.
//
// The row keeps it as the eight hex digits the storefront used, which is also
// what names the title's folder under the content root, so the same string
// answers both "which game is this" and "is it installed".
uint32_t TitleIdOf(const ListEntry& item) {
  if (item.extra.size() != 8) return 0;
  char* end = nullptr;
  const unsigned long id = std::strtoul(item.extra.c_str(), &end, 16);
  return (end && *end == 0) ? uint32_t(id) : 0;
}

bool IsInstalled(const ListEntry& item) {
  const uint32_t id = TitleIdOf(item);
  return id && !nxe_game::PackagePathForTitle(id).empty();
}

// A page describing one title.
//
// The Xbox 360 had a real product page for this and it is not in the package:
// EsTempDetails is in the scene enum, but no details scene ships in homepage.xzp
// or slots.xzp -- it lived in the Marketplace app, Dash.MP.ContentExplorer,
// which this port does not have. What can be built is a page of the same slots
// everything else uses, carrying what the storefront actually said about the
// game: who made it, who published it, when it arrived, how it was rated, and
// its own store copy.
//
// The box art leads, so the page opens on the game rather than on a fact.
uint32_t FillDetailChannel(PPCContext& ctx, uint8_t* base, uint32_t container,
                           const ListEntry& item) {
  const uint32_t channel =
      AddChannel(ctx, base, container, item.title.c_str(), item.title.c_str());
  if (!channel) return 0;

  int added = 0;
  auto card = [&](const char* label, const std::string& value, const char* image) {
    if (value.empty() && !image) return;
    SlotSpec spec{};
    spec.name = label ? label : item.title.c_str();
    spec.description = label ? label : item.title.c_str();
    spec.description2 = value.empty() ? nullptr : value.c_str();
    spec.image = image;
    spec.helptext = "%EvResStr(IDS_SELECTSLOT)%";
    // No cmd anywhere on this page: these are facts, not somewhere to go, and a
    // tile that swallowed A without doing anything would read as broken.
    if (AddSlot(ctx, base, channel, spec)) ++added;
  };

  card(nullptr, item.subtitle, item.image.empty() ? nullptr : item.image.c_str());

  // Play, when the game is actually here.
  //
  // The row's title id is the same one that names the title's folder under the
  // content root, so a game staged in storage can be played from the page that
  // advertises it -- which is what the storefront did for anything you owned.
  // Nothing is downloaded and nothing is fetched: this only ever offers what is
  // already on the disk.
  //
  // It reuses the manifest command rather than EcLaunchLocalTitle so it lands in
  // the hook below, where the emulator the library already uses can run it.
  static std::string s_play_url;  // borrowed by the slot for the length of AddSlot
  if (IsInstalled(item)) {
    s_play_url = kCategoryUrl + std::string("play:") + item.extra;
    SlotSpec play{};
    play.name = "Play Game";
    play.description = "Play Game";
    play.description2 = "Installed on this console";
    play.cmd = "EcNavToLocalEpixManifest";
    play.param1 = s_play_url.c_str();
    play.helptext = "%EvResStr(IDS_SELECTSLOT)%";
    if (AddSlot(ctx, base, channel, play)) ++added;
  }

  card("Published by", item.publisher, nullptr);
  card("Developed by", item.developer, nullptr);
  card("Released", item.released, nullptr);
  if (!item.rating.empty()) {
    static std::string s_rating;  // borrowed by the slot for the length of the call
    s_rating = item.rating + " out of 5";
    card("Rating", s_rating, nullptr);
  }
  card("About this game", item.description, nullptr);

  REXKRNL_WARN("[slots] detail page '{}': channel {:#x}, {} card(s)", item.title, channel, added);
  return channel;
}

// A URL in guest memory, as UTF-16. The string setters take a field to write
// into rather than handing back a bare pointer, and what 0x922CD1D8 wants is the
// bare pointer, so this writes one. Category URLs are ASCII.
uint32_t GuestWide(PPCContext& ctx, uint8_t* base, const std::string& s) {
  const uint32_t bytes = uint32_t(s.size() + 1) * 2;
  const uint32_t p = Call(ctx, base, __imp__sub_92144098, bytes);
  if (!p) return 0;
  auto* out = reinterpret_cast<be<uint16_t>*>(base + p);
  for (size_t i = 0; i < s.size(); ++i) out[i] = uint16_t(uint8_t(s[i]));
  out[s.size()] = 0;
  return p;
}

// Build every category's container up front rather than on first press.
//
// The manager keys containers by URL, so the press-time path finds these
// already made and only has to push the page. Doing it here also means a
// container the manager refuses shows up in the boot log, next to the row it
// belongs to, instead of as a tile that does nothing when it is pressed.
//
// Cheap: slots are built, not drawn. Nothing fetches a picture until the shell
// prepares a channel, which does not happen until that page is opened.
// What a built page is: the container the manager gave us, and the exact URL
// pointer it was registered under.
//
// Both are kept because the manager's own lookup cannot be relied on to find
// the container again from an equal-but-different string -- asking it a second
// time produced a second, empty container rather than the one already built.
// Holding the handle here means a category is built once however the lookup
// behaves, and handing the page back the same pointer keeps it looking at the
// container that actually has the games in it.
struct CategoryPage {
  uint32_t container = 0;
  uint32_t url = 0;
  uint32_t channel_id = 0;  // the id string, for 0x922CF3F8 and the scene params
  uint32_t channel = 0;     // the channel itself, to prepare when the page opens
  bool prepared = false;    // 0x922D60B8 has run over it; it only needs to once
};

std::map<std::string, CategoryPage>& CategoryPages() {
  static std::map<std::string, CategoryPage> s_pages;
  return s_pages;
}

bool IsCategoryChannel(uint32_t channel) {
  if (!channel) return false;
  for (const auto& [name, page] : CategoryPages()) {
    if (page.channel == channel) return true;
  }
  return false;
}

bool IsCategoryContainer(uint32_t container) {
  if (!container) return false;
  for (const auto& [name, page] : CategoryPages()) {
    if (page.container == container) return true;
  }
  return false;
}

void PrebuildCategoryPages(PPCContext& ctx, uint8_t* base, const std::string& kind,
                           const std::vector<MarketCategory>& cats) {
  const uint32_t scratch = CategoryScratch(ctx, base);
  if (!scratch) return;

  size_t built = 0;
  for (const auto& cat : cats) {
    const std::string key = CategoryKey(kind, cat.name);
    if (CategoryPages().count(key)) continue;
    const uint32_t url = GuestWide(ctx, base, CategoryUrl(kind, cat.name));
    if (!url) continue;
    StoreBe32(base, scratch, 0);
    const int32_t hr = int32_t(Call5(ctx, base, __imp__sub_922CD1D8, kEpixManager, url, scratch));
    const uint32_t container = Be32(base, scratch);
    if (hr < 0 || !container) {
      REXKRNL_WARN("[slots] '{}': manager refused a container ({:#x})", cat.name, uint32_t(hr));
      continue;
    }
    StoreBe32(base, container + 256, 0);  // load result: S_OK, since nothing is fetched
    uint32_t built_channel = 0;
    if (Be32(base, container + 80) == 0) {
      built_channel = FillCategoryChannel(ctx, base, container, kind, cat);
    }
    CategoryPages()[key] =
        CategoryPage{container, url, GuestWide(ctx, base, cat.name), built_channel};
    // Prepare here, while the shell is still in its own loading phase, rather
    // than waiting for the page to be opened from a button handler.
    if (built_channel) Call(ctx, base, __imp__sub_922D60B8, built_channel);

    // And a page per title underneath it, for the games that carry metadata.
    for (const ListEntry* item : cat.games) {
      if (!item->has_details()) continue;
      const std::string dkey = CategoryKey(kind + ":title", item->extra);
      if (CategoryPages().count(dkey)) continue;
      const uint32_t durl = GuestWide(ctx, base, kCategoryUrl + dkey);
      if (!durl) continue;
      StoreBe32(base, scratch, 0);
      const int32_t dhr =
          int32_t(Call5(ctx, base, __imp__sub_922CD1D8, kEpixManager, durl, scratch));
      const uint32_t dcontainer = Be32(base, scratch);
      if (dhr < 0 || !dcontainer) continue;
      StoreBe32(base, dcontainer + 256, 0);  // load result: S_OK, nothing is fetched
      uint32_t dchannel = 0;
      if (Be32(base, dcontainer + 80) == 0) {
        dchannel = FillDetailChannel(ctx, base, dcontainer, *item);
      }
      CategoryPages()[dkey] =
          CategoryPage{dcontainer, durl, GuestWide(ctx, base, item->title), dchannel};
      if (dchannel) Call(ctx, base, __imp__sub_922D60B8, dchannel);
    }
    ++built;
  }
  REXKRNL_WARN("[slots] {}: {} of {} category page(s) ready", kind, built, cats.size());
}

}  // namespace

// Finding a category's container.
//
// 0x922CC270 is the manager's "do I already have a container for this URL"
// lookup, and for our URLs it always answers no -- asking for one already built
// produced a second, empty container instead. Whatever it matches on, it is not
// the string, so the page that gets pushed misses in exactly the same way: it
// looks up the URL it was handed, is given a fresh empty container, and waits
// for a fetch that is never going to happen because nothing is fetching. That
// wait is where opening a category stopped.
//
// Answering the lookup properly fixes both halves at once -- the page finds the
// container that actually has the titles in it, and because that container is
// one of ours, the wait below returns immediately.
//
// 0x922CD1D8 releases whatever this returns when it does not need it, so the
// reference has to be counted up on the way out or the container would be freed
// out from under the page.
extern "C" void sub_922CC270(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t url_ptr = ctx.r4.u32;
  if (url_ptr) {
    const std::string url = WideAt(base, url_ptr);
    if (url.rfind(kCategoryUrl, 0) == 0) {
      const auto it = CategoryPages().find(url.substr(std::strlen(kCategoryUrl)));
      if (it != CategoryPages().end() && it->second.container) {
        Call(ctx, base, __imp__sub_922CEA50, it->second.container);
        ctx.r3.u32 = it->second.container;
        return;
      }
    }
  }
  __imp__sub_922CC270(ctx, base);
}

// Waiting for a container that is already finished.
//
//     if (0x921465E0(*(container + 236), ...) == 258) return 0x8000000A;
//     return *(container + 256);
//
// so this blocks on an event at +236 and, once it is signalled, reports the load
// result at +256. Only a fetch completing ever signals that event, and a category
// container is filled in place and never fetches anything, so anyone who waits on
// one waits forever -- which is what the page push did: it came up, asked whether
// its container had finished loading, and stopped there with the shell frozen.
//
// For our containers the answer is simply yes, with S_OK.
extern "C" void sub_922CED88(PPCContext& __restrict ctx, uint8_t* base) {
  if (IsCategoryContainer(ctx.r3.u32)) {
    ctx.r3.u32 = 0;  // S_OK: built, not loaded, and done either way
    return;
  }
  __imp__sub_922CED88(ctx, base);
}

// How far every category's pictures have got.
//
// 0x922D3AE8 refuses to draw an epix whose state at +48 is below 2, so a tile
// whose picture has not arrived is text on a blank card. Reporting the whole set
// on each open, rather than only the page being opened, is what showed the
// loader giving up part way through: it stopped at 156 of 483 and never
// recovered, which is how the memory ceiling came to light.
void LogCategoryPictures(uint8_t* base, const std::string& opening) {
  int drawable = 0, pending = 0, unbound = 0;
  for (const auto& [key, page] : CategoryPages()) {
    if (!page.channel) continue;
    int seen = 0;
    for (uint32_t node = Be32(base, page.channel + 48); node && node != page.channel + 48;) {
      const uint32_t slot = node - 4;
      // +72 is the epix 0x922D5B70 builds from the slot's shallowimg, which is
      // where a tile with no named <epix> keeps its picture.
      const uint32_t epix = Be32(base, slot + 72);
      if (!epix) {
        ++unbound;
      } else if (Be32(base, epix + 48) >= 2) {
        ++drawable;
      } else {
        ++pending;
      }
      node = Be32(base, node);
      if (++seen > 80) break;  // the list is circular; never trust it blindly
    }
  }
  REXKRNL_WARN("[slots] opening '{}'; pictures across every page: {} drawable, {} loading, "
               "{} unbound",
               opening, drawable, pending, unbound);
}

// Open one of our pages, by "<kind>:<name>".
//
// Shared by the tile that navigates here and by the gamer blade's Messages item,
// which arrives through XamShowMessagesUI rather than through a slot.
int32_t OpenPageByKey(PPCContext& ctx, uint8_t* base, uint32_t nav, const std::string& key) {
  const uint32_t scratch = CategoryScratch(ctx, base);
  if (!scratch) {
    return int32_t(0x8007000E);  // E_OUTOFMEMORY
  }

  // The page was built at boot, so this is a lookup and not a fetch. Asking the
  // manager again would hand back a second, empty container.
  const auto page = CategoryPages().find(key);
  if (page == CategoryPages().end() || !page->second.container) {
    REXKRNL_WARN("[slots] '{}': no page was built for it", key);
    return int32_t(0x80004005);  // E_FAIL
  }
  const uint32_t container = page->second.container;

  // Prepare the channel before the page comes up.
  //
  // The shell only walks the channels of the container it booted with, so a page
  // opened over one of ours is never prepared: 0x922D5B70 never runs over these
  // slots, no picture is bound, and every tile comes up as text on a blank card.
  // This is the same call the shell makes for its own channels.
  //
  // Preparing only starts the loads -- 0x922D48B8 comes back 0x8000000A, pending
  // -- so the pictures arrive over the following frames rather than before the
  // page does. Waiting for them here was worse than useless: spinning on the
  // dashboard's pump held the very thread that completes them.
  //
  // Once is enough, and once is important: the open is retried while the shell
  // is busy, and re-preparing on every attempt would restart every load.
  if (page->second.channel && !page->second.prepared) {
    Call(ctx, base, __imp__sub_922D60B8, page->second.channel);
    LogCategoryPictures(base, key);
    page->second.prepared = true;
  }

  // 0x922C5BF8 selects a channel before pushing whenever param2 is set. Ours has
  // one channel, so there is nothing to choose between -- but the scene still
  // expects a channel to have been picked, and comes up on nothing when it has
  // not.
  if (page->second.channel_id) {
    Call(ctx, base, __imp__sub_922CF3F8, container, page->second.channel_id);
  }

  // The parameter block 0x922C5580 forwards to the scene: [0] unused, [1] the
  // URL naming the container, [2] the channel to focus. The URL is the pointer
  // the container was registered under, not a caller's copy of the text.
  const uint32_t params = scratch + 8;
  for (uint32_t i = 0; i < 7; ++i) StoreBe32(base, params + i * 4, 0);
  StoreBe32(base, params + 4, page->second.url);
  StoreBe32(base, params + 8, page->second.channel_id);

  REXKRNL_WARN("[slots] opening '{}': nav {:#x}, container {:#x}, {} channel(s)", key, nav,
               container, Be32(base, container + 80));
  const int32_t pushed = int32_t(
      Call5(ctx, base, __imp__sub_922C5580, nav, kHomepagePackage, kHomepageScene, params, 1));
  REXKRNL_WARN("[slots] opened '{}': push {:#x}", key, uint32_t(pushed));
  return pushed;
}

// The navigator the shell is actually using.
//
// A page has to be pushed onto the same one the dashboard navigates with, and
// asking 0x92141108 for one does not give you that -- it hands back a different
// navigator (0x1117d against the 0x1014d the tiles get), and a page pushed onto
// it opens correctly and then takes the process down on B:
//
//     opened 'messages:Messages'
//     [theme] BACK leaving=0x21a55 target=0x11a5a
//     0xC0000005, read of 0x100000000
//
// 0x922D30C8 is the command dispatcher and its first argument is the navigator,
// so every button press through a slot goes past here carrying the right one.
// Recorded rather than derived, because it is the only reliable source.
std::atomic<uint32_t> g_last_nav{0};

extern "C" void sub_922D30C8(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.r3.u32) {
    g_last_nav.store(ctx.r3.u32, std::memory_order_relaxed);
  }
  __imp__sub_922D30C8(ctx, base);
}

// Open a page from outside this file.
//
// The gamer blade's Messages item lands in XamShowMessagesUI, which has a user
// index rather than a navigator, so one is asked for the same way the guide-look
// path asks: 0x92141108 with r3 = 1.
namespace nxe_channels {

bool HasCategoryPage(const std::string& kind, const std::string& name) {
  const auto page = CategoryPages().find(CategoryKey(kind, name));
  return page != CategoryPages().end() && page->second.container != 0;
}

void NoteNavigator(uint32_t nav) {
  if (nav) {
    g_last_nav.store(nav, std::memory_order_relaxed);
  }
}

// Navigate to a scene the way the dashboard does.
//
// dash_2a65(navigator, container, scene, a4, a5, out) is the call behind every
// item on the gamer blade, and the two arguments past the scene name are what
// parameterise it -- a4 being the gamer a scene in gamer.xzp is about. Pushing
// the scene with 0x922C5580 instead works, but there is nowhere to say who it
// is for, and GamerRootScene bound to nobody offers a stranger's panel.
int32_t NavigatePackageScene(PPCContext& ctx, uint8_t* base, uint32_t nav,
                             const std::string& package, const std::string& scene,
                             uint32_t a4, uint32_t a5) {
  if (!nav || package.empty() || scene.empty()) return int32_t(0x80004005);  // E_FAIL
  const uint32_t names = SceneNameBlock(ctx, base, package, scene);
  if (!names) return int32_t(0x8007000E);  // E_OUTOFMEMORY

  PPCContext call = ctx;
  call.r3.u64 = nav;
  call.r4.u64 = names;
  call.r5.u64 = names + uint32_t(package.size() * 2 + 2);
  call.r6.u64 = a4;
  call.r7.u64 = a5;
  call.r8.u64 = 0;  // the dashboard passes no out pointer either
  __imp__sub_921F6190(call, base);
  const int32_t hr = int32_t(call.r3.u32);
  REXKRNL_WARN("[slots] navigated '{}#{}' on nav {:#x} a4 {:#x} -> {:#x}", package, scene,
               nav, a4, uint32_t(hr));
  return hr;
}

// Push any scene out of any package the dashboard can reach.
//
// 0x922C5580 is not specific to homepage.xur: it takes a package name and a
// scene name, both UTF-16, and the pages here only ever handed it the homepage
// pair because that is what they wanted. gamer.xzp carries the real gamer blade
// -- GamerRootScene.xur is the list, GamerCardPanelScene.xur the panel beside it
// -- and the dashboard already navigates into that package for Themes, so the
// scenes are reachable by name.
//
// The names are written into a block of guest memory rather than found in the
// image, so any scene can be named without hunting for a string constant first.
int32_t PushPackageScene(PPCContext& ctx, uint8_t* base, uint32_t nav,
                         const std::string& package, const std::string& scene) {
  if (!nav || package.empty() || scene.empty()) return int32_t(0x80004005);  // E_FAIL

  const uint32_t params = CategoryScratch(ctx, base) + 8;
  for (uint32_t i = 0; i < 7; ++i) StoreBe32(base, params + i * 4, 0);

  const uint32_t pkg = SceneNameBlock(ctx, base, package, scene);
  if (!pkg) return int32_t(0x8007000E);  // E_OUTOFMEMORY
  const uint32_t scn = pkg + uint32_t(package.size() * 2 + 2);

  const int32_t pushed =
      int32_t(Call5(ctx, base, __imp__sub_922C5580, nav, pkg, scn, params, 1));
  REXKRNL_WARN("[slots] pushed '{}#{}' on nav {:#x} -> {:#x}", package, scene, nav,
               uint32_t(pushed));
  return pushed;
}

int32_t OpenCategoryPageOn(PPCContext& ctx, uint8_t* base, uint32_t nav,
                           const std::string& kind, const std::string& name) {
  if (!nav) return int32_t(0x80004005);  // E_FAIL
  return OpenPageByKey(ctx, base, nav, CategoryKey(kind, name));
}

int32_t OpenCategoryPageResult(PPCContext& ctx, uint8_t* base, const std::string& kind,
                               const std::string& name) {
  // The one the shell navigates with, seen going past the dispatcher. Asking
  // 0x92141108 for a fresh one instead is what made B crash.
  uint32_t nav = g_last_nav.load(std::memory_order_relaxed);
  if (!nav) {
    PPCContext ask = ctx;
    ask.r3.u64 = 1;
    sub_92141108(ask, base);
    nav = ask.r3.u32;
    REXKRNL_WARN("[slots] no navigator seen yet; asking for one gave {:#x}", nav);
  }
  if (!nav) {
    REXKRNL_WARN("[slots] no navigator to open '{}:{}' with", kind, name);
    return int32_t(0x80004005);  // E_FAIL
  }
  return OpenPageByKey(ctx, base, nav, CategoryKey(kind, name));
}

bool OpenCategoryPage(PPCContext& ctx, uint8_t* base, const std::string& kind,
                      const std::string& name) {
  return OpenCategoryPageResult(ctx, base, kind, name) >= 0;
}

}  // namespace nxe_channels

// Opening a category.
//
// This is EcNavToLocalEpixManifest's handler. Compared with its remote twin at
// 0x922C5AA8 the two are the same function -- find or build a container for the
// URL, pump it until it stops returning 0x8000000A, then push homepage.xur over
// it with 0x922C5580 -- and differ only in the gate at the top:
//
//     remote  if (!0x922CD528(manager)) return;      // Xbox LIVE has to be up
//     local   if (!(XboxHardwareInfo & 0x10)) return; // devkit/kiosk bit
//
// so nothing about the URL is treated differently; "local" only means it is
// allowed to run offline, on hardware with that bit set. Retail consoles do not
// have it, which is why the Experience Discs could open local manifests and a
// retail dashboard could not.
//
// For our own URLs neither gate is wanted and neither is the fetch: there is no
// manifest to parse, because the container is filled here directly with the same
// AddChannel and AddSlot the Game Marketplace row is built from. Everything else
// -- the container, the page, the back stack -- is the dashboard's own.
//
// Anything that is not ours falls through to the original untouched.
extern "C" void sub_922C5BF8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t nav = ctx.r3.u32;
  const uint32_t url_ptr = ctx.r4.u32;
  const std::string url = url_ptr ? WideAt(base, url_ptr) : std::string();

  if (url.rfind(kCategoryUrl, 0) != 0) {
    __imp__sub_922C5BF8(ctx, base);
    return;
  }

  std::string name = url.substr(std::strlen(kCategoryUrl));

  // "setpic:<file>" sets the profile picture rather than opening a page.
  //
  // The verb is not "gamerpic:", though it was. A page key is kind + ":" + name
  // (see CategoryKey), so the Change Gamer Picture page's own key is literally
  // "gamerpic:Gamer Picture" -- and this branch claimed it, read "Gamer Picture"
  // as a filename, found no such picture and returned an error. The tile on the
  // profile page did nothing at all as a result. Only the blade route worked,
  // because profile_ui.cpp calls OpenCategoryPageOn directly and never builds a
  // URL. A verb and a page kind cannot share a prefix; this one no longer does.
  if (name.rfind("setpic:", 0) == 0) {
    const std::string file = name.substr(7);
    const bool set = nxe_tiles::SetGamerPicture(
        nxe_paths::Resolve(REXCVAR_GET(gamerpics_dir)), file);
    REXKRNL_WARN("[slots] gamer picture '{}' -> {}", file, set ? "set" : "not set");
    ctx.r3.u32 = set ? 0u : uint32_t(-2147467259);
    return;
  }

  // "play:<titleid>" runs the game rather than opening a page.
  if (name.rfind("play:", 0) == 0) {
    const std::string hex = name.substr(5);
    uint32_t id = 0;
    if (hex.size() == 8) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(hex.c_str(), &end, 16);
      if (end && *end == 0) id = uint32_t(parsed);
    }
    const bool ran = id && nxe_game::LaunchStagedTitle(ctx, base, id);
    REXKRNL_WARN("[slots] play {:#010x} -> {}", id, ran ? "launched" : "not launched");
    ctx.r3.u32 = ran ? 0u : uint32_t(-2147467259);
    return;
  }

  ctx.r3.u32 = uint32_t(OpenPageByKey(ctx, base, nav, name));
}

// Does the render path ever prepare these slots, and what does it bind?
//
// 0x922D60B8 walks a channel's slot list and calls this on each one; this is
// where a slot's epixid becomes an actual epix. 0x922D5B70 sorts the result by
// format:
//
//     if ( *(v6 + 12) == 1 ) a1[19] = v6;    // EpixScene -> slot+76
//     else                   a1[16] = v6;    // everything else -> slot+64
//
// so an IMG epix lands at +64 and a scene at +76, and a slot with neither is
// the blank gradient card. Logging both after the fact says outright whether a
// tile came out empty because nothing bound, or because something bound and did
// not draw.
extern "C" void sub_922D5B70(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t slot = ctx.r3.u32;
  __imp__sub_922D5B70(ctx, base);
  if (!slot) return;
  const uint32_t shallow = Be32(base, slot + 72);

  // Log a bounded number of these, but never let that bound reach the work
  // below: the fetch this function starts is what makes a picture tile draw at
  // all, and an early return here once the log filled up silently skipped it.
  // The shell prepares two dozen stock slots before it reaches anything added
  // late, so the tiles that lost their images were exactly the ones furthest
  // down the list.
  static int s_logged = 0;
  if (s_logged < 24) {
    ++s_logged;
    REXKRNL_WARN("[slots] prepare {:#x} epixid='{}' -> shallow={:#x}{} named={:#x} scene={:#x}",
                 slot, WideAt(base, Be32(base, slot + 60)), shallow,
                 shallow ? (" fmt=" + std::to_string(Be32(base, shallow + 12)) + " path='" +
                            WideAt(base, Be32(base, shallow + 20)) + "'")
                         : std::string(),
                 Be32(base, slot + 64), Be32(base, slot + 76));
  }

  // Start the fetch these slots would otherwise never get.
  //
  // An IMG epix is not drawable until it has been loaded: 0x922D3AE8 refuses
  // anything whose state at +48 is below 2, and that state is only raised by
  // 0x922D37C0 -> 0x922D7AD0, which for format 6 is a real fetch. A scene needs
  // none of this -- 0x922D37C0 does nothing at all for format 1 -- which is why
  // every stock tile draws and these did not.
  //
  // The fetch is normally kicked off by 0x922D5D80, and that function opens with
  //
  //     if ( !a1[24] && a1[28] == 267242991 ) return 1;   // +96 cond, +112 rating
  //
  // so a slot carrying neither a <condition> nor a <rating> reports itself
  // visible and loads nothing. The slot constructor leaves both at exactly those
  // values, so every slot built here takes that early return.
  //
  // Inventing a rating or a condition to dodge it would feed straight into the
  // visibility tests below it -- sub_922DDB28 on the rating, sub_922C8B78 on the
  // condition -- and a wrong guess hides the tile instead of drawing it. Calling
  // the loader directly asks for the one thing that is actually missing.
  //
  // Here rather than at AddSlot because slot+72 does not exist until the guest
  // call above has built it, and only slots with an image have one -- the stock
  // slots have +72 == 0 and are left completely alone.
  if (shallow && Be32(base, shallow + 12) == kFormatImg && Be32(base, shallow + 48) < 2) {
    uint32_t flags = uint32_t(REXCVAR_GET(channel_slot_load_flags));

    // 0x800 on a category page's slots, and only there.
    //
    // 0x922D7108 is the completion, and it opens with
    //
    //     if ((flags & 0x800) == 0) return 0;
    //
    // so without that bit the read finishes, reports success, and then nothing
    // marks the picture loaded: 0x922D99A8 never runs, the epix stays at state 1
    // and 0x922D3AE8 refuses to draw it. That is why a category page came up as
    // titles on blank cards while every one of its files had read back 0x0.
    //
    // The shell's own channels get there another way and are left on the cvar's
    // value, because they already draw and this is not the place to find out
    // what else the bit does to them.
    if (IsCategoryChannel(Be32(base, slot + 16))) flags |= 0x800;
    Call(ctx, base, __imp__sub_922D48B8, slot, flags);
    REXKRNL_WARN("[slots]   kicked the image load for {:#x} with flags {:#x}; epix state now {}",
                 slot, flags, Be32(base, shallow + 48));
  }
}

// Hand the scene the picture directly instead of a cache reference.
//
// 0x922D3AE8 does not give picture.xur the image URL. It gives it
//
//     dash_298b_0: sub_9220B950(out, n, L"imagecache://%08X/%s", key, url)
//
// so the parameter is imagecache://<key>/memory://<addr>,<size>. The inner
// memory:// URL is right -- 0x922DA060 builds it over the bytes this port just
// read off disk, and the disc tile proves the shell loads memory:// happily --
// but the wrapper names an entry in an image cache that only the download path
// ever populates. Having gone in through the file loader instead, there is no
// such entry, and the trace shows the result: picture.xur renders, and the image
// URL is never requested, because what it was handed resolves to nothing.
//
// Unwrapping it hands over the memory:// URL the cache would have pointed at.
// Gated on the input rather than the caller so it only ever touches a request
// that is already a memory:// image; every other user of this helper passes
// something else and is left alone.
extern "C" void sub_9220BBF8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t in = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  const uint32_t out_chars = ctx.r6.u32;
  __imp__sub_9220BBF8(ctx, base);
  if (!in || !out || int32_t(ctx.r3.u32) < 0) return;
  const std::string url = WideAt(base, in);
  if (url.rfind("memory://", 0) != 0 || url.size() + 1 > out_chars) return;
  auto* text = reinterpret_cast<be<uint16_t>*>(base + out);
  for (size_t i = 0; i < url.size(); ++i) {
    text[i] = uint16_t(uint8_t(url[i]));
  }
  text[url.size()] = 0;
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("[slots] tile pictures are handed over as '{}' rather than an imagecache key", url);
  }
}

// Fill in a channel whose definition could not be fetched.
//
// 0x922DAC08 loads a <definitionpath> channel: it takes the path from
// channel+32, picks a fetch mode the way the epix images do -- bit 4 local when
// channel+96 is set, bits 0|1 for an HTTP download when it is not -- and hands
// it to 0x922D7AD0. welcome.xml and xbox360channel.xml both come back 0x0 here.
// communitychannel.xml comes back 0x8000FFFF, every run, which is why the parse
// then discards the channel.
//
// Rather than keep chasing that failure, this builds what the document says.
// Setting channel+16 marks the definition complete -- the same field the
// channeldef handler writes at </channeldef> -- so nothing downstream treats the
// channel as still waiting.
extern "C" void sub_922DAC08(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t channel = ctx.r3.u32;
  const std::string id = channel ? ChannelId(base, channel) : std::string();
  const std::string path = channel ? WideAt(base, Be32(base, channel + 32)) : std::string();
  __imp__sub_922DAC08(ctx, base);
  const int32_t result = int32_t(ctx.r3.u32);

  static int s_logged = 0;
  if (s_logged < 16) {
    ++s_logged;
    REXKRNL_WARN("[slots] channel definition '{}' path='{}' -> {:#x}", id, path,
                 static_cast<uint32_t>(result));
  }

  if (!channel || id != "COMMUNITY" || !REXCVAR_GET(channel_community_slots)) return;
  static bool s_done = false;
  if (s_done) return;
  s_done = true;

  // The description would have come from the channeldef; without it the tab has
  // no name at all.
  //
  // The manifest's own value is %EvResStr(IDS_CHANNELNAME_FRIENDS)%, which draws
  // as "Friends" -- the same label the stock offline Friends channel already
  // carries, leaving two identically named tabs and no way to tell which is
  // which. "Community" is the channel's own id and makes it findable.
  SetString(ctx, base, __imp__sub_922CA4A8, channel + 28, "Community");
  StoreBe32(base, channel + 16, 1);

  // And the spacing, which is the rest of what the document would have set:
  //
  //     <type>local</type>
  //     <spacing>920</spacing>
  //
  // 0x922DAEF0 writes <spacing> to channel+80 and clamps it into [200, 1100],
  // so a channel that never parsed one is left at whatever the constructor gave
  // it. The channel was in the list with its slots and its description and still
  // did not appear in the tab strip, which is what a zero-height tab looks like.
  // 920 is the manifest's own value.
  StoreBe32(base, channel + 80, 920);

  // Clear the condition, which is the actual reason the tab never appeared.
  //
  // The channel carries one at +72 -- 0x922D5620 puts it there -- and it can only
  // have come from communitychannel.xml, so that document does parse at least as
  // far as its <condition> before whatever makes the load return 0x8000FFFF.
  // The condition is
  //
  //     <condition>!EcoLiveTier(None)</condition>
  //
  // which asks for a Live tier above None. Signed out that is false, and a
  // channel whose condition is false is kept in the list and left out of the tab
  // strip -- exactly what Promotions does with EcoEventsAvailable(). Both were
  // missing from the strip for the same honest reason.
  //
  // So this is a deliberate deviation from stock rather than a fix: a real
  // disconnected console would not show this tab at all. Clearing +72 is what a
  // channel with no condition looks like, which is how WelcomeToXbox360 and
  // XBOX360 are always visible.
  StoreBe32(base, channel + 72, 0);

  int added = 0;
  for (const auto& spec : kAddFriendSlots) {
    if (AddSlot(ctx, base, channel, spec)) ++added;
  }

  // And prepare them, which the failed load never got as far as doing.
  //
  // 0x922DAC08 runs 0x922D60B8 over the channel's slots at its LABEL_19, reached
  // only once the definition is in hand. Coming in after it has already returned
  // an error means the slots added above have never been through 0x922D5B70:
  // no epix is bound, no image is fetched, and the tile draws as nothing. The
  // first attempt at this stopped here and the tab came up empty.
  Call(ctx, base, __imp__sub_922D60B8, channel);

  REXKRNL_WARN("[slots] 'COMMUNITY': definition failed {:#x}; built and prepared {} slot(s) from "
               "communitychannel.xml, {} now",
               static_cast<uint32_t>(result), added, Be32(base, channel + 56));
}

extern "C" void sub_922DAEF0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t parser = ctx.r3.u32;
  __imp__sub_922DAEF0(ctx, base);
  if (!parser || !REXCVAR_GET(channel_extra_slots)) return;

  // State 7 is the handler's "gave up". 0x922DBB88 discards a channel whose
  // definition ended in a non-zero state -- "if ( v11 && *(v11 + 5200) )
  // sub_922CF148(...)" -- which is why eight channels are created and only seven
  // survive. Naming the one that fails, and the element it was on, is the whole
  // question for COMMUNITY.
  if (Be32(base, parser + 5208) == 7) {
    static int s_logged = 0;
    if (s_logged < 8) {
      ++s_logged;
      const uint32_t ch = Be32(base, parser + 5220);
      REXKRNL_WARN("[slots] channeldef FAILED for '{}' on element '{}' (text '{}')",
                   ch ? ChannelId(base, ch) : std::string("(none)"), Ansi(base, parser + 1052, 64),
                   Ansi(base, parser + 1084, 96));
    }
  }

  // State 8 is "the channeldef just closed"; anything else is mid-definition.
  if (Be32(base, parser + 5208) != 8) return;

  const uint32_t channel = Be32(base, parser + 5220);
  if (!channel) return;
  const std::string id = ChannelId(base, channel);

  // The Add Friend tile from communitychannel.xml, on the channel it belongs to.
  //
  // COMMUNITY is the channel the document was written for, but it is a second
  // tab also labelled Friends and it only appears at all if its
  // "!EcoLiveTier(None)" condition is cleared. Friends is the same idea already
  // on screen, so the tile goes here and the stock Meet and Greet card stays as
  // the head of the row.
  if (id == "Friends") {
    static bool s_friends_done = false;
    if (s_friends_done) return;
    s_friends_done = true;
    const uint32_t before_f = Be32(base, channel + 56);
    // The Meet and Greet card goes, the same as the Game Marketplace one: it is
    // an upsell for a service this port cannot reach, and Add Friend is the
    // channel's real content.
    HideStockUpsell(base, channel, id);

    // The per-person pages, built before the tiles that look for them.
    // HasCategoryPage answers from what has already been built, so a friend tile
    // asking first would be told there is no page and fall back for the whole
    // session.
    PrebuildCategoryPages(ctx, base, "person", PeopleCategories());

    int added_f = 0;
    for (const auto& spec : kAddFriendSlots) {
      if (AddSlot(ctx, base, channel, spec)) ++added_f;
    }

    // Then one tile per friend, after Add Friend.
    //
    // A friend is an ordinary picture tile: the gamerpic is the shallowimg, the
    // gamertag the description and the presence the second line. EcShowGamerCard
    // takes the xuid in param1, which is why the file carries one -- a row with
    // no xuid still draws, it just has nothing to open.
    int added_friends = 0;
    for (const auto& fr : Friends()) {
      SlotSpec spec{};
      spec.name = fr.title.c_str();
      spec.description = fr.title.c_str();
      spec.description2 = fr.subtitle.empty() ? nullptr : fr.subtitle.c_str();
      spec.epix_id = fr.epix_id.c_str();
      spec.image = fr.image.empty() ? nullptr : fr.image.c_str();
      // Our own page when there is one, the system gamercard when there is not.
      // The gamercard is kept as the fallback rather than removed: it is the
      // right destination and costs nothing to try on a build where XAM can
      // draw it.
      std::string durl;
      if (nxe_channels::HasCategoryPage("person", fr.title)) {
        durl = CategoryUrl("person", fr.title);
        spec.cmd = "EcNavToLocalEpixManifest";
        spec.helptext = "%EvResStr(IDS_SELECTSLOT)%";
        spec.param1 = durl.c_str();
      } else if (!fr.extra.empty()) {
        spec.cmd = "EcShowGamerCard";
        spec.helptext = "%EvResStr(IDS_SELECTSLOT)%";
        spec.param1 = fr.extra.c_str();
      }
      // A row with no picture would draw as the blank gradient card, which is
      // worse than not being there at all.
      if (!spec.image) continue;
      if (AddSlot(ctx, base, channel, spec)) ++added_friends;
    }
    if (added_friends) {
      REXKRNL_WARN("[slots] 'Friends': {} friend tile(s) added", added_friends);
      added_f += added_friends;
    }

    // Then Recent Players, as one tile that opens the list.
    //
    // The same drill-down the marketplace categories use, for the same reason:
    // there are far more people here than the 64 slots a channel has, and
    // Friends has already spent 18 of them. The tile wears a grid of their
    // gamerpics, built by the fetch tool.
    added_f += BuildCategoryRow(ctx, base, channel, id, "friends", RecentCategories(),
                                /*categories_only=*/true, /*per_row=*/0,
                                REXCVAR_GET(channel_marketplace_pages));

    // The inbox's page, with no tile of its own on this row.
    PrebuildCategoryPages(ctx, base, "messages", MessageCategories());

    // The profile's page, likewise: the gamer blade's Profile item opens it.
    // See the XamShowGamerCardUI hook in profile_ui.cpp.
    PrebuildCategoryPages(ctx, base, "profile", ProfileCategories());

    // Change Gamer Picture, which the profile page links to.
    PrebuildCategoryPages(ctx, base, "gamerpic", GamerpicCategories());

    // Following and Followers, which the profile page links to in the same way.
    PrebuildCategoryPages(ctx, base, "following", FollowingCategories());
    PrebuildCategoryPages(ctx, base, "followers", FollowersCategories());

    // The inbox is not here. It hangs off the gamer blade's Messages item
    // instead, which is where it belongs -- see nxe_channels::OpenCategoryPage
    // and the XamShowMessagesUI hook in guide_bridge.cpp. Its page is still
    // built, by PrebuildCategoryPages below.

    // The channel keeps its stock spacing of 510. communitychannel.xml asks for
    // 920, but that suits a row of transnograd cutouts with no footprint of
    // their own; a tile drawn on picture.xur occupies its own card and sits in
    // the same pitch as every other channel.
    REXKRNL_WARN("[slots] '{}': {} slot(s) before, {} added, {} now", id, before_f, added_f,
                 Be32(base, channel + 56));
    return;
  }

  // Inside Xbox becomes Spotlight. The 9199 manifest labels this channel with
  // %EvResStr(IDS_INSIDEXBOX)%, but the tab in the shipping NXE is Spotlight and
  // that is the row the kiosk's own SPOTLIGHT channel was written for, so the
  // description is overwritten to match what goes in it.
  if (id == "Inside Xbox") {
    static bool s_spotlight_done = false;
    if (s_spotlight_done) return;
    s_spotlight_done = true;
    const uint32_t before_s = Be32(base, channel + 56);
    SetString(ctx, base, __imp__sub_922CA4A8, channel + 28, "Spotlight");
    HideStockUpsell(base, channel, id);
    int added_s = 0;
    for (const auto& spec : kSpotlightSlots) {
      if (AddSlot(ctx, base, channel, spec)) ++added_s;
    }
    REXKRNL_WARN("[slots] '{}' -> Spotlight: {} slot(s) before, {} added, {} now", id, before_s,
                 added_s, Be32(base, channel + 56));
    return;
  }

  // Video & Music Marketplace. Same treatment as the Game Marketplace: the
  // stock upsell card goes and the row becomes the store.
  //
  // Music has no half here. The Zune marketplace that backed it is gone and
  // nothing preserved it either, so the row is films and television and the
  // channel keeps its original name.
  if (id == "Video") {
    static bool s_video_done = false;
    if (s_video_done) return;
    s_video_done = true;
    if (VideoCategories().empty()) return;  // no video.txt: leave the channel alone

    const uint32_t before_v = Be32(base, channel + 56);
    HideStockUpsell(base, channel, id);  // the orange "Enjoy Instant Movie Night" card
    const int added_v =
        BuildCategoryRow(ctx, base, channel, id, "video", VideoCategories(),
                         REXCVAR_GET(channel_video_categories_only),
                         int(REXCVAR_GET(channel_marketplace_row_games)),
                         REXCVAR_GET(channel_marketplace_pages));
    REXKRNL_WARN("[slots] '{}': {} slot(s) before, {} added, {} now", id, before_v, added_v,
                 Be32(base, channel + 56));
    return;
  }

  if (id != "Games") return;

  static bool s_done = false;
  if (s_done) return;
  s_done = true;

  const uint32_t before = Be32(base, channel + 56);

  // The orange "Let us Play" card.
  HideStockUpsell(base, channel, id);

  int added = 0;

  // The marketplace, and nothing else.
  //
  // The kiosk's four navigation cards -- Game Library, What's New, Xbox Basics,
  // Storage -- used to lead this row. They are gone: this channel is the store,
  // and 0x922D6018 refuses any slot past the 64th, so every one they held back
  // was a game that could not be shelved.
  //
  // Real Xbox 360 Marketplace titles with their real title ids and the real
  // 219x300 box art the storefront served, rather than anything derived from
  // this console's play history -- the row is a shop, not a shelf.
  //
  // The catalogue is grouped: each genre gets a heading card and the games that
  // belong to it follow immediately after. Those genres are the marketplace's
  // own browse-by-genre categories, not something invented here -- Shooter,
  // Role Playing, Racing & Flying and the rest are the names the storefront
  // filed these titles under, and each game sits under the one it shipped with.
  // Your own games first, then the shop.
  //
  // The detail pages are built before the row so the tiles can find them: a tile
  // asks HasCategoryPage when it is built, and a page that does not exist yet
  // answers no for the rest of the session.
  PrebuildCategoryPages(ctx, base, "ach", AchievementCategories());
  PrebuildCategoryPages(ctx, base, "game", GameStatsCategories());
  added += BuildCategoryRow(ctx, base, channel, id, "mygames", TitleCategories(),
                           /*categories_only=*/true, /*per_row=*/0,
                           REXCVAR_GET(channel_marketplace_pages));

  added += BuildCategoryRow(ctx, base, channel, id, "games", Categories(),
                           REXCVAR_GET(channel_marketplace_categories_only),
                           int(REXCVAR_GET(channel_marketplace_row_games)),
                           REXCVAR_GET(channel_marketplace_pages));

  REXKRNL_WARN("[slots] '{}': {} slot(s) before, {} added, {} now", id, before, added,
               Be32(base, channel + 56));

  if (!REXCVAR_GET(channel_welcome_tab)) return;

  // The kiosk's "Welcome to Xbox 360" tab, added as its own channel rather than
  // folded into anything that already exists. The container comes off the
  // channel just finished, so this needs no seam of its own; the tab lands
  // beside Game Marketplace because that is where the list is up to.
  const uint32_t container = Be32(base, channel + 20);
  if (!container) {
    REXKRNL_WARN("[slots] no container behind channel {:#x}; skipping the Welcome tab", channel);
    return;
  }
  const uint32_t welcome =
      AddChannel(ctx, base, container, "WelcomeToXbox360", "Welcome to Xbox 360");
  if (!welcome) {
    REXKRNL_WARN("[slots] could not create the Welcome tab (channel cap is 32)");
    return;
  }
  int wadded = 0;
  for (const auto& spec : kWelcomeSlots) {
    if (AddSlot(ctx, base, welcome, spec)) ++wadded;
  }
  // Below Game Marketplace, which is the front of the list.
  MoveChannelToFront(base, container, welcome);
  REXKRNL_WARN("[slots] 'WelcomeToXbox360' created at {:#x}, {} slot(s) added, {} now, moved below "
               "Game Marketplace",
               welcome, wadded, Be32(base, welcome + 56));
}
