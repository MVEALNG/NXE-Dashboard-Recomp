// The Xbox 360 Guide, running as a recompiled module.
//
// Messages on the gamer blade calls XamShowMessagesUI, which is XAM's own Guide
// blade rather than a dashboard scene. This port had no XAM to show it: the
// runtime implements the XAM *API* natively, but the Guide is a UI that lives
// inside xam.xex, and the nxe_dash dump carries xam only as
// MODULE_PATCH|PATCH_DELTA -- a delta patch with no base image, which is
// unusable on its own.
//
// A complete one does exist, in the avatar editor's assets:
//
//     xam.xex   2,424,832 bytes   v2.0.17559.0   encryption=0x0000 (none)
//
// and it is self-contained in the way that matters: it imports only
// xboxkrnl.exe, and it carries its own UI resources (controlp, shrdres,
// gamercrd, skin, xam, mplayer, chnlchng) in its XEX resource table. So it can
// be recompiled and linked in beside the dashboard.
//
// That is what nxe_dash_xam.dll is. rexglue recompiles it as a DLL module
// target declared in nxe_dash_manifest.toml -- 19,880 functions -- and
// generated/$flash_dash/module_registry.cpp registers it:
//
//     kernel_state->RegisterRecompiledModule("xam.xex", "xam.xex", "nxe_dash_xam");
//
// The two images do not collide. XAM occupies 0x81670000-0x81A76A88 and the
// dashboard 0x9213D000-0x9279ECA4, so both function tables coexist in one
// address space -- measured by running the recompiled XAM standalone, which
// reported "Function table initialized for module: code=81670000-81A76A88".
//
// Reaching XamShowMessagesUI inside it
// -----------------------------------
// XAM is a system module and exports by ordinal, not by name. The ordinal is
// not guessed: the dashboard's own import thunk carries it, at guest 0x92740884
//
//     XamShowMessagesUI:
//         li    r3, 0
//         li    r4, 0x2C0        ; xam.xex :: XamShowMessagesUI
//         mtctr r11
//         bctr
//
// so 0x2C0 it is, and UserModule::GetProcAddressByOrdinal turns that into an
// address in XAM's image, which FunctionDispatcher::GetFunction turns into the
// recompiled native function.
//
// What this is honest about
// ------------------------
// This is the first time XAM code has ever run in this port, and a version
// mismatch stands behind it: the dashboard is 2.0.9199.0 and this XAM is
// 2.0.17559.0. Ordinal 0x2C0 is read from the 9199 dashboard and resolved
// against a 17559 XAM, and XAM ordinals are not guaranteed stable across
// builds. If that mapping is wrong, the address resolves to some other export.
//
// There is also a deeper unknown: the runtime already implements XAM natively,
// so the recompiled XAM's internal state -- users, notifications, content -- is
// separate from the one the rest of the dashboard has been talking to all along.
// Calling one export into a module that has not been through its own start-up is
// exactly the kind of thing that faults.
//
// So every step is guarded and logged, and any failure falls back to leaving the
// button inert, which is what it did before. A dashboard that still works is
// worth more than a Guide that takes it down, and this must not regress the
// screens that now work.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/memory.h>
#include <rex/image_info.h>
#include <rex/system/user_module.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "blade_nav.h"
#include "channel_pages.h"
#include "install_paths.h"
#include "profile_list.h"

using namespace rex;

// Off by default, and it must stay that way until XAM's start-up is understood.
//
// Entering XAM works -- it runs and returns -- but it does not leave the process
// intact. A moment after the call the dashboard dies, on threads that had nothing
// to do with it:
//
//     XamShowMessagesUI: returned from the Guide -> 0x5
//     [FATAL] Call to invalid or unregistered function at guest address 0x00000000  (t43488)
//     [FATAL] Call to invalid or unregistered function at guest address 0x00000000  (t14324)
//
// Two different threads means shared guest state, not the caller's registers --
// isolating the PPCContext and copying back only r3 did not help. XAM writes into
// an address space it believes it owns, and here it shares one with a dashboard
// that has been running for a minute already.
//
// So the wiring stays in the build, proven and measurable, and stays switched off
// unless asked for. Set guide_enable = true in nxe_dash.toml to experiment; expect
// the dashboard to fall over shortly afterwards.
// Off by default again, because pressing the button still takes the dashboard
// down and an inert button is worth more than a crash.
//
// NXE.toml cannot hold this: the dashboard rewrites it from its own cvar state
// on exit, so a hand-added guide_enable survives exactly one run and is then
// erased -- two "the Guide did not open" runs turned out to be runs where the
// setting had already been wiped. Use the command line to experiment:
//
//     NXE.exe --guide_enable=true
REXCVAR_DEFINE_BOOL(guide_enable, false, "Dashboard",
                    "Route Messages and the Guide button into the recompiled XAM Guide "
                    "(nxe_dash_xam). Runs real XAM code and currently destabilises the "
                    "dashboard; off by default.");

// Register XAM's function table BEFORE LoadUserModule, so XAM's entry point --
// its initialisation -- is dispatchable and actually runs.
//
// This is the ordering that previously ended the process with no further output.
// Separated behind its own switch so the two orders can be compared without
// rebuilding, and so the stable one stays the default.
// Resolve the Guide but never call it. Isolates "loading XAM breaks the process"
// from "calling XAM breaks the process", which the logs alone cannot separate --
// the GPU thread dies during the call, but the load happens moments earlier.
// Bisect the load. With guide_call=false the module is never executed and the
// process still dies, so the damage is in getting XAM in, not in running it.
// This splits that in half: LoadUserModule on its own, versus LoadUserModule
// followed by InitializeFunctionTable + RegisterModule.
// Second half of the bisection: create XAM's dispatch table but do not populate
// it. Separates "allocating an 8.5MB table at 0x81C00000" from "writing 19,879
// entries into it".
// Bring the Guide up during start-up rather than on first use. Tried, and it is
// worse: doing it before the dashboard's xstart runs makes the dashboard's own
// CRT init fault, dereferencing a pointer it loads from 0x92000E40. Kept as a
// switch because it isolates "before the guest runs" from "while it runs".
// Initialise a dispatch table for an unused range instead of XAM's. Answers the
// one question the logs cannot: is the dashboard broken by a second module
// existing at all, or specifically by a table over 0x81670000-0x81A76A88?
// The Guide's own look, drawn by the dashboard instead of by XAM.
//
// Running real XAM is a dead end for now -- its start-up corrupts the runtime's
// own heap, which is a different order of problem from the file and flag bugs
// that came before it. But the Guide's artwork does not need XAM: it is a XUI
// package like any other, and the dashboard has a perfectly good XUI engine that
// already opens Themes, the Gamercard and the account screens.
//
// xam.xzp was recovered out of XAM's XEX resource table (see DumpGuideResources)
// and staged beside the dashboard's own packages. It carries three scenes --
// hudbkgnd.xur, nuihud.xur and notify.xur -- of which hudbkgnd is the Guide's
// backdrop.
// A scene comes out of a XEX resource section, not out of a file on disk.
//
// The container name is turned into a locator by dash_293d, and the answer is
// not a path:
//
//     [theme] dash_293d -> locator 'section://30021000,media#'
//     [theme] dash_280c 'section://30021000,media#' + 'MessageScene.xur' -> 0x80004005
//
// so "MEDIA.xzp" became the container "media", and the dashboard's XEX has no
// such section -- its resource table holds 29, none of them media. MEDIA.xzp is
// a loose file beside the dashboard, and loose files serve images through
// file://media:/ but they are not where scenes come from. That, and not the
// XUIZ version, is why the first two attempts failed identically.
//
// The containers that do exist are the XEX's own section names:
//
//     thermal firstrun noobe controlp gamer signin slots parental homepage
//     iptv download neon signupbo dashuisk network videos pictures music
//     memory messenge accountm games gamercar dvd dashcomm dashmain consoles
//     arcade
//
// There is no Messages inbox among them, and there was never going to be: on a
// console that blade is XAM's, drawn from huduiskin's xam section, and this
// dashboard's executable simply does not carry it.
//
// Two substitutes were tried. dashcomm's MiniGamercard.xur is the Guide's own
// gamercard panel and sounded closest, but it is built to sit inside a larger
// Guide scene: on its own it draws the gamercard and an empty screen behind it.
// messenge's MessengerWelcome.xur is a complete, finished NXE screen about
// messaging, so it is the one kept -- a sign-up wizard rather than an inbox,
// but a whole screen rather than a fragment.
//
// Both values stay cvars, so any of the four hundred-odd scenes across those 29
// sections can be tried without a rebuild:
//
//     NXE.exe --guide_look_package=dashcomm.xzp --guide_look_scene=MiniGamercard.xur
//     NXE.exe --guide_look_package=gamer.xzp    --guide_look_scene=GamerCardPanelScene.xur
REXCVAR_DEFINE_STRING(guide_look_package, "messenge.xzp", "Dashboard",
                      "Package the Guide's look is drawn from. Must name a XEX resource "
                      "section, not a loose file.");
// hudbkgnd.xur was tried first and draws nothing on its own: it is the Guide's
// backdrop, the layer that sits behind the blades and dims what is under them,
// so with no blade in front of it there is nothing to see. GamerCard.xur out of
// gamercrd is a real panel with content, and the gamercard is what the Guide
// leads with.
// One scene, or several stacked into something Guide-shaped.
//
// A comma separated list, pushed in the order given. Each entry is either a
// bare scene name, taken from guide_look_locator, or "package#scene" to reach
// across packages -- the packages recovered from huduiskin each hold only one
// or two scenes, so a Guide-looking screen has to be assembled out of several.
//
// hudbkgnd is the dimming layer the blades sit on, so it belongs first.
REXCVAR_DEFINE_STRING(guide_look_scene,
                      "xam.skin.xzp#hudbkgnd.xur,gamercrd.skin.xzp#GamerCard.xur", "Dashboard",
                      "Scene, or comma separated scenes, opened when the Guide is raised.");

// Where to fall back when the loose package will not load.
//
// A section is the only thing the shell is documented to open, so if the file
// route does not work the button still lands somewhere real rather than doing
// nothing at all.
REXCVAR_DEFINE_STRING(guide_look_fallback_package, "messenge.xzp", "Dashboard",
                      "Section used when the locator above fails.");
REXCVAR_DEFINE_STRING(guide_look_fallback_scene, "MessengerWelcome.xur", "Dashboard",
                      "Scene used when the locator above fails.");

// Load a scene straight out of a loose package instead of a XEX section.
//
// dash_2a65 always builds a section:// locator from the container name, which is
// why only the 29 sections compiled into the dashboard can be reached. But the
// locator is just a string handed to dash_280c, and file:// locators demonstrably
// work elsewhere -- every sharedres image resolves through
// "file://media:/shrdres.xzp#<name>". So the same two calls dash_2a65 makes can be
// made here with a locator of our own, and a package recovered from huduiskin
// becomes loadable without being compiled into anything.
//
// Set this to use that route; empty keeps the ordinary section path.
REXCVAR_DEFINE_STRING(guide_look_locator, "file://media:/guide_res/gamercrd.skin.xzp#", "Dashboard",
                      "Locator to load the scene from. Empty uses the section path.");

// On: huduiskin can be read after all.
//
// The two failed key attempts were a red herring. Decryption was never broken --
// checked directly, the retail key reproduces the SHA-1 of the file's first
// compression block exactly as recorded in its own header. What failed was the
// check afterwards: the loader insisted on a PE image and huduiskin has none.
// It is 0xB6000 of image whose four resource sections account for 744,927 bytes
// of it, and nothing else. The runtime now accepts a resource-only XEX (see
// patches/), which is what this file is.
//
// The first attempt at this reported "XEX load failed with code 2, tried both
// encryption keys", which read as "neither key works". It was not: code 3 is a
// bad PE header and code 2 is an allocation failure, and the retry was failing
// to allocate because the first attempt had already reserved the image's memory
// and never released it. The devkit key was therefore never tried at all. With
// that fixed in xex_module.cpp, huduiskin gets a real second attempt.
REXCVAR_DEFINE_BOOL(guide_recover_packages, true, "Dashboard",
                    "Load huduiskin.xex on first use to recover its version 1 UI packages.");

// Leave XAM's dispatch table where XAM's own code looks for it.
//
// Relocating it to 0x60000000 fixed one crash and caused another. XAM's
// generated REX_LOOKUP_FUNC computes the table address from constants baked in
// at codegen time -- image_base + image_size, which for this image is
// 0x81C00000 -- so moving the table at runtime only moves it for us. The moment
// XAM makes an indirect call it reads the old address and finds nothing:
//
//     access  read
//     fault address  0x0000000181C4C950        (= 0x81C00000 + 0x4C950)
//
// which is exactly the limitation the relocation was written with, now reached
// because start-up gets as far as the storage enumeration and that calls
// indirectly.
//
// Natural placement needs the range to be genuinely free, which is what went
// wrong the first time: 0x81C00000 sits in the XEX heaps and the dashboard had
// already been allocating there for a minute by the time the Guide loaded. So
// this pairs with guide_preload -- claim the range before the dashboard grows
// into it, rather than after.
REXCVAR_DEFINE_BOOL(guide_table_natural, false, "Dashboard",
                    "Leave XAM's dispatch table at its natural address so XAM's own indirect "
                    "calls resolve. Needs guide_preload to claim the range early.");

REXCVAR_DEFINE_BOOL(guide_dummytable, false, "Dashboard",
                    "Create the second dispatch table over a dummy unused range instead of XAM's.");

// Off, and it has earned that.
//
// Preload runs the whole load/register/start-up sequence from OnPostSetup, so
// anything that fails in it takes the dashboard down before it draws -- which is
// exactly what happened: XAM's start-up tripped an assert and the process
// vanished at launch with no dashboard at all. On first use instead, a failure
// costs the Guide and nothing else.
REXCVAR_DEFINE_BOOL(guide_preload, false, "Dashboard",
                    "Load and register the Guide during start-up instead of on first use.");

REXCVAR_DEFINE_BOOL(guide_fill, true, "Dashboard",
                    "Populate XAM's dispatch table via ReXModule_Register. With it false the "
                    "table is created but left empty.");

REXCVAR_DEFINE_BOOL(guide_register, true, "Dashboard",
                    "Register XAM's function table after loading. With it false the module is "
                    "loaded but no dispatch table is created for its range.");

REXCVAR_DEFINE_BOOL(guide_call, true, "Dashboard",
                    "Actually call into the recompiled Guide. With it false the module is still "
                    "loaded and registered, which is the half being tested.");

// Write XAM's UI packages out to disk once it is loaded.
//
// The Guide's artwork does not ship loose anywhere in this dump. It lives in
// the XEX resource tables of xam.xex and $flash_huduiskin.xex, and both store
// their image LZX-compressed (FILE_FORMAT_INFO compression=0x0002), so the
// packages cannot simply be read out of the file -- which is why the shell's
// own shrdres has been short of names all along.
//
// The runtime already decompresses the image to load the module, so once XAM is
// in guest memory the packages are sitting there in the clear. This copies them
// out, and doubles as the sharpest test available of whether XAM's data mapped
// at all: a package that reads back as a valid XUIZ header proves the image is
// really there, and one that reads as zeros proves it is not -- which is the
// standing theory for why calling into XAM makes indirect calls through null.
REXCVAR_DEFINE_BOOL(guide_dump_resources, false, "Dashboard",
                    "After loading XAM, write its XUI resource packages to guide_res_dir.");
REXCVAR_DEFINE_STRING(guide_res_dir,
                      "gamedir/guide_res", "Dashboard",
                      "Where guide_dump_resources writes XAM's UI packages.");

// Run XAM's start-up, and register its table early enough that it can.
//
// Without this the module is loaded but its entry point never runs, so every
// global XAM owns is still zero when the first export is called -- which is the
// standing explanation for a cold call making an indirect call through null.
// Leaving it off means calling into a module that has never started, and that
// has never once worked; the only version of this that can succeed is the one
// where XAM has been through DLL_PROCESS_ATTACH first.
//
// With guide_preload it happens during start-up, on XAM's own guest thread, so
// initialisation is long finished before the Guide button is ever pressed --
// which also keeps it off the pad-poll thread, where running it inline
// deadlocked.
REXCVAR_DEFINE_BOOL(guide_init, true, "Dashboard",
                    "Register the recompiled XAM function table before loading the module, so "
                    "XAM's entry point runs. Requires guide_enable.");

namespace {

int GuideFaultFilter(EXCEPTION_POINTERS* ep) {
  const auto* rec = ep->ExceptionRecord;
  REXKRNL_ERROR("Guide: fault {:#010x} at rip {:#018x}",
                static_cast<uint32_t>(rec->ExceptionCode),
                static_cast<uint64_t>(ep->ContextRecord->Rip));
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    REXKRNL_ERROR("Guide:   {} of {:#018x}",
                  rec->ExceptionInformation[0] == 1 ? "write" : "read",
                  static_cast<uint64_t>(rec->ExceptionInformation[1]));
  }
  void* frames[24];
  const USHORT n = CaptureStackBackTrace(0, 24, frames, nullptr);
  for (USHORT i = 0; i < n && i < 14; ++i) {
    const auto a = reinterpret_cast<uintptr_t>(frames[i]);
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "<unknown>";
    uintptr_t mbase = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(a), &mod) &&
        mod != nullptr) {
      mbase = reinterpret_cast<uintptr_t>(mod);
      char full[MAX_PATH];
      if (GetModuleFileNameA(mod, full, MAX_PATH) != 0) {
        const char* slash = std::strrchr(full, '\\');
        std::snprintf(name, sizeof(name), "%s", slash != nullptr ? slash + 1 : full);
      }
    }
    REXKRNL_ERROR("Guide:   [{}] {} +{:#x}", i, name, mbase ? a - mbase : a);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}


// Read off the dashboard's own import thunk; see the note above.
constexpr uint16_t kOrdinalXamShowMessagesUI = 0x2C0;

// The guest path the module registry was told about.
constexpr const char* kGuidePath = "xam.xex";

// X_INPUT_GAMEPAD_GUIDE, from rex/input/input.h.
constexpr uint16_t kPadGuide = 0x0400;

enum class GuideState { kUntried, kStarting, kReady, kUnavailable };

GuideState g_state = GuideState::kUntried;
::PPCFunc* g_show_messages = nullptr;

// The loaded XAM, kept so anything else that needs a retail export can resolve
// it after start-up. The Guide only ever wanted one ordinal; the avatar bridge
// wants a dozen, and re-loading the module to get them would register its
// function table a second time.
rex::system::object_ref<rex::system::UserModule> g_xam_module;

// Load the recompiled Guide and resolve the one export we need.
//
// LoadUserModule goes through FindRecompiledModule, so "xam.xex" resolves to
// nxe_dash_xam rather than being interpreted. Its entry point is called, which
// is XAM's own initialisation -- the part most likely to fault, and the reason
// the whole thing sits behind a guard.
// XAM's own sign-in state, answered definitely.
//
// XamShowMessagesUI refuses with 5 before it draws anything, and the refusal is
// not mysterious -- it is the first gate in sub_816C1718:
//
//     r25 = 5                              // the return value, set up front
//     ...
//     sub_816DD118(user_index)             // XAM's sign-in state
//     r3 = (result == 2) ? 1 : 0           // clz/rotate idiom: "is it exactly 2"
//     if (r3 == 0) -> return 5
//
// 2 is XUSER_SIGNIN_STATE_SIGNED_IN_TO_LIVE, and sub_816DD118 gets it by reading
// XAM's own user table at 0x81A228E0 -- a global inside XAM's image that only
// XAM's start-up ever fills in. Start-up segfaults here, so the table is whatever
// the XEX shipped, and XAM concludes nobody is signed in.
//
// Rather than initialise XAM's entire user subsystem to satisfy one comparison,
// the function is replaced in the dispatch table. SetFunction overwrites the
// recompiled entry, so XAM's own code calls this instead -- the same trade this
// port makes everywhere else: answer a query definitely rather than let the guest
// branch on something undefined.
//
// This says the same thing live_signin.cpp already tells the dashboard, so the
// two halves of the process at least agree about the user.
void GuideSigninState(PPCContext& ctx, uint8_t* base) {
  (void)base;
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("Guide: XAM asked for the sign-in state of user {}; answering 2 (LIVE)",
                 ctx.r3.u32);
  }
  ctx.r3.u64 = 2;
}

// XAM 2.0.17559.0 addresses. Only valid for the module this project ships.
constexpr uint32_t kXamGetSigninState = 0x816DD118;

bool RegisterGuideCode(runtime::FunctionDispatcher* dispatcher) {
  HMODULE lib = LoadLibraryA("nxe_dash_xam.dll");
  if (lib == nullptr) {
    REXKRNL_WARN("Guide: LoadLibraryA('nxe_dash_xam.dll') failed ({})", GetLastError());
    return false;
  }

  auto get_info = reinterpret_cast<const PPCImageInfo* (*)()>(
      reinterpret_cast<void*>(GetProcAddress(lib, "ReXModule_GetImageInfo")));
  auto register_fn = reinterpret_cast<runtime::FunctionDispatcher::RegisterFn>(
      reinterpret_cast<void*>(GetProcAddress(lib, "ReXModule_Register")));
  if (!get_info || !register_fn) {
    REXKRNL_WARN("Guide: nxe_dash_xam.dll is missing ReXModule_GetImageInfo/Register");
    return false;
  }

  const PPCImageInfo* image = get_info();
  REXKRNL_INFO("Guide: image code={:#010x}-{:#010x} image={:#010x}+{:#x}", image->code_base,
               image->code_base + image->code_size, image->image_base, image->image_size);

  uint32_t code_base = image->code_base;
  uint32_t code_size = image->code_size;
  uint32_t image_base = image->image_base;
  uint32_t image_size = image->image_size;
  if (REXCVAR_GET(guide_dummytable)) {
    // Somewhere nothing else claims, small enough that the table is trivial.
    code_base = 0x40001000;
    code_size = 0x1000;
    image_base = 0x40000000;
    image_size = 0x10000;
    REXKRNL_INFO("Guide: using a DUMMY table range {:#010x}+{:#x} instead of XAM's", code_base,
                 code_size);
  } else if (REXCVAR_GET(guide_table_natural)) {
    // Exactly what the image says, so image_base + image_size lands the table on
    // the address XAM's own generated lookup was compiled to read.
    REXKRNL_INFO("Guide: table left at its natural {:#010x} so XAM's own lookups resolve",
                 image_base + image_size);
  } else {
    // XAM's real code range, with its dispatch table moved out of the way.
    //
    // This is the fix for the crash that followed every Guide call. Bisected in
    // four steps, each a separate run:
    //
    //   LoadUserModule only ................... clean, 6,186 lines
    //   + InitializeFunctionTable ............. dies, even with the table empty
    //     and XAM never called
    //   same call, dummy range 0x40001000 ..... clean, 3,244 lines
    //   XAM's code range, table at 0x60000000 . clean, 4,173 lines, XAM called twice
    //
    // So it is neither "a second module exists" nor anything XAM does when it
    // runs: it is the table landing at 0x81C00000. That address is not free in
    // the way AllocFixed believes -- it sits in the 0x8xxxxxxx XEX heaps, right
    // behind XAM's own image, in the same region the dashboard's large
    // allocations come from, and putting 8.5MB of dispatch table there left the
    // dashboard dereferencing pointers that no longer meant anything. The
    // symptom was always the same shape, a `lwz rX, 0(r11)` on a pointer loaded
    // from the dashboard's data, reached from its own start-up path.
    //
    // The table lands at image_base + image_size, so overriding those two moves
    // it into the 64KB virtual heap, clear of both images.
    //
    // Known limitation: XAM's own generated REX_LOOKUP_FUNC still computes
    // 0x81C00000 from the constants baked into it at codegen time, so an
    // indirect call made by XAM itself will not find this table. Nothing has hit
    // that yet -- the path exercised so far is all direct calls -- and the
    // alternative is the crash above.
    image_base = 0x50000000;
    image_size = 0x10000000;
    REXKRNL_INFO("Guide: XAM code {:#010x}+{:#x}, table relocated to {:#010x}", code_base,
                 code_size, image_base + image_size);
  }

  if (!dispatcher->InitializeFunctionTable(code_base, code_size, image_base, image_size,
                                           /*is_entrypoint=*/false)) {
    REXKRNL_WARN("Guide: InitializeFunctionTable failed for the XAM range");
    return false;
  }
  if (!REXCVAR_GET(guide_fill)) {
    REXKRNL_INFO("Guide: dispatch table created but left empty (guide_fill=false)");
    return false;
  }
  dispatcher->RegisterModule("xam", image->code_base, register_fn);
  REXKRNL_INFO("Guide: registered the recompiled XAM function table");

  // Applied after RegisterModule so it overwrites the recompiled entry.
  dispatcher->SetFunction(kXamGetSigninState, GuideSigninState);
  REXKRNL_INFO("Guide: sign-in state at {:#010x} answered as signed in to LIVE",
               kXamGetSigninState);
  return true;
}

// Run XAM's start-up on a guest thread of its own.
//
// It was previously run inline, via LoadUserModule(call_entry=true), which
// executes XAM's entry point on whichever thread asked for the Guide -- the
// dashboard's blade thread, already several frames deep inside a
// XamShowMessagesUI kernel call. XAM's start-up creates threads, takes kernel
// locks and then waits on its own workers, and from that context it deadlocks:
// the third ExCreateThread stops returning while one of its workers sits in
// KeWaitForSingleObject(0x81aa09b4).
//
// Two things were wrong about that, and neither is XAM's fault. The dashboard's
// thread should not be blocked for the length of XAM's start-up, and XAM's
// start-up should not run in a context that already holds kernel state it will
// contend with.
//
// So it gets what it would have on a console: an ordinary thread whose entry is
// XAM's entry point, with r3 = the module handle and r4 = 1 (DLL_PROCESS_ATTACH,
// the value xstart tests before it does any work). The dashboard carries on
// immediately either way.
//
// The system process is deliberate -- XAM's own threads are created against it
// (ExCreateThread routes creation_flags & 2 there), so its start-up belongs in
// the same process as the threads it will go on to make.
void StartXamOnItsOwnThread(uint32_t entry_point) {
  auto* kernel_state = REX_KERNEL_STATE();
  if (kernel_state == nullptr || entry_point == 0) {
    return;
  }

  // Held for the life of the process. Create() retains the thread itself, but
  // there is no reason to hand ownership back to a local that is about to go out
  // of scope while the thread is still starting.
  static rex::system::object_ref<rex::system::XThread> s_thread;

  // The title process, not the system process.
  //
  // The system process is where ExCreateThread routes XAM's own threads, and
  // running the start-up there was the obvious symmetry -- but the thread was
  // created and then never scheduled: no XThread::Execute, and none of the
  // start-up's own logging. The title process is the one this runtime actually
  // runs guest threads in.
  // Two arguments, via the XapiThreadStartup trampoline.
  //
  // xstart is a DllMain: it takes (module_handle, reason) and does nothing at all
  // unless reason == 1, DLL_PROCESS_ATTACH --
  //
  //     r31 = r3; r30 = r4;
  //     ... ; if ( r30 == 1 ) sub_816968A8(); else r3 = 1;
  //
  // A plain guest thread entry only ever receives start_context, in r3, so the
  // first attempt ran with r4 = 0 and XAM politely returned 1 without doing
  // anything. That was visible only once xstart itself was instrumented:
  //
  //     Guide: XAM xstart ENTERED (r3=0x1 r4=0x0)
  //     Guide: XAM xstart returned 0x1
  //
  // XThread already has the mechanism: when xapi_thread_startup is set it calls
  // THAT with r3 = start_address and r4 = start_context. So the entry point goes
  // in xapi_thread_startup, and the two values it wants follow as the pair.
  s_thread = rex::system::object_ref<rex::system::XThread>(new rex::system::XThread(
      kernel_state, /*stack_size=*/0x40000, /*xapi_thread_startup=*/entry_point,
      /*start_address=*/1 /* module handle, unused by xstart beyond being passed on */,
      /*start_context=*/1 /* DLL_PROCESS_ATTACH */,
      /*creation_flags=*/0, /*guest_thread=*/true, /*main_thread=*/false,
      /*guest_process=*/kernel_state->GetTitleProcess()));
  s_thread->set_name("XAM Start-up");

  const X_STATUS result = s_thread->Create();
  if (XFAILED(result)) {
    REXKRNL_WARN("Guide: could not start XAM's start-up thread ({:#x})",
                 static_cast<uint32_t>(result));
    return;
  }
  REXKRNL_INFO("Guide: XAM start-up thread created, entry {:#010x}", entry_point);
}

// XAM's resource table, read from the shipped xam.xex.
//
// These are the loaded virtual addresses and sizes straight out of the XEX's
// RESOURCE_INFO header (key 0x000002FF), so they are only valid for the exact
// xam.xex this project recompiles -- v2.0.17559.0, 2,424,832 bytes. Each entry
// is checked against a XUIZ magic before it is written, so a mismatched image
// produces a warning rather than a directory full of rubbish.
struct GuideResource {
  const char* name;
  uint32_t address;
  uint32_t size;
};

constexpr GuideResource kGuideResources[] = {
    {"FFFE07D2", 0x81B6A280, 7250},   {"fusion", 0x81B70000, 18835},
    {"controlp", 0x81B74A00, 142298}, {"chnlchng", 0x81B97600, 4180},
    {"lld2lico", 0x81B98680, 7358},   {"lld2sico", 0x81B9A380, 3262},
    {"mplayer", 0x81B9B080, 3092},    {"gamercrd", 0x81B9BD00, 32827},
    {"skin", 0x81BA3D80, 39895},      {"shrdres", 0x81BAD980, 223911},
    {"xam", 0x81BE4480, 59961},
};

void DumpGuideResources() {
  const std::string dir = REXCVAR_GET(guide_res_dir);
  if (dir.empty()) {
    return;
  }
  auto* memory = REX_KERNEL_MEMORY();
  if (memory == nullptr) {
    REXKRNL_WARN("Guide: no memory to read resources from");
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  int written = 0;
  for (const auto& res : kGuideResources) {
    auto* p = memory->TranslateVirtual(res.address);
    if (p == nullptr) {
      REXKRNL_WARN("Guide: resource '{}' at {:#010x} is not mapped", res.name, res.address);
      continue;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(p);

    // XUIZ is what a XUI package starts with. Anything else means the image is
    // not where it is expected to be, and writing it would be worse than not.
    const bool xuiz = bytes[0] == 'X' && bytes[1] == 'U' && bytes[2] == 'I' && bytes[3] == 'Z';
    if (!xuiz) {
      REXKRNL_WARN("Guide: resource '{}' does not start with XUIZ ({:02x} {:02x} {:02x} {:02x})"
                   " -- not written",
                   res.name, bytes[0], bytes[1], bytes[2], bytes[3]);
      continue;
    }

    const auto path = std::filesystem::path(dir) / (std::string(res.name) + ".xzp");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      REXKRNL_WARN("Guide: could not write '{}'", path.string());
      continue;
    }
    out.write(reinterpret_cast<const char*>(bytes), res.size);
    ++written;
    REXKRNL_INFO("Guide: wrote {} ({} bytes)", path.string(), res.size);
  }
  REXKRNL_WARN("Guide: {} of {} resource package(s) written to '{}'", written,
               static_cast<int>(sizeof(kGuideResources) / sizeof(kGuideResources[0])), dir);
}

void ResolveGuide() {
  g_state = GuideState::kUnavailable;

  if (!REXCVAR_GET(guide_enable)) {
    REXKRNL_INFO("Guide: recompiled XAM is present but switched off "
                 "(set guide_enable = true in nxe_dash.toml)");
    return;
  }

  auto* kernel_state = REX_KERNEL_STATE();
  if (kernel_state == nullptr) {
    return;
  }

  auto info = kernel_state->FindRecompiledModule(kGuidePath);
  if (!info) {
    REXKRNL_WARN("Guide: no recompiled module registered for '{}'", kGuidePath);
    return;
  }
  REXKRNL_INFO("Guide: '{}' is served by '{}'", kGuidePath, info->shared_lib_name);

  // Order matters, and both alternatives were tried.
  //
  // Registering the function table BEFORE LoadUserModule looks more correct --
  // it would give XAM's entry point somewhere to land, and XAM's entry point is
  // its initialisation. It also ends the process. With that order the log stops
  // dead:
  //
  //     Module 'xam' registered 19879 functions
  //     Guide: registered the recompiled XAM function table   <- last line, ever
  //
  // no fault caught, nothing from any other thread, with or without call_entry.
  // XAM starts up expecting to *be* the system, and this process is already a
  // dashboard with rexglue's own XAM underneath it.
  //
  // Loading first and registering after is stable, and is what actually got XAM
  // code running. The cost is that XAM's entry point is dispatched before its
  // table exists, so it does not run --
  //
  //     Execute(81696AF0): function not in function table
  //
  // -- which is harmless, and leaves XAM callable but uninitialised.
  auto* dispatcher = kernel_state->function_dispatcher();
  if (dispatcher == nullptr) {
    REXKRNL_WARN("Guide: no function dispatcher");
    return;
  }

  // With guide_init, the table goes in first so XAM's entry point can run.
  if (REXCVAR_GET(guide_init) && !RegisterGuideCode(dispatcher)) {
    return;
  }

  // Not a memory collision -- that was checked and ruled out.
  //
  // Probing 0x815F0000 before the load faults on the read itself
  // (0xC0000005 at host 0x2_815F0000, i.e. guest base + 0x815F0000), because
  // TranslateVirtual only adds the base and does not check commitment. An
  // uncommitted page there means nothing of the dashboard's lives where XAM's
  // image goes, so the fatals that follow a Guide call are not this image landing
  // on top of something.
  // The module is always loaded WITHOUT running its entry point.
  //
  // XAM's start-up is run afterwards, on a guest thread of its own -- see
  // StartXamOnItsOwnThread below. Running it inline, which is what
  // call_entry=true does, executes it on the dashboard's thread from inside a
  // XamShowMessagesUI kernel call, and it deadlocks there.
  REXKRNL_INFO("Guide: loading the module");

  rex::system::object_ref<rex::system::UserModule> module;
  __try {
    module = kernel_state->LoadUserModule(kGuidePath, /*call_entry=*/false);
  } __except (GuideFaultFilter(GetExceptionInformation())) {
    REXKRNL_ERROR("Guide: XAM's start-up faulted; see the frames above");
    return;
  }
  if (!module) {
    REXKRNL_WARN("Guide: LoadUserModule('{}') failed", kGuidePath);
    return;
  }
  REXKRNL_INFO("Guide: loaded, entry point {:#010x}", module->entry_point());
  g_xam_module = module;

  // Is XAM's image actually in guest memory? If its data never mapped, every
  // global it reads is zero, which is exactly how a cold call ends up making an
  // indirect call through a null pointer.
  if (auto* memory = REX_KERNEL_MEMORY()) {
    if (auto* p = memory->TranslateVirtual(0x81670000)) {
      const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
      REXKRNL_INFO("Guide: guest memory at 0x81670000 = {:02x} {:02x} {:02x} {:02x} "
                   "(expect non-zero PPC code)",
                   b[0], b[1], b[2], b[3]);
    } else {
      REXKRNL_WARN("Guide: 0x81670000 is not mapped -- XAM's image is not in guest memory");
    }
  }

  if (REXCVAR_GET(guide_dump_resources)) {
    DumpGuideResources();
  }

  if (!REXCVAR_GET(guide_register)) {
    REXKRNL_INFO("Guide: module loaded; function table NOT registered (guide_register=false)");
    return;
  }
  if (!REXCVAR_GET(guide_init) && !RegisterGuideCode(dispatcher)) {
    return;
  }

  // Load the recompiled code and register its functions.
  //
  // The manifest entry alone is not enough. It records the mapping --
  //
  //     Registered recompiled module: pe='xam.xex' guest='xam.xex' lib='nxe_dash_xam'
  //
  // and LoadUserModule parses the XEX so the ordinal resolves, but nothing ever
  // loaded nxe_dash_xam.dll, so not one of its 19,880 functions was dispatchable:
  //
  //     Guide: ordinal 0x2c0 -> 0x816c3de8, but no recompiled function is registered there
  //
  // The runtime has no code for this: "Loaded DLL module" exists only in
  // rexglue.exe, where it belongs to codegen loading the XEX, and none of
  // rexruntime{,d,rd}.dll contains it. The host half of the DLL-module feature is
  // not wired up in this SDK build, so it is done here with what the generated
  // module exports:
  //
  //     ReXModule_GetImageInfo() -> const PPCImageInfo*   code/image layout
  //     ReXModule_Register(IModuleRegistrar*)             19,880 SetFunction calls
  //
  // SetFunction rejects addresses outside every registered range, so XAM's range
  // needs its own function table first. is_entrypoint stays false -- the
  // dashboard already holds that role.

  const uint32_t address = module->GetProcAddressByOrdinal(kOrdinalXamShowMessagesUI);
  if (!address) {
    REXKRNL_WARN("Guide: ordinal {:#x} did not resolve -- this XAM is 2.0.17559.0 and the "
                 "ordinal was read from the 2.0.9199.0 dashboard",
                 kOrdinalXamShowMessagesUI);
    return;
  }

  g_show_messages = dispatcher->GetFunction(address);
  if (g_show_messages == nullptr) {
    REXKRNL_WARN("Guide: ordinal {:#x} -> {:#010x}, but no recompiled function is registered "
                 "there",
                 kOrdinalXamShowMessagesUI, address);
    return;
  }

  REXKRNL_INFO("Guide: XamShowMessagesUI ordinal {:#x} -> {:#010x}, recompiled function ready",
               kOrdinalXamShowMessagesUI, address);

  if (REXCVAR_GET(guide_init)) {
    // Not ready yet: the export must not be called until xstart has returned.
    StartXamOnItsOwnThread(module->entry_point());
    g_state = GuideState::kStarting;
  } else {
    g_state = GuideState::kReady;
  }
}

// Record a fault where it happens.
//
// An __except handler only sees the exception code -- by the time it runs the
// stack is gone, which is why "XAM's start-up cannot run here" never said where.
// An exception FILTER runs before unwinding, so the frames are still live and can
// be walked. Addresses are logged raw, per module, to be fed to llvm-symbolizer.
using GuestFunc = void (*)(PPCContext&, uint8_t*);

// The runtime's own implementation of a function this file also defines.
GuestFunc RuntimeFunc(const char* name) {
  static std::map<std::string, GuestFunc> cache;
  auto it = cache.find(name);
  if (it != cache.end()) {
    return it->second;
  }
  HMODULE module = GetModuleHandleA("rexruntimed.dll");
  if (module == nullptr) {
    module = GetModuleHandleA("rexruntime.dll");
  }
  GuestFunc fn = module ? reinterpret_cast<GuestFunc>(
                              reinterpret_cast<void*>(GetProcAddress(module, name)))
                        : nullptr;
  cache[name] = fn;
  return fn;
}

// XAM has finished DLL_PROCESS_ATTACH.
//
// Creating the start-up thread is not the same as having started. The run that
// first got this far shows the two in the wrong order:
//
//     Guide: XAM start-up thread created, entry 0x81696af0
//     XamShowMessagesUI(user 0): entering the recompiled Guide
//     XamShowMessagesUI: returned from the Guide -> 0x65b
//     Guide: XAM xstart ENTERED (r3=0x1 r4=0x1)
//
// -- the export was called, ran and failed before the entry point had run at
// all. Marking the Guide ready the moment the thread is *created* meant every
// press still made the cold call that guide_init was added to prevent, and
// 0x65b is what a cold XAM returns.
//
// So readiness now means started, not merely loaded. xstart tells us when it
// returns; until then the button reports that the Guide is still starting.
// Asked of nxe_dash_xam.dll, which sets it when xstart returns.
bool XamHasStarted() {
  using Query = int (*)();
  static Query query = nullptr;
  if (query == nullptr) {
    if (HMODULE m = GetModuleHandleA("nxe_dash_xam.dll")) {
      query = reinterpret_cast<Query>(reinterpret_cast<void*>(
          GetProcAddress(m, "nxe_guide_xam_started")));
    }
  }
  return query != nullptr && query() != 0;
}

// The Guide's UI package, in the format this dashboard can actually read.
//
// The first attempt staged the xam package recovered from xam.xex and the shell
// refused it outright:
//
//     [theme] navigate container='xam.xzp' scene='hudbkgnd.xur' -> 0x80004005
//
// with no file access at all. The packages do not match. Every package this
// dashboard ships is XUIZ version 1; the one out of xam.xex is version 3,
// because that XAM is 2.0.17559.0 while this dashboard is 2.0.9199.0. A version
// 3 package cannot be parsed by a version 1 reader, so the scene never loaded.
//
// The matching package is the "xam" resource inside $flash_huduiskin.xex from
// this same 9199 update -- and the shrdres beside it is 183,016 bytes, exactly
// the size of the shrdres this dashboard already loads, which is a good sign it
// is the right source. That file is both encrypted and LZX compressed, so it
// cannot be read off disk; but the runtime decrypts and decompresses a XEX in
// order to load it, so the packages can be copied straight out of guest memory
// afterwards, exactly as DumpGuideResources does for xam.xex.
//
// Addresses and sizes are from huduiskin's own RESOURCE_INFO header
// (key 0x000002FF).
struct SkinResource {
  const char* name;
  uint32_t address;
  uint32_t size;
};

constexpr SkinResource kSkinResources[] = {
    {"gamercrd", 0x91110000, 61179},
    {"xam", 0x9111EF00, 238118},
    {"shrdres", 0x91159180, 183016},
    {"skin", 0x91185C80, 262614},
};

constexpr const char* kSkinPath = "huduiskin.xex";

// Load huduiskin and write its packages out beside the dashboard's own.
//
// Done once, on the first press, and only when the file the shell needs is not
// already there -- so an ordinary run never loads a second module at all.
void EnsureGuidePackage(const std::string& package) {
  static bool s_tried = false;
  if (s_tried || !REXCVAR_GET(guide_recover_packages)) {
    return;
  }
  s_tried = true;

  const std::string dir = REXCVAR_GET(guide_res_dir);
  std::error_code ec;
  auto* kernel_state = REX_KERNEL_STATE();
  if (kernel_state == nullptr || dir.empty()) {
    return;
  }

  REXKRNL_WARN("Guide look: loading {} to recover the version 1 UI packages", kSkinPath);
  rex::system::object_ref<rex::system::UserModule> module;
  __try {
    module = kernel_state->LoadUserModule(kSkinPath, /*call_entry=*/false);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    REXKRNL_WARN("Guide look: {} faulted while loading", kSkinPath);
    return;
  }
  // A null module is not fatal here.
  //
  // LoadUserModule keeps going after ReadImage and tries to parse PE headers,
  // which a resource-only XEX does not have -- "PE signature mismatch", then
  // "Failed to load XEX PE headers!". By that point the image has already been
  // decrypted, decompressed and mapped, which is everything this needs: the
  // packages are sitting at their own addresses whether or not a module object
  // came back. So the result is noted and the memory is read regardless.
  if (!module) {
    REXKRNL_WARN("Guide look: {} produced no module object (no PE headers, as expected for a "
                 "resource-only XEX); reading its resources from memory anyway",
                 kSkinPath);
  }

  auto* memory = REX_KERNEL_MEMORY();
  if (memory == nullptr) {
    return;
  }
  std::filesystem::create_directories(dir, ec);

  for (const auto& res : kSkinResources) {
    auto* p = memory->TranslateVirtual(res.address);
    if (p == nullptr) {
      REXKRNL_WARN("Guide look: '{}' at {:#010x} is not mapped", res.name, res.address);
      continue;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(p);
    if (!(bytes[0] == 'X' && bytes[1] == 'U' && bytes[2] == 'I' && bytes[3] == 'Z')) {
      REXKRNL_WARN("Guide look: '{}' is not a XUIZ package ({:02x} {:02x} {:02x} {:02x})",
                   res.name, bytes[0], bytes[1], bytes[2], bytes[3]);
      continue;
    }
    const uint32_t version =
        (uint32_t(bytes[4]) << 24) | (bytes[5] << 16) | (bytes[6] << 8) | bytes[7];

    const auto path = std::filesystem::path(dir) / (std::string(res.name) + ".skin.xzp");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) {
      out.write(reinterpret_cast<const char*>(bytes), res.size);
      REXKRNL_WARN("Guide look: wrote {} ({} bytes, XUIZ version {})", path.string(), res.size,
                   version);
    }
  }
}

// Open a scene the way the shell opens its own.
//
// This is the same pair the disc tile uses to reach Game Details:
//
//     v6 = dash_2968(1);
//     dash_2a65(v6, L"arcade.xzp", L"TitleDetailsRomeScene.xur", args, 0, 0);
//
// dash_2968 hands back the navigator and dash_2a65 pushes a scene onto it, with
// the package in r4 and the scene file in r5. Nothing about it is specific to
// the dashboard's own packages, so a package staged beside them opens the same
// way.
extern "C" void nxe_theme_trace_scope(int delta);
extern "C" void sub_92141108(PPCContext& __restrict ctx, uint8_t* base);  // dash_2968
extern "C" void sub_921F6190(PPCContext& __restrict ctx, uint8_t* base);  // dash_2a65

// A NUL-terminated UTF-16BE string in guest memory, allocated once and kept.
//
// The strings have to live where the guest can read them, and they are small and
// fixed, so each one is allocated on first use and never freed.
uint32_t GuestString(uint8_t* base, const std::string& text) {
  static std::map<std::string, uint32_t> cache;
  auto it = cache.find(text);
  if (it != cache.end()) {
    return it->second;
  }
  auto* memory = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->memory() : nullptr;
  const uint32_t bytes = static_cast<uint32_t>((text.size() + 1) * 2);
  const uint32_t address = memory ? memory->SystemHeapAlloc(bytes) : 0;
  if (address) {
    auto* out = reinterpret_cast<rex::be<uint16_t>*>(base + address);
    for (size_t i = 0; i < text.size(); ++i) {
      out[i] = static_cast<uint16_t>(text[i]);
    }
    out[text.size()] = 0;
  }
  cache[text] = address;
  return address;
}

extern "C" void sub_9218F518(PPCContext& __restrict ctx, uint8_t* base);  // dash_280c
extern "C" void sub_9218FA60(PPCContext& __restrict ctx, uint8_t* base);  // dash_280e

// Load and show a scene from an explicit locator.
//
// This is what dash_2a65 does once dash_293d has produced its section:// string:
//
//     v11 = dash_280c(locator, scene_name, args, &scene);
//     if ( v11 >= 0 ) dash_280e(navigator, 0, scene, 255);
//
// with the locator supplied rather than derived, so a package that is a file on
// disk rather than a section in the executable can be opened.
bool ShowGuideLookFromFile(PPCContext& ctx, uint8_t* base, const std::string& locator,
                           const std::string& scene) {
  auto* memory = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->memory() : nullptr;
  const uint32_t locator_ptr = GuestString(base, locator);
  const uint32_t scene_ptr = GuestString(base, scene);
  const uint32_t out_ptr = memory ? memory->SystemHeapAlloc(4) : 0;
  if (!locator_ptr || !scene_ptr || !out_ptr) {
    REXKRNL_WARN("Guide look: no guest memory for the direct load");
    return false;
  }
  *reinterpret_cast<rex::be<uint32_t>*>(base + out_ptr) = 0;

  PPCContext load = ctx;
  load.r3.u64 = locator_ptr;
  load.r4.u64 = scene_ptr;
  load.r5.u64 = 0;  // no scene parameters
  load.r6.u64 = out_ptr;
  sub_9218F518(load, base);

  const int32_t loaded = static_cast<int32_t>(load.r3.u32);
  const uint32_t scene_object = *reinterpret_cast<const rex::be<uint32_t>*>(base + out_ptr);
  REXKRNL_WARN("Guide look: dash_280c '{}' + '{}' -> {:#x}, scene {:#x}", locator, scene,
               static_cast<uint32_t>(loaded), scene_object);
  if (loaded < 0 || !scene_object) {
    return false;
  }

  PPCContext nav = ctx;
  nav.r3.u64 = 1;
  sub_92141108(nav, base);
  const uint32_t navigator = nav.r3.u32;
  if (!navigator) {
    REXKRNL_WARN("Guide look: dash_2968 gave no navigator");
    return false;
  }

  PPCContext show = ctx;
  show.r3.u64 = navigator;
  show.r4.u64 = 0;
  show.r5.u64 = scene_object;
  show.r6.u64 = 255;
  sub_9218FA60(show, base);

  const int32_t shown = static_cast<int32_t>(show.r3.u32);
  REXKRNL_WARN("Guide look: dash_280e -> {:#x}", static_cast<uint32_t>(shown));
  return shown >= 0;
}

// True when the Guide's look was put on screen.
//
// Two routes, tried in order. The loose package is preferred because it is the
// only way to reach anything recovered from huduiskin; the section is the
// fallback, because a section is what the shell is actually built to open and
// it always works.
bool ShowGuideLook(PPCContext& ctx, uint8_t* base) {
  std::string package = REXCVAR_GET(guide_look_package);
  std::string wanted = REXCVAR_GET(guide_look_scene);
  if (package.empty() || wanted.empty()) {
    return false;
  }

  EnsureGuidePackage(package);

  // Borrow theme_trace's scene tracing for the duration, so the locator it
  // builds and the .xur it tries to load are both on the record.
  nxe_theme_trace_scope(1);

  const std::string locator = REXCVAR_GET(guide_look_locator);
  if (!locator.empty()) {
    // Split on commas and push each in turn. One that fails is reported and
    // skipped rather than abandoning the rest, because a missing backdrop is no
    // reason not to draw the panel that sits on it.
    int shown = 0;
    size_t start = 0;
    while (start <= wanted.size()) {
      const size_t comma = wanted.find(',', start);
      std::string entry = wanted.substr(start, comma == std::string::npos ? std::string::npos
                                                                          : comma - start);
      start = comma == std::string::npos ? wanted.size() + 1 : comma + 1;
      while (!entry.empty() && entry.front() == ' ') {
        entry.erase(entry.begin());
      }
      if (entry.empty()) {
        continue;
      }

      // "package#scene" reaches across packages; a bare name uses the default.
      std::string entry_locator = locator;
      std::string entry_scene = entry;
      const size_t hash = entry.find('#');
      if (hash != std::string::npos) {
        entry_locator = "file://media:/guide_res/" + entry.substr(0, hash) + "#";
        entry_scene = entry.substr(hash + 1);
      }

      if (ShowGuideLookFromFile(ctx, base, entry_locator, entry_scene)) {
        ++shown;
      } else {
        REXKRNL_WARN("Guide look: '{}' would not load from '{}'", entry_scene, entry_locator);
      }
    }

    if (shown) {
      REXKRNL_WARN("Guide look: {} scene(s) on screen", shown);
      nxe_theme_trace_scope(-1);
      return true;
    }
    REXKRNL_WARN("Guide look: nothing loaded from the packages; falling back to the section");
    package = REXCVAR_GET(guide_look_fallback_package);
    wanted = REXCVAR_GET(guide_look_fallback_scene);
  }

  const uint32_t package_ptr = GuestString(base, package);
  const uint32_t scene_ptr = GuestString(base, wanted);
  if (!package_ptr || !scene_ptr) {
    REXKRNL_WARN("Guide look: no guest memory for the scene name");
    nxe_theme_trace_scope(-1);
    return false;
  }

  // The navigator, on a context of its own so the caller's registers survive.
  PPCContext nav = ctx;
  nav.r3.u64 = 1;
  sub_92141108(nav, base);
  const uint32_t navigator = nav.r3.u32;
  if (!navigator) {
    REXKRNL_WARN("Guide look: dash_2968 gave no navigator");
    nxe_theme_trace_scope(-1);
    return false;
  }

  PPCContext go = ctx;
  go.r3.u64 = navigator;
  go.r4.u64 = package_ptr;
  go.r5.u64 = scene_ptr;
  go.r6.u64 = 0;  // no scene parameters
  go.r7.u64 = 0;
  go.r8.u64 = 0;
  sub_921F6190(go, base);

  nxe_theme_trace_scope(-1);

  const int32_t result = static_cast<int32_t>(go.r3.u32);
  REXKRNL_WARN("Guide look: '{}' from '{}' -> {:#x}", wanted, package,
               static_cast<uint32_t>(result));
  return result >= 0;
}

// Raise the Guide.
//
// Nothing else will: on a console the Xbox button never reaches the title,
// because XAM intercepts it and puts up the Guide itself. Here the dashboard is
// the only thing running, it does not watch for that button, and until now XAM
// was not present to watch either.
//
// Which blade this opens is a real limitation. XAM exports a XamShow*UI family
// that raises specific blades -- Messages is one of them -- but the Guide's root
// is raised from inside XAM's own input handling and has no export to call. So
// this opens the Messages blade, which is a Guide blade and proves the path,
// rather than the Guide root, which would need XAM's internal entry found first.
void RaiseGuide(PPCContext& ctx, uint8_t* base);

}  // namespace

namespace nxe_guide {

// Bring the Guide up during start-up, before the dashboard is running.
//
// Creating XAM's dispatch table is what destabilises the process -- bisected:
// LoadUserModule alone is clean over a 6,186-line run, and adding
// InitializeFunctionTable kills it even with the table left empty and XAM never
// called. Since the allocation itself succeeds (AllocFixed fails rather than
// overwriting, and it did not fail), the suspicion is doing it underneath a guest
// that is already live rather than the range being wrong.
//
// So it is done from OnPostSetup, after the XEX is loaded but before the title
// thread starts, which is the same window the storage device and profile mounts
// already use.
void Preload() {
  if (!REXCVAR_GET(guide_enable) || !REXCVAR_GET(guide_preload)) {
    return;
  }
  REXKRNL_INFO("Guide: preparing the Guide during start-up");
  ResolveGuide();
}

// A retail XAM export as a callable recompiled function, or null.
//
// Two things have to be true and both are checked: the XEX has to name the
// ordinal, and a recompiled function has to be registered at the address it
// names. The second is not implied by the first -- the ordinal resolves
// straight out of the XEX header, while the function only exists if
// nxe_dash_xam's table was registered and covers that address.
::PPCFunc* ResolveXamOrdinal(uint16_t ordinal) {
  if (!g_xam_module) {
    return nullptr;
  }
  auto* kernel_state = REX_KERNEL_STATE();
  if (!kernel_state) {
    return nullptr;
  }
  auto* dispatcher = kernel_state->function_dispatcher();
  if (!dispatcher) {
    return nullptr;
  }
  const uint32_t address = g_xam_module->GetProcAddressByOrdinal(ordinal);
  if (!address) {
    REXKRNL_WARN("xam: ordinal {:#06x} does not resolve in this XAM", ordinal);
    return nullptr;
  }
  auto* fn = dispatcher->GetFunction(address);
  if (!fn) {
    REXKRNL_WARN("xam: ordinal {:#06x} -> {:#010x}, but no recompiled function is registered "
                 "there",
                 ordinal, address);
    return nullptr;
  }
  REXKRNL_INFO("xam: ordinal {:#06x} -> {:#010x}, recompiled function ready", ordinal, address);
  return fn;
}

// True once XAM is loaded and its function table registered.
bool XamLoaded() { return static_cast<bool>(g_xam_module); }

}  // namespace nxe_guide

// XamInputGetState -- polled every frame, and the only place the Xbox button
// becomes visible to this port.
//
// Forwarded to the runtime first so the pad state is filled exactly as before;
// this only reads the result. XINPUT_STATE is dwPacketNumber then wButtons, so
// the buttons are a big-endian u16 at +4.
// Opening the inbox once the shell will accept a page.
//
// XamShowMessagesUI fires while a scene transition is still running -- the gamer
// blade closing over the profile section -- and 0x922C5580 will not push a page
// under one: it returns 0x8030000B. The refusal is not clean either. Whatever it
// leaves behind on the navigator breaks the next BACK, which comes back
// 0x8030000A instead of 0, and that is what stranded the profile section with no
// way back to the dashboard -- BACK returns 0 in all 622 other recorded cases,
// and 0x8030000A appeared only in runs where Messages had been pressed.
//
// A fixed delay cannot cover a transition whose length is not known, so the
// request is held and retried from the input poll -- the one thing that runs
// every frame -- until the shell takes it. Only "busy" is retried; any other
// result is final and stops at once.
constexpr int32_t kSceneBusy = int32_t(0x8030000B);
constexpr int kOpenMessagesFrames = 120;  // ~2s at 60Hz
constexpr int kOpenMessagesEvery = 15;    // 8 attempts over that, not 120
std::atomic<int> g_open_messages_for{0};

REX_HOOK_RAW(__imp__XamInputGetState) {
  const uint32_t state_ptr = ctx.r5.u32;

  if (auto* forward = RuntimeFunc("__imp__XamInputGetState")) {
    forward(ctx, base);
  }

  // The held inbox open; see XamShowMessagesUI.
  if (const int left = g_open_messages_for.load(std::memory_order_relaxed); left > 0) {
    bool done = false;
    // Spaced out on purpose. A refused push is not free -- it is the thing that
    // breaks BACK -- so this asks a handful of times across the window rather
    // than once a frame, which would repeat the damage sixty times a second.
    if (left % kOpenMessagesEvery == 0) {
      const int32_t hr =
          nxe_channels::OpenCategoryPageResult(ctx, base, "messages", "Messages");
      if (hr >= 0) {
        done = true;
      } else if (hr != kSceneBusy) {
        REXKRNL_WARN("XamShowMessagesUI: the inbox could not be opened -> {:#x}",
                     uint32_t(hr));
        done = true;
      }
    }
    if (!done && left == 1) {
      REXKRNL_WARN("XamShowMessagesUI: the shell stayed busy for {} frames; the inbox was "
                   "not opened",
                   kOpenMessagesFrames);
      done = true;
    }
    g_open_messages_for.store(done ? 0 : left - 1, std::memory_order_relaxed);
  }

  if (!state_ptr || ctx.r3.u32 != 0) {
    return;
  }

  const uint8_t* p = base + state_ptr + 4;
  const uint16_t buttons = static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);

  // Any button at all means a person is driving. The dashboard signs profiles in
  // by itself while booting, and that is indistinguishable from a selection at
  // XamUserLogon -- same flags, same XUID array, and it opens the Sign In screen
  // on its own too. Nobody has touched the pad when it does that, which is the
  // one thing that separates the two.
  if (buttons != 0) {
    nxe_profile::NotePadInput();
  }

  // Log every button transition, so a press can be correlated with whatever the
  // guest does next. Edge-triggered -- a held button is polled every frame.
  {
    static uint16_t previous = 0;
    if (buttons != previous) {
      const uint16_t pressed = static_cast<uint16_t>(buttons & ~previous);
      if (pressed) {
        REXKRNL_INFO("[pad] buttons {:#06x} pressed (state {:#06x})", pressed, buttons);
      }
      previous = buttons;
    }
  }

  // Edge-triggered: the button is held down across many polls.
  static bool s_was_down = false;
  const bool down = (buttons & kPadGuide) != 0;
  if (down && !s_was_down) {
    REXKRNL_WARN("Guide button pressed");
    RaiseGuide(ctx, base);
  }
  s_was_down = down;
}

REX_HOOK_RAW(__imp__XamShowMessagesUI) {
  const uint32_t user_index = ctx.r3.u32;

  // Unconditional, because its absence was itself the confusing part: every
  // branch below used to be silent on success, so a run where the button did
  // nothing looked exactly like a run where it was never pressed.
  REXKRNL_WARN("XamShowMessagesUI(user {}): pressed", user_index);

  // Without XAM, draw the Guide ourselves.
  //
  // This is the default path. It touches none of XAM: the dashboard's own XUI
  // engine opens a scene out of XAM's UI package, so the Guide looks like the
  // Guide and nothing runs that can take the process down. guide_enable opts
  // into the real thing instead, which still corrupts the heap during start-up.
  if (!REXCVAR_GET(guide_enable)) {
    // The inbox, which is what this item is for -- but not from here.
    //
    // Messages is case 0 of the gamer blade's dispatcher and lands in this hook.
    // The Guide blade it used to open cannot be shown, and the messages are read
    // from xblmessaging by tools/fetch_messages.py and built into a page like any
    // other category, so that page is what should open.
    //
    // Pushing it here does not work: the blade is still up at this moment and the
    // shell refuses to put a scene under it --
    //
    //     opening 'messages:Messages': nav 0x1014d, container 0x4205cc20
    //     opened  'messages:Messages': push 0x8030000b
    //
    // -- and a failed push falls through to the guide-look scenes, which is what
    // was appearing instead of the inbox. The navigator was never the problem;
    // the navigator was. The blade navigates with its own, kept at +0x10C of the
    // blade object, and the dashboard's tiles use a different one; pushing the
    // inbox onto the dashboard's is what the refusal was about. profile_ui.cpp
    // found this first for the Profile item, and Messages is the same case of
    // the same dispatcher, so it is reached the same way.
    if (nxe_channels::HasCategoryPage("messages", "Messages")) {
      const uint32_t blade_nav = nxe_blade::Navigator(base);
      if (blade_nav) {
        const int32_t hr =
            nxe_channels::OpenCategoryPageOn(ctx, base, blade_nav, "messages", "Messages");
        REXKRNL_WARN("XamShowMessagesUI(user {}): inbox on the blade navigator {:#x} -> {:#x}",
                     user_index, blade_nav, uint32_t(hr));
        if (hr >= 0) {
          ctx.r3.u64 = X_ERROR_SUCCESS;
          return;
        }
      } else {
        REXKRNL_WARN("XamShowMessagesUI(user {}): no blade dispatch on this thread; "
                     "falling back to the held open",
                     user_index);
      }
      // No blade navigator, or it would not take the page: fall back to asking
      // again from the input poll over the next couple of seconds.
      g_open_messages_for.store(kOpenMessagesFrames, std::memory_order_relaxed);
      ctx.r3.u64 = X_ERROR_SUCCESS;
      return;
    }
    REXKRNL_WARN("XamShowMessagesUI(user {}): no inbox page was built; nothing to show",
                 user_index);
    if (ShowGuideLook(ctx, base)) {
      ctx.r3.u64 = X_ERROR_SUCCESS;
      return;
    }
    static bool s_said = false;
    if (!s_said) {
      s_said = true;
      REXKRNL_WARN("XamShowMessagesUI(user {}): the Guide's look could not be opened; the "
                   "button stays inert",
                   user_index);
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
    return;
  }

  if (g_state == GuideState::kUntried) {
    ResolveGuide();
  }

  if (g_state == GuideState::kStarting) {
    if (XamHasStarted()) {
      g_state = GuideState::kReady;
      REXKRNL_WARN("Guide: XAM start-up has finished; the Guide is now callable");
    } else {
      // Calling now is the cold call that returns 0x65b. XAM is starting on its
      // own thread; a later press will find it ready.
      REXKRNL_WARN("XamShowMessagesUI(user {}): XAM is still starting up -- press again in a "
                   "moment",
                   user_index);
      ctx.r3.u64 = X_ERROR_SUCCESS;
      return;
    }
  }
  if (g_state != GuideState::kReady || g_show_messages == nullptr) {
    static bool s_said = false;
    if (!s_said) {
      s_said = true;
      REXKRNL_WARN("XamShowMessagesUI(user {}): the Guide is not available; leaving the button "
                   "inert as before",
                   user_index);
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
    return;
  }

  if (!REXCVAR_GET(guide_call)) {
    static bool s_said = false;
    if (!s_said) {
      s_said = true;
      REXKRNL_WARN("XamShowMessagesUI(user {}): loaded and resolved, not calling (guide_call=false)",
                   user_index);
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
    return;
  }

  REXKRNL_WARN("XamShowMessagesUI(user {}): entering the recompiled Guide", user_index);

  // Guarded, because this is XAM code that has never run in this port and is
  // being entered without whatever start-up it expects. A fault here would
  // otherwise take down a dashboard that works, and the screens fixed earlier in
  // this session matter more than this one button. The generated module is
  // compiled with /EHa, so a hardware fault surfaces as an SEH exception and can
  // be caught; the Guide is then marked unavailable so it is tried only once.
  // XAM runs on a context of its own, and only the return value is copied back.
  //
  // Handing it the dashboard's own context does work -- it returns 5 -- and then
  // kills the dashboard a moment later:
  //
  //     XamShowMessagesUI: returned from the Guide -> 0x5
  //     [FATAL] Call to invalid or unregistered function at guest address 0x00000000
  //
  // XAM's function runs a whole call tree through this context, setting lr and
  // the volatile registers as it goes, and the dashboard's caller then carries on
  // with registers that are no longer its own. Copying the context in and only r3
  // out leaves the caller exactly as it was.
  PPCContext guide_ctx = ctx;
  __try {
    g_show_messages(guide_ctx, base);
    ctx.r3.u64 = guide_ctx.r3.u64;
    REXKRNL_WARN("XamShowMessagesUI: returned from the Guide -> {:#x}", ctx.r3.u32);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_state = GuideState::kUnavailable;
    g_show_messages = nullptr;
    ctx.r3.u64 = X_ERROR_FUNCTION_FAILED;
    REXKRNL_ERROR("XamShowMessagesUI: the Guide faulted and has been disabled for this run. "
                  "The dashboard is unaffected.");
  }
}

namespace {

void RaiseGuide(PPCContext& ctx, uint8_t* base) {
  if (g_state == GuideState::kUntried) {
    ResolveGuide();
  }
  if (g_state != GuideState::kReady || g_show_messages == nullptr) {
    static bool s_said = false;
    if (!s_said) {
      s_said = true;
      REXKRNL_WARN("Guide button: the Guide is not available");
    }
    return;
  }

  // The pad poll's context must not be disturbed -- its caller is still using
  // r3 and the state pointer -- so the Guide runs on a context of its own.
  PPCContext guide_ctx = ctx;
  guide_ctx.r3.u64 = 0;  // user index

  REXKRNL_WARN("Guide button: entering the recompiled Guide");
  __try {
    g_show_messages(guide_ctx, base);
    REXKRNL_WARN("Guide button: returned from the Guide -> {:#x}", guide_ctx.r3.u32);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_state = GuideState::kUnavailable;
    g_show_messages = nullptr;
    REXKRNL_ERROR("Guide button: the Guide faulted and has been disabled for this run. "
                  "The dashboard is unaffected.");
  }
}

}  // namespace

// At global scope with C linkage on purpose.
//
// The first version of this sat in the anonymous namespace above, which gives
// it internal linkage -- it compiled, linked, and overrode nothing at all;
// nxe_dash.map still resolved __imp__NtProtectVirtualMemory to rexruntimed.dll.
// A kernel export can only be replaced from the global scope, the same way
// shared_resources.cpp replaces XamBuildLegacySystemResourceLocator.
extern "C" {

// XAM's start-up asks to protect devkit memory, and the runtime asserts on it.
//
// The first run that got XAM initialising reached exactly here:
//
//     Guide: XAM xstart ENTERED (r3=0x1 r4=0x1)
//     attmpted allocation to devkit memory area (debug_memory=2)
//     ... process gone, exception code 0x80000003
//
// 0x80000003 is a breakpoint, which is what assert_true compiles to, and the
// runtime has one live on this path -- xboxkrnl_memory.cpp:206
//
//     u32 NtProtectVirtualMemory_entry(base, size, protect_bits, old_protect,
//                                      u32 debug_memory) {
//       assert_true(debug_memory == 0);
//
// while the matching assert in NtAllocateVirtualMemory is commented out, which
// is why the allocation only warned and the protect killed the process.
//
// XAM was built for a devkit and asks for the debug heap; this runtime has no
// separate devkit region, so the flag is meaningless here and the allocation it
// pairs with has already been served out of normal memory. Clearing it before
// the runtime sees it keeps XAM's start-up on the one heap that exists, which is
// where its memory actually came from.
//
// Scoped to XAM: the dashboard never passes a non-zero debug_memory, so this
// changes nothing for it, and a non-zero value from anywhere else is still worth
// hearing about.
void __imp__NtProtectVirtualMemory(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.r7.u32 != 0) {
    static bool s_said = false;
    if (!s_said) {
      s_said = true;
      REXKRNL_WARN("NtProtectVirtualMemory: clearing debug_memory={} (devkit heap; this "
                   "runtime has only one)",
                   ctx.r7.u32);
    }
    ctx.r7.u64 = 0;
  }
  if (auto* fn = RuntimeFunc("__imp__NtProtectVirtualMemory")) {
    fn(ctx, base);
    return;
  }
  REXKRNL_WARN("NtProtectVirtualMemory: the runtime's own implementation was not found");
  ctx.r3.u64 = 0;
}

}  // extern "C"
