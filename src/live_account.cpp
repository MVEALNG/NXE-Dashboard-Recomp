// Xbox LIVE logon status and account lookup, answered locally.
//
// Companion to the 0x58003 answer in xlivebase.cpp. Same trade, stated once
// more so it is not lost: there is no Xbox LIVE being reached here. This reports
// a connection that is simulated locally, at the user's direction, so the
// offline dashboard can get past a gate it would otherwise never pass.
//
// What is NOT invented: the account record itself. The profile on the storage
// device carries a real encrypted Account blob, and account_decrypt.cpp already
// recovers it with the known console key (that is where the gamertag "testing"
// comes from). The 380-byte XAMACCOUNTINFO handed out below is that record, read
// off disk, not a fabrication. Only the LIVE flags are adjusted, and the note on
// those says exactly what is changed and why.
//
// XNetLogonGetExtendedStatus
// --------------------------
// A bare REX_EXPORT_STUB, so it returned an undefined r3 and -- worse -- never
// wrote either of its two output buffers. Callers then read whatever was on
// their own stack and reported it as a logon error.
//
// The contract is from guest 0x922BEA50, read as assembly rather than inferred:
//
//     addi r4, r1, 0x70+var_20        ; a DWORD, the status
//     addi r3, r1, 0x70+var_1C        ; a 12-byte buffer
//     bl   XNetLogonGetExtendedStatus
//     lwz  r11, 0x70+var_20(r1)
//     cmplw cr6, r11, 0x80151002      ; XONLINE logon errors
//     ...              0x80151007
//
// Neither argument is a XUID, so there is no register-pair hazard here.
//
// XamProfileFindAccount / XamProfileLoadAccountInfo
// -------------------------------------------------
// Both are bare stubs, and both take a 64-bit XUID in a SINGLE register, which
// is why they are raw hooks. The runtime's typed marshaller reads the low 32
// bits of one register per parameter and cannot express that -- the same
// limitation that put a count into a pointer parameter earlier in this port. The
// call site at guest 0x922542A4 settles it, and note the "ld" (64-bit load):
//
//     addi r29, r31, 0x1B0            ; handle out
//     addi r30, r31, 0x34             ; account info out
//     mr   r5, r29
//     mr   r4, r30
//     ld   r3, 0(r11)                 ; XUID, 64-bit, one register
//     bl   XamProfileFindAccount
//     cmplwi r3, 0
//     bne  fail                       ; ZERO is success
//
//     lwz  r3, 0(r29)                 ; the handle FindAccount wrote
//     mr   r5, r30                    ; account info out
//     ld   r4, 0(r11)                 ; XUID again, 64-bit
//     bl   XamProfileLoadAccountInfo
//     mr.  r26, r3
//     bge  ok                         ; >= 0 is success
//
// So FindAccount(xuid, info_out, handle_out) -> 0 on success, and
// LoadAccountInfo(handle, xuid, info_out) -> >= 0 on success.
//
// The flags byte the caller acts on
// ---------------------------------
// Immediately after loading, the guest branches on the first DWORD of the
// record:
//
//     if ( (a1[13] & 0x10000000) != 0 )   // reserved_flags, password-protected
//         ... prompt for the account password ...
//     else
//         ... continue straight in ...
//
// a1[13] is a1 + 0x34, which is exactly the buffer just filled. 0x10000000 is
// the password-protected bit. It is cleared here, and the LIVE-enabled bit set,
// because a password prompt cannot be answered against an account that is not
// really being authenticated -- there is nothing to check a password with. Those
// two bits are the only part of the record that is not served verbatim.

#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

using namespace rex;

namespace nxe_profile {
const std::vector<uint8_t>& AccountBlob();
}

namespace {

// The status block the caller supplies alongside the DWORD.
constexpr size_t kExtendedStatusBytes = 12;

// XAMACCOUNTINFO, as the guest expects it.
constexpr size_t kAccountInfoBytes = 380;

// reserved_flags, the first DWORD of the record.
constexpr uint32_t kAccountPasswordProtected = 0x10000000;
constexpr uint32_t kAccountLiveEnabled = 0x20000000;

// A non-zero handle for the account. The guest only carries it from
// FindAccount to LoadAccountInfo; it never interprets it.
constexpr uint32_t kAccountHandle = 1;

// XC_COUNTRY_UNITED_STATES. Only the low byte is consulted by the caller.
constexpr uint32_t kCountryUnitedStates = 103;

void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// The real record from the profile, with only the two flag bits adjusted.
bool WriteAccountInfo(uint8_t* dst) {
  const auto& blob = nxe_profile::AccountBlob();
  if (blob.size() < kAccountInfoBytes) {
    return false;
  }
  std::memcpy(dst, blob.data(), kAccountInfoBytes);

  uint32_t reserved = LoadBe32(dst);
  reserved &= ~kAccountPasswordProtected;  // nothing here can verify a password
  reserved |= kAccountLiveEnabled;
  StoreBe32(dst, reserved);
  return true;
}

u32 XNetLogonGetExtendedStatus_entry(mapped_void status_block, mapped_u32 status_out) {
  if (auto* block = status_block.as<uint8_t*>()) {
    std::memset(block, 0, kExtendedStatusBytes);
  }
  if (status_out) {
    *status_out = 0;  // no logon error
  }

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XNetLogonGetExtendedStatus -> no logon error (LIVE simulated locally)");
  }
  return X_ERROR_SUCCESS;
}

}  // namespace

// Raw hooks: the XUID is 64 bits in one register. See the note above.
REX_HOOK_RAW(__imp__XamProfileFindAccount) {
  const uint64_t xuid = ctx.r3.u64;
  const uint32_t info_out = ctx.r4.u32;
  const uint32_t handle_out = ctx.r5.u32;

  if (!handle_out || nxe_profile::AccountBlob().size() < kAccountInfoBytes) {
    ctx.r3.u64 = X_ERROR_FUNCTION_FAILED;
    return;
  }

  if (info_out) {
    WriteAccountInfo(base + info_out);
  }
  StoreBe32(base + handle_out, kAccountHandle);

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamProfileFindAccount({:#x}) -> account found", xuid);
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;  // zero is success here
}

REX_HOOK_RAW(__imp__XamProfileLoadAccountInfo) {
  const uint32_t handle = ctx.r3.u32;
  const uint64_t xuid = ctx.r4.u64;
  const uint32_t info_out = ctx.r5.u32;

  if (!info_out || !WriteAccountInfo(base + info_out)) {
    ctx.r3.u64 = X_ERROR_FUNCTION_FAILED;
    return;
  }

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamProfileLoadAccountInfo(handle {}, xuid {:#x}) -> {} byte record", handle, xuid,
                 kAccountInfoBytes);
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamUserGetOnlineCountryFromXUID -- also a 64-bit XUID in one register:
//
//     ld   r3, 0(r11)
//     bl   XamUserGetOnlineCountryFromXUID
//     mr   r29, r3                          ; the country id is the return value
//
// The account screen feeds the result straight to guest 0x922F2008, which only
// rejects a handful of countries per language (101 and 39, or 53 and 56 under
// language 8) and passes everything else. Undefined r3 therefore sometimes
// landed on a rejected country and sometimes on nonsense; the point of answering
// is that it is now the same value every run. 103 is the United States, which is
// consistent with the console locale this port already reports elsewhere.
REX_HOOK_RAW(__imp__XamUserGetOnlineCountryFromXUID) {
  (void)base;
  const uint64_t xuid = ctx.r3.u64;

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamUserGetOnlineCountryFromXUID({:#x}) -> {}", xuid, kCountryUnitedStates);
  }
  ctx.r3.u64 = kCountryUnitedStates;
}

namespace {

// The system UI is not being driven by a title here -- this dashboard IS the
// title, and there is no system UI layer above it.
u32 XamIsSysUiInvokedByTitle_entry() { return 0; }

}  // namespace

REX_EXPORT(__imp__XamIsSysUiInvokedByTitle, XamIsSysUiInvokedByTitle_entry)
REX_EXPORT(__imp__XNetLogonGetExtendedStatus, XNetLogonGetExtendedStatus_entry)
