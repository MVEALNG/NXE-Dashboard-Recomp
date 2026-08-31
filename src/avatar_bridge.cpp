// Routing the dashboard's XamAvatar* imports to the recompiled retail xam.
//
// Why this can work here
// ----------------------
// The SDK's avatar exports are stubs. XamAvatarGetAssets in particular cannot
// be written from scratch: it builds the in-memory avatar model the dashboard's
// renderer walks -- buffer_a's +0 and +4 point at structures fed to guest
// sub_924708B8 over the 72-joint skeleton, and +4's +88 feeds sub_92477088 --
// out of the manifest's component asset ids resolved against AvatarAssetPack.
// Real xam already contains all of that.
//
// And this dashboard's xam.xex is byte-identical (md5 0d26260f...) to the one
// the avatar editor recompiles, so the same code is available here, at the same
// addresses. src/guide_bridge.cpp already loads it, relocates its dispatch
// table to 0x60000000 (0x81C00000 would alias straight through the title at
// 0x92000000) and registers its 19,880 functions. This file only resolves more
// ordinals from that same module and forwards to them.
//
// Why the dashboard is the better host for it
// -------------------------------------------
// The avatar editor has carried this bridge for far longer, but per its
// docs/avatar-xam-status.md it is stuck one step earlier: with the avatar
// service up, "the title asks for nothing" -- XamAvatarGetAssetsResultSize and
// XamAvatarGetAssets are never called. The dashboard calls both, every time it
// builds an avatar object (guest sub_92470938). So the step the editor cannot
// reach is the step this title performs already.
//
// What is deliberately conservative
// ---------------------------------
// Every forward falls back to the SDK's own export when xam is not loaded, the
// ordinal does not resolve, or the cvar is off, so a failure here degrades to
// exactly the behaviour without this file rather than taking down a dashboard
// that works. Retail xam has never run these paths in this port.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/memory.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

using namespace rex;

namespace nxe_guide {
// Defined in guide_bridge.cpp.
::PPCFunc* ResolveXamOrdinal(uint16_t ordinal);
bool XamLoaded();
}  // namespace nxe_guide

// Off by default. Turning it on loads retail xam (via guide_preload) and sends
// the avatar exports into code that has never run in this port.
REXCVAR_DEFINE_BOOL(avatar_retail_xam, false, "Avatar",
                    "Forward the XamAvatar* exports to the recompiled retail xam instead of "
                    "the SDK stubs. Requires guide_enable + guide_preload, which load it.");

// Bring xam up far enough for the avatar service, instead of running its whole
// DllMain.
//
// guide_init runs the real xstart. Measured here, it succeeds -- "XAM xstart
// ENTERED (r3=0x1 r4=0x1)" then "returned 0x1" -- and then its worker threads
// fault a few milliseconds later, reading guest 0 down
//
//   sub_816B9668 -> sub_816B92C8 -> sub_816B8B08 -> sub_8167EB60
//     -> sub_8167E918 -> sub_8167E760 -> sub_81679558 -> sub_816769D0
//
// which is xam's task system running a job queued by a subsystem the SDK also
// provides. The avatar editor reached the same conclusion from the other
// direction: xstart boots the whole system service -- device drivers, the NIC,
// secure storage, XConfig, the content manager, eight workers -- and most of it
// duplicates or fights the SDK's own xam.
//
// So run only the stages the avatar service needs, in xstart's own order.
REXCVAR_DEFINE_BOOL(avatar_xam_minimal_init, true, "Avatar",
                    "Run only the xam startup routines the avatar service needs, instead of "
                    "its whole DllMain (guide_init).");

namespace {

// xam export ordinals, from the SDK's export table
// (rexglue-sdk/src/kernel/xam/export_table.inc).
enum XamAvatarOrdinal : uint16_t {
  kXamAvatarInitialize = 0x05DC,
  kXamAvatarShutdown = 0x05DD,
  kXamAvatarGetAssetsResultSize = 0x05E0,
  kXamAvatarGetAssets = 0x05E1,
  kXamAvatarGetManifestLocalUser = 0x05E4,
  kXamAvatarManifestGetBodyType = 0x05E6,
  kXamAvatarLoadAnimation = 0x05E7,
  kXamAvatarBeginEnumAssets = 0x05E8,
  kXamAvatarEndEnumAssets = 0x05E9,
  kXamAvatarEnumAssets = 0x05EA,
  kXamAvatarGetAssetBinary = 0x05F5,
  kXamAvatarGetAssetIcon = 0x05F6,
};

struct Forward {
  uint16_t ordinal;
  const char* name;
  ::PPCFunc* fn;
};

Forward g_forwards[] = {
    {kXamAvatarInitialize, "XamAvatarInitialize", nullptr},
    {kXamAvatarGetAssetsResultSize, "XamAvatarGetAssetsResultSize", nullptr},
    {kXamAvatarGetAssets, "XamAvatarGetAssets", nullptr},
    {kXamAvatarGetManifestLocalUser, "XamAvatarGetManifestLocalUser", nullptr},
    {kXamAvatarManifestGetBodyType, "XamAvatarManifestGetBodyType", nullptr},
    {kXamAvatarBeginEnumAssets, "XamAvatarBeginEnumAssets", nullptr},
    {kXamAvatarEndEnumAssets, "XamAvatarEndEnumAssets", nullptr},
    {kXamAvatarEnumAssets, "XamAvatarEnumAssets", nullptr},
    {kXamAvatarGetAssetBinary, "XamAvatarGetAssetBinary", nullptr},
    {kXamAvatarGetAssetIcon, "XamAvatarGetAssetIcon", nullptr},
};

std::once_flag g_resolved;

// xam startup routines, at the addresses this build puts them.
constexpr uint32_t kXamCrtStartup = 0x817399A8;           // (hmodule, reason)
constexpr uint32_t kXamStoreModuleHandle = 0x816E0B58;    // (hmodule)
constexpr uint32_t kXamInitHeaps = 0x816E6618;            // ()
constexpr uint32_t kXamCreatePool = 0x816E6518;           // (id, ...)
constexpr uint32_t kXamInitStringTable = 0x8168B920;      // ()
constexpr uint32_t kXamInitNotifyQueues = 0x816FED30;     // ()
constexpr uint32_t kXamInitNotifyListeners = 0x816FF008;  // ()
constexpr uint32_t kXamInitTimerState = 0x81699DF0;       // ()
constexpr uint32_t kXamInitTaskPools = 0x816BA650;        // ()
constexpr uint32_t kXamInitContentTable = 0x816C9EA0;     // ()
constexpr uint32_t kXamInitServiceObject = 0x816A17B8;    // ()
constexpr uint32_t kXamInitServices = 0x816BDE90;         // (0)
constexpr uint32_t kXamTitlePoolSize = 16u * 1024u * 1024u;

// xam's own import slot for XboxHardwareInfo.
constexpr uint32_t kXamHardwareInfoSlot = 0x815F0480;
constexpr uint32_t kStorageFlag = 0x200;

struct InitStage {
  const char* name;
  uint32_t address;
  size_t arg_count;
  uint64_t args[6];
};

const InitStage kStages[] = {
    {"CRT startup (sub_817399A8)", kXamCrtStartup, 2, {1 /*hmodule*/, 1 /*DLL_PROCESS_ATTACH*/}},
    {"module handle (sub_816E0B58)", kXamStoreModuleHandle, 1, {1}},
    {"pool allocator (sub_816E6618)", kXamInitHeaps, 0, {}},
    {"title pool 0 (sub_816E6518)", kXamCreatePool, 6, {0, 1, kXamTitlePoolSize, 0, 2, 1}},
    {"string table (sub_8168B920)", kXamInitStringTable, 0, {}},
    {"notify queues (sub_816FED30)", kXamInitNotifyQueues, 0, {}},
    {"notify listeners (sub_816FF008)", kXamInitNotifyListeners, 0, {}},
    {"timer state (sub_81699DF0)", kXamInitTimerState, 0, {}},
    {"task pools (sub_816BA650)", kXamInitTaskPools, 0, {}},
    {"content table (sub_816C9EA0)", kXamInitContentTable, 0, {}},
    {"service object (sub_816A17B8)", kXamInitServiceObject, 0, {}},
    // Registers the services themselves. Without it the content manager
    // (service 254) is never registered, and the avatar core's request to
    // resolve "AvatarAssetPack.toc" answers 0x80070057.
    {"services (sub_816BDE90)", kXamInitServices, 1, {0}},
    {"subsystem sub_816A95A0", 0x816A95A0, 0, {}},
    {"subsystem sub_8167ED90", 0x8167ED90, 0, {}},
    {"subsystem sub_816C0698", 0x816C0698, 0, {}},
    {"subsystem sub_816A45F8", 0x816A45F8, 0, {}},
    {"subsystem sub_816DB788", 0x816DB788, 0, {}},
    {"subsystem sub_816F44D0", 0x816F44D0, 0, {}},
    {"subsystem sub_81696808", 0x81696808, 0, {}},
    {"subsystem sub_81696280", 0x81696280, 0, {}},
    {"subsystem sub_81710D18", 0x81710D18, 0, {}},
    {"subsystem sub_81778F68", 0x81778F68, 0, {}},
    {"subsystem sub_816BB468", 0x816BB468, 0, {}},
};

uint64_t CallGuest(uint32_t address, const uint64_t* args, size_t arg_count) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* thread_state = rex::runtime::ThreadState::Get();
  if (!kernel_state || !thread_state) {
    return 0;
  }
  uint64_t argv[8] = {};
  for (size_t i = 0; i < arg_count && i < 8; ++i) {
    argv[i] = args[i];
  }
  return kernel_state->function_dispatcher()->Execute(thread_state, address, argv,
                                                      arg_count > 8 ? 8 : arg_count);
}

// Tell xam that content storage exists.
//
// The content-table stage opens with
//
//     lwz r11,1152(r11)      ; xam's XboxHardwareInfo slot
//     lwz r11,0(r11)         ; the flags dword
//     rlwinm. r11,r11,0,22,22 ; bit 0x200
//     bne  ...               ; set -> build the content table
//     li r3,0 ; blr          ; clear -> return 0 and do nothing
//
// and returning 0 reads like success, which is how this hides. The SDK sets
// those flags to 0x20, so bit 0x200 has never been on and xam's content
// registry stays empty -- which is what makes the avatar core's
// "AvatarAssetPack.toc" lookup fail.
void EnableContentStorageFlag() {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    return;
  }
  auto read32 = [memory](uint32_t addr) -> uint32_t {
    auto* p = memory->TranslateVirtual<rex::be<uint32_t>*>(addr);
    return p ? static_cast<uint32_t>(*p) : 0u;
  };
  const uint32_t info = read32(kXamHardwareInfoSlot);
  if (!info) {
    REXKRNL_WARN("Avatar: xam's XboxHardwareInfo slot at {:08X} is empty; the content table "
                 "stage will do nothing",
                 kXamHardwareInfoSlot);
    return;
  }
  auto* slot = memory->TranslateVirtual<rex::be<uint32_t>*>(info);
  if (!slot) {
    return;
  }
  const uint32_t flags = static_cast<uint32_t>(*slot);
  if (!(flags & kStorageFlag)) {
    *slot = flags | kStorageFlag;
    REXKRNL_INFO("Avatar: XboxHardwareInfo at {:08X} flags {:08X} -> {:08X} (bit 0x200 = "
                 "content storage present)",
                 info, flags, flags | kStorageFlag);
  }
}

void RunMinimalInit() {
  if (!REXCVAR_GET(avatar_xam_minimal_init)) {
    return;
  }
  if (!rex::runtime::ThreadState::Get()) {
    REXKRNL_WARN("Avatar: minimal xam init needs a guest thread; skipped");
    return;
  }

  EnableContentStorageFlag();

  REXKRNL_INFO("Avatar: running minimal xam bring-up ({} stage(s))",
               static_cast<int>(std::size(kStages)));
  for (const auto& stage : kStages) {
    const uint64_t rc = CallGuest(stage.address, stage.args, stage.arg_count);
    REXKRNL_INFO("Avatar:   {} -> {:#010x}", stage.name, static_cast<uint32_t>(rc));
  }
}

void ResolveAll() {
  if (!nxe_guide::XamLoaded()) {
    REXKRNL_WARN("Avatar: retail xam is not loaded (needs guide_enable + guide_preload); "
                 "the SDK stubs stay in place");
    return;
  }
  int ready = 0;
  for (auto& forward : g_forwards) {
    forward.fn = nxe_guide::ResolveXamOrdinal(forward.ordinal);
    if (forward.fn) {
      ++ready;
    } else {
      REXKRNL_WARN("Avatar: {} ({:#06x}) did not resolve", forward.name, forward.ordinal);
    }
  }
  REXKRNL_INFO("Avatar: {} of {} retail xam avatar export(s) ready", ready,
               static_cast<int>(std::size(g_forwards)));

  // On a guest thread by construction: this runs from the first forwarded
  // avatar export, which the dashboard calls from its own thread.
  RunMinimalInit();
}

// The retail implementation for an ordinal, or null to keep the SDK's.
::PPCFunc* Retail(uint16_t ordinal) {
  if (!REXCVAR_GET(avatar_retail_xam)) {
    return nullptr;
  }
  std::call_once(g_resolved, ResolveAll);
  for (const auto& forward : g_forwards) {
    if (forward.ordinal == ordinal) {
      return forward.fn;
    }
  }
  return nullptr;
}

// The SDK's own implementation of an export, out of the runtime DLL.
//
// Not FindPPCFuncByName: the hooks below define __imp__XamAvatar* as strong
// symbols, which is what makes them win over the SDK's, and a name lookup then
// finds this file's hook rather than the implementation it is meant to fall
// back to. Reaching into the runtime's export table gets the real one. Without
// this the fallback path reported "neither a retail nor an SDK implementation"
// and XamAvatarInitialize failed even with the bridge switched off.
using GuestFunc = void (*)(PPCContext&, uint8_t*);

GuestFunc RuntimeFunc(const char* name) {
  static std::map<std::string, GuestFunc> cache;
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
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

// Forward to retail xam if it is available, otherwise to the SDK's own export.
//
// Raw hooks throughout: these take a mix of pointers and 64-bit ids, and the
// register layout has to survive untouched into xam. Passing ctx straight
// through is exactly that -- xam reads the same registers the dashboard set.
void ForwardOrFallback(uint16_t ordinal, const char* name, const char* sdk_symbol,
                       PPCContext& ctx, uint8_t* base) {
  // Which avatar entry points the dashboard actually uses, and with what.
  // Cheap enough to leave on: a full avatar build is a handful of calls.
  REXKRNL_INFO("[avatar/api] {} r3={:08X} r4={:08X} r5={:08X} r6={:08X}", name, ctx.r3.u32,
               ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  if (auto* fn = Retail(ordinal)) {
    fn(ctx, base);
    return;
  }
  if (auto* sdk = RuntimeFunc(sdk_symbol)) {
    sdk(ctx, base);
    return;
  }
  static std::once_flag once;
  std::call_once(once, [&] {
    REXKRNL_WARN("Avatar: {} has neither a retail nor an SDK implementation", name);
  });
  ctx.r3.u64 = X_ERROR_FUNCTION_FAILED;
}

}  // namespace

#define NXE_AVATAR_FORWARD(ord, sym)                                     \
  REX_HOOK_RAW(__imp__##sym) {                                           \
    ForwardOrFallback(ord, #sym, "__imp__" #sym, ctx, base);             \
  }

NXE_AVATAR_FORWARD(kXamAvatarInitialize, XamAvatarInitialize)
NXE_AVATAR_FORWARD(kXamAvatarGetAssetsResultSize, XamAvatarGetAssetsResultSize)
NXE_AVATAR_FORWARD(kXamAvatarGetAssets, XamAvatarGetAssets)
NXE_AVATAR_FORWARD(kXamAvatarGetManifestLocalUser, XamAvatarGetManifestLocalUser)
NXE_AVATAR_FORWARD(kXamAvatarManifestGetBodyType, XamAvatarManifestGetBodyType)
NXE_AVATAR_FORWARD(kXamAvatarBeginEnumAssets, XamAvatarBeginEnumAssets)
NXE_AVATAR_FORWARD(kXamAvatarEndEnumAssets, XamAvatarEndEnumAssets)
NXE_AVATAR_FORWARD(kXamAvatarEnumAssets, XamAvatarEnumAssets)
NXE_AVATAR_FORWARD(kXamAvatarGetAssetBinary, XamAvatarGetAssetBinary)
NXE_AVATAR_FORWARD(kXamAvatarGetAssetIcon, XamAvatarGetAssetIcon)
