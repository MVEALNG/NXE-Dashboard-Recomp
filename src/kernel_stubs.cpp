// Kernel imports the NXE dashboard needs that the SDK does not implement.
//
// Two distinct gaps, both found at link time:
//
//   1. XUsbcam* (8 functions) -- the Xbox Live Vision camera API. The SDK's
//      xboxkrnl export table DECLARES these (ordinals 0x21A-0x223) but ships no
//      bodies, so the generated import thunks referenced undefined symbols.
//      The dashboard queries the camera during startup; with no camera attached
//      the honest answer is "not connected", which is also what this build can
//      truthfully report. Reporting absence is correct behaviour here, not a
//      placeholder to revisit.
//
//   2. sub_92740DF4 -- the import thunk for xam ordinal 684 (0x2AC), which is
//      absent from the SDK's xam export table (0x2A8-0x2AB and 0x2AD are all
//      present; 684 is skipped). Codegen reported "Cannot resolve ordinal 684
//      from xam" and emitted the thunk unnamed. Its true identity is unknown, so
//      it returns 0 and logs; if the dashboard actually depends on it, the
//      warning will say so.
//
// These are guest-ABI entry points (DECLARE_REX_FUNC), so they take the PPC
// context rather than typed C arguments -- no signature guessing required.

#include <rex/hook.h>

// ERROR_DEVICE_NOT_CONNECTED
static constexpr uint32_t kDeviceNotConnected = 0x0000048Fu;

// No camera present. GetState reports 0 (no state bits set) rather than an
// error code, matching how a state query differs from an operation.
// Not a stub, and not a behaviour change: the camera state is still 0, which is
// the right answer for a port with no camera. It is a real export purely to stop
// the logging. The dashboard polls this every frame, and REX_EXPORT_STUB_RETURN
// writes a warning on every call -- 9,019 lines in one run, enough to push the
// log past its 5MB rotation and split a session in two, which cost real time
// while chasing the theme screen. The theme scene's vision-effects branch at
// guest 0x922E7810 keys off this value, so it stays exactly as it was.
static rex::u32 XUsbcamGetState_entry() { return 0u; }
REX_EXPORT(__imp__XUsbcamGetState, XUsbcamGetState_entry)

REX_EXPORT_STUB_RETURN(__imp__XUsbcamCreate, kDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamDestroy, kDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamGetConfig, kDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetConfig, kDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetView, kDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetCaptureMode, kDeviceNotConnected)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamReadFrame, kDeviceNotConnected)

// xam ordinal 684 -- identity unknown, see above.
REX_STUB_RETURN(sub_92740DF4, 0u)

// XamProfileEnumerate: the SDK stubs this with REX_EXPORT_STUB, which only logs
// and never assigns r3 -- so the guest reads an undefined result and never sees
// a terminating condition. The dashboard spun on it 59,372 times in half a
// second (rotating ~57 MB of logs) before dying.
//
// There are no profiles staged in this game directory, and ERROR_NO_MORE_FILES
// is the documented way an Xbox enumerator reports exhaustion, so this is the
// truthful answer rather than a placeholder.
static constexpr uint32_t kNoMoreFiles = 0x00000103u;  // ERROR_NO_MORE_FILES
// XamProfileEnumerate is now implemented in user_profile.cpp, which
// enumerates the local profile instead of reporting none.

// ---------------------------------------------------------------------------
// User profile / Live account queries.
//
// All of these ship as bare REX_EXPORT_STUB in the SDK, which logs a warning and
// never assigns r3 -- so the guest reads an undefined result. That is the same
// defect that made XamProfileEnumerate spin above. The dashboard polls this set
// every 5 seconds waiting for a coherent account state and never advances past
// its sign-in gate.
//
// The values below describe an offline console with a local profile and no Xbox
// Live subscription, which is what this build can honestly represent: no network
// stack, no Live services, no account data staged. They are deliberate answers,
// not placeholders.
static constexpr uint32_t kErrorNotFound   = 0x00000490u;  // ERROR_NOT_FOUND
static constexpr uint32_t kNoSubscription  = 0u;           // no Live subscription
static constexpr uint32_t kBaseTier        = 0u;           // base membership tier
static constexpr uint32_t kNoTenure        = 0u;           // account age, in years

REX_EXPORT_STUB_RETURN(__imp__XamUserGetUserTenure, kNoTenure)
REX_EXPORT_STUB_RETURN(__imp__XamUserGetSubscriptionType, kNoSubscription)
REX_EXPORT_STUB_RETURN(__imp__XamUserGetMembershipTierFromXUID, kBaseTier)

// No Live hive is staged, so every lookup misses and the caller uses its
// built-in default rather than waiting on a value that will never arrive.
REX_EXPORT_STUB_RETURN(__imp__XamGetLiveHiveValueW, kErrorNotFound)

// Matches the XamProfileEnumerate answer above: there are no profiles to
// enumerate, so report exhaustion instead of handing back an undefined handle.
// XamProfileCreateEnumerator likewise moved to user_profile.cpp.

// ---------------------------------------------------------------------------
// Device / dashboard-context queries.
//
// Another set of bare REX_EXPORT_STUBs that never assign r3, so the guest reads
// an undefined result. XamContentDeviceCheckUpdates is the likely source of the
// on-screen "Device Detected - Update required. Insert the disc that came with
// your device." dialog: asked whether a connected device needs updating, a
// garbage non-zero answer means "yes".
//
// This build has no attached storage device pending an update, no OOBE to run,
// and no Omni configuration, so each answers accordingly.
static constexpr uint32_t kNoUpdatesRequired = 0u;
static constexpr uint32_t kNoConfigNeeded    = 0u;
static constexpr uint32_t kDoNotRunFirstRun  = 0u;
static constexpr uint32_t kOk                = 0u;

REX_EXPORT_STUB_RETURN(__imp__XamContentDeviceCheckUpdates, kNoUpdatesRequired)
REX_EXPORT_STUB_RETURN(__imp__XamDoesOmniNeedConfiguration, kNoConfigNeeded)
REX_EXPORT_STUB_RETURN(__imp__XamFirstRunExperienceShouldRun, kDoNotRunFirstRun)
REX_EXPORT_STUB_RETURN(__imp__XamSetDashContext, kOk)
REX_EXPORT_STUB_RETURN(__imp__XamGetDashContext, kOk)

// XamUserGetIndexFromXUID: maps a XUID to the controller slot that user is
// signed in on. Another bare REX_EXPORT_STUB with no assignment to r3.
//
// It is the last meaningful query the dashboard makes before terminating: after
// two rounds of signature verification it asks this twice, reads undefined
// garbage, and exits (code 3, ~1.5s after start). This build has a single
// profile occupying slot 0, so that is the answer.
static constexpr uint32_t kUserIndexZero = 0u;
REX_EXPORT_STUB_RETURN(__imp__XamUserGetIndexFromXUID, kUserIndexZero)

