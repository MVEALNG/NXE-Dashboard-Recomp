// Dates and times, formatted from the whole timestamp.
//
// The game detail panel showed a "last played" line reading
//
//     00/40719/32766
//
// with the middle number different on every screenshot -- 2047, then 16191, then
// 40719 -- against a fixed 32766. Month 00, day 40719, year 32766 is a
// SYSTEMTIME that was never filled in, and the reason is the argument, not the
// formatter.
//
// The runtime declares it as a typed hook:
//
//     void XamFormatDateString_entry(u32 unk, u64 filetime, mapped_void out,
//                                    u32 out_count)
//
// and that u64 cannot survive. The runtime's ArgTranslator reads the low 32 bits
// of one register per parameter no matter what the parameter is declared as --
// the same limitation that truncated a XUID to 0xBABEBABE for XamProfileOpen and
// put a count into a pointer parameter for XamUserCreateTitlesPlayedEnumerator.
// So the FILETIME arrived with its top half gone, which lands far outside any
// representable date, and the SYSTEMTIME conversion left the struct untouched.
//
// The call site settles that the whole 64 bits really are in one register, at
// guest 0x922E7214:
//
//     li   r6, 0x104                    ; output_count, 260 chars
//     addi r5, r1, 0x4C0+var_460        ; output buffer
//     ld   r4, 0x4C0+var_468(r1)        ; FILETIME -- ld, 64-bit, one register
//     mr   r3, r26                      ; user index
//     bl   XamFormatDateString
//
// So these are raw hooks that read ctx.r4.u64. Everything else is the runtime's
// behaviour kept as it was: the buffer is cleared first, the output is UTF-16
// big-endian, and the formats are the same "MM/DD/YYYY" and "HH:MM" it used.
//
// A zero or unrepresentable timestamp now leaves the buffer empty rather than
// printing nonsense -- a blank date is honest about not knowing, where
// 00/40719/32766 was not.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace rex;

namespace {

// The guest reads UTF-16 big-endian.
void WriteWideBe(uint8_t* dst, uint32_t capacity_chars, const std::string& text) {
  std::memset(dst, 0, size_t(capacity_chars) * sizeof(uint16_t));
  if (capacity_chars == 0) {
    return;
  }
  const size_t count = text.size() < size_t(capacity_chars) - 1 ? text.size() : capacity_chars - 1;
  for (size_t i = 0; i < count; ++i) {
    dst[i * 2] = 0;
    dst[i * 2 + 1] = static_cast<uint8_t>(text[i]);
  }
}

// FILETIME -> local SYSTEMTIME, false when the value cannot be a date.
bool LocalTimeFrom(uint64_t filetime, SYSTEMTIME* out) {
  if (filetime == 0) {
    return false;
  }
  FILETIME ft;
  ft.dwLowDateTime = static_cast<DWORD>(filetime & 0xFFFFFFFFull);
  ft.dwHighDateTime = static_cast<DWORD>(filetime >> 32);

  SYSTEMTIME utc;
  if (!FileTimeToSystemTime(&ft, &utc)) {
    return false;
  }
  if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, out)) {
    *out = utc;
  }
  return true;
}

void FormatInto(PPCContext& ctx, uint8_t* base, bool date) {
  const uint64_t filetime = ctx.r4.u64;  // the whole thing; see the note above
  const uint32_t out_addr = ctx.r5.u32;
  const uint32_t out_count = ctx.r6.u32;
  if (!out_addr || !out_count) {
    return;
  }
  uint8_t* dst = base + out_addr;

  SYSTEMTIME st{};
  if (!LocalTimeFrom(filetime, &st)) {
    std::memset(dst, 0, size_t(out_count) * sizeof(uint16_t));
    static bool s_warned = false;
    if (!s_warned) {
      s_warned = true;
      REXKRNL_WARN("XamFormat{}String: {:#x} is not a representable time; leaving it blank",
                   date ? "Date" : "Time", filetime);
    }
    return;
  }

  char text[32] = {};
  if (date) {
    std::snprintf(text, sizeof(text), "%02u/%02u/%u", st.wMonth, st.wDay, st.wYear);
  } else {
    std::snprintf(text, sizeof(text), "%02u:%02u", st.wHour, st.wMinute);
  }
  WriteWideBe(dst, out_count, text);

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    REXKRNL_INFO("XamFormatDateString({:#x}) -> {}", filetime, text);
  }
}

}  // namespace

REX_HOOK_RAW(__imp__XamFormatDateString) { FormatInto(ctx, base, true); }
REX_HOOK_RAW(__imp__XamFormatTimeString) { FormatInto(ctx, base, false); }
