// Reading profile settings when one of them has no answer.
//
// This is the runtime's XamUserReadProfileSettingsEx with exactly one change:
// a setting the profile does not carry is reported as absent instead of failing
// the whole call.
//
// Why that matters
// ----------------
// The Game Library was empty, and the cause was not the library. The dashboard
// builds its played-title list from a count it reads off the gamercard, at guest
// 0x921FCD50:
//
//     result = XamUserReadProfileSettings(FFFE07D1, ..., 6, dword_92015EF0, ...);
//     if ( !result )                       // ONLY on success
//     {
//         ...
//         nData = v7->pSettings[3].data.nData;   // 0x10040004, titles played
//         *(a3 + 64) = nData;
//     }
//
// and that count is the gate on the loader itself, at guest 0x921FD370:
//
//     SignalState = <the count>;
//     if ( SignalState ) { ... XamUserCreateTitlesPlayedEnumerator ... }
//
// The six settings it asks for are 10040006, 402C0011, 10040012, 10040004,
// 63E80044 and 5004000B. The profile staged now carries all six -- an earlier
// one was missing 10040012 -- but 63E80044 (the avatar manifest) is still
// withheld on purpose, because serving it drives the dashboard into the avatar
// renderer, which faults at guest 0x92471038 regardless of whether the bytes
// are genuine. See the note in profile_settings.cpp. So one of the six is
// reported absent, which is the case this file exists to handle.
//
// The runtime failed the entire read if any single setting was missing:
//
//     if (any_missing) { ... return X_ERROR_INVALID_PARAMETER; }
//
// so the gamercard read always failed, the count was never stored, the flag that
// triggers the loader was never set, and XamUserCreateTitlesPlayedEnumerator was
// never called even once -- which the logs confirm. The library was not filtering
// the games out. It was never asking for them.
//
// Why reporting it absent is the correct answer
// ---------------------------------------------
// Not a workaround: an unset setting is a normal thing for a profile to have, and
// the record format already has a way to say so. The runtime's own writer sets
//
//     out_setting->from = !setting || !setting->is_set ? 0 : ...
//
// so "no value" is expressible, and every other field is already zeroed. The
// runtime bails out before it can ever be used. Its own comment says as much:
//
//     // TODO(benvanik): don't fail? most games don't even check!
//
// The dashboard does check, and it handles the absent case deliberately. Right
// after reading the count it looks at the avatar manifest:
//
//     if ( v7->pSettings[4].data.nData == 1000 )   // a real 1000-byte manifest
//         ... adopt the avatar ...
//     else
//         memset(a3 + 132, 0, 1000);               // no avatar
//         *(a3 + 128) = 0;
//
// An absent manifest reports a length of zero, so the dashboard takes its own
// no-avatar branch. That is the outcome wanted here twice over: the gamercard
// read succeeds and the library populates, and the avatar renderer is never
// entered, so the fault at 0x92471038 is not reached. Withholding the manifest
// and succeeding the read are no longer in conflict.
//
// Everything else -- sizing, the insufficient-buffer protocol, the header and
// record layout, overlapped completion -- is the runtime's behaviour unchanged.

#include <cstdint>
#include <cstring>

#include "profile_list.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xio.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

using namespace rex;
using namespace rex::system;
using namespace rex::system::xam;

namespace {

// Defined in the runtime's xam_user.cpp, which is not a public header.
struct X_USER_READ_PROFILE_SETTINGS {
  rex::be<uint32_t> setting_count;
  rex::be<uint32_t> settings_ptr;
};
static_assert(sizeof(X_USER_READ_PROFILE_SETTINGS) == 8, "read-settings header must be 8 bytes");

uint32_t ReadProfileSettings(uint32_t title_id, uint32_t user_index, uint32_t xuid_count,
                             rex::be<uint64_t>* xuids, uint32_t setting_count,
                             rex::be<uint32_t>* setting_ids, uint32_t unk,
                             rex::be<uint32_t>* buffer_size_ptr, uint8_t* buffer,
                             uint32_t overlapped) {
  (void)title_id;
  (void)unk;

  if (xuid_count && xuids) {
    xuid_count = 1;
  }
  if (setting_count < 1 || setting_count > 32) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!buffer_size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // What is being asked for, and whether we have it.
  //
  // The gamercard shows Gamerscore 0 and Zone None even though
  // ApplyGamercardValues reports setting them, and none of the three ids appear
  // anywhere in a log -- so either the card asks for something else, or it asks
  // for these and GetSetting comes back empty. Those need telling apart before
  // anything is changed.
  {
    static int s_logged = 0;
    if (s_logged < 80 && setting_ids) {
      auto* profile = REX_KERNEL_STATE()->user_profile();
      for (uint32_t i = 0; i < setting_count && s_logged < 80; ++i, ++s_logged) {
        const uint32_t id = setting_ids[i];
        auto* setting = profile ? profile->GetSetting(id) : nullptr;
        // The value too, not just whether it exists.
        //
        // Every id the card asks for comes back "set", and the card still draws
        // zeros -- so what matters now is what is actually in them.
        std::string value = "-";
        if (auto* i32 = dynamic_cast<UserProfile::Int32Setting*>(setting)) {
          value = std::to_string(i32->value);
        } else if (auto* f32 = dynamic_cast<UserProfile::FloatSetting*>(setting)) {
          value = std::to_string(f32->value);
        }
        REXKRNL_INFO("ReadProfileSettings: user {} wants {:#010x} -> {} value={}", user_index, id,
                     !setting ? "absent" : (setting->is_set ? "set" : "present but unset"), value);
      }
    }
  }

  const auto buffer_size = static_cast<uint32_t>(*buffer_size_ptr);
  if (buffer_size && !buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto* profile_for_size = REX_KERNEL_STATE()->user_profile();

  // Size the reply from what will actually be written, not from the length
  // encoded in the setting id.
  //
  // This is the buffer overflow that made the gamer tile unusable. The runtime
  // sized the variable-length payload from Setting::Key::size -- the 12-bit
  // length carried in the setting id -- but Append writes the stored value's
  // real length:
  //
  //     UnicodeSetting::Append   size = 2 * (value.size() + 1)
  //     BinarySetting::Append    size = value.size()
  //
  // and neither is bounded by the id. A stored string longer than the id
  // declares therefore ran SettingByteStream::Advance past the end of the
  // caller's buffer, where ByteStream asserts and takes the process with it.
  //
  // That is the abort behind the blank gamerpic. Serving a real tile lets the
  // dashboard get far enough to build a gamercard, the gamercard reads profile
  // settings, and this tripped -- three runs in four with the tile enabled,
  // none with it disabled. The tile code was never at fault.
  //
  // The id's length is kept as a floor so a caller that sized its buffer the old
  // way still matches when the stored value is shorter.
  const auto payload_size = [&](uint32_t setting_id) -> size_t {
    auto* setting = profile_for_size ? profile_for_size->GetSetting(setting_id) : nullptr;
    if (!setting || !setting->is_set) {
      return 0;
    }
    if (auto* text = dynamic_cast<UserProfile::UnicodeSetting*>(setting)) {
      return text->value.empty() ? 0 : 2 * (text->value.size() + 1);
    }
    if (auto* blob = dynamic_cast<UserProfile::BinarySetting*>(setting)) {
      return blob->value.size();
    }
    return 0;
  };

  // Size the reply exactly as the runtime does: a fixed record per setting, plus
  // room after them for the payload of the variable-length kinds.
  uint32_t needed_header_size = 0;
  uint32_t needed_data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    needed_header_size += sizeof(X_USER_PROFILE_SETTING);
    UserProfile::Setting::Key setting_key;
    setting_key.value = static_cast<uint32_t>(setting_ids[i]);
    switch (static_cast<UserProfile::Setting::Type>(setting_key.type)) {
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::BINARY: {
        const size_t actual = payload_size(static_cast<uint32_t>(setting_ids[i]));
        needed_data_size += static_cast<uint32_t>(
            actual > setting_key.size ? actual : static_cast<size_t>(setting_key.size));
        break;
      }
      default:
        break;
    }
  }
  if (xuids) {
    needed_header_size *= xuid_count;
    needed_data_size *= xuid_count;
  }
  needed_header_size += sizeof(X_USER_READ_PROFILE_SETTINGS);

  const uint32_t needed_size = needed_header_size + needed_data_size;
  if (!buffer || buffer_size < needed_size) {
    if (!buffer_size) {
      *buffer_size_ptr = needed_size;
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  if (!xuids && user_index) {
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  auto* user_profile = REX_KERNEL_STATE()->user_profile();

  // The runtime returned X_ERROR_INVALID_PARAMETER here if any setting was
  // missing. Missing settings are reported per-record instead; see above.
  auto* out_header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer);
  auto* out_setting = reinterpret_cast<X_USER_PROFILE_SETTING*>(&out_header[1]);
  out_header->setting_count = setting_count;
  out_header->settings_ptr = REX_KERNEL_MEMORY()->HostToGuestVirtual(out_setting);

  UserProfile::SettingByteStream out_stream(REX_KERNEL_MEMORY()->HostToGuestVirtual(buffer), buffer,
                                            buffer_size, needed_header_size);
  uint32_t absent = 0;
  for (uint32_t n = 0; n < setting_count; ++n) {
    const uint32_t setting_id = setting_ids[n];
    auto setting = user_profile->GetSetting(setting_id);

    std::memset(out_setting, 0, sizeof(X_USER_PROFILE_SETTING));
    // from == 0 is the record's own way of saying "this one has no value".
    out_setting->from = !setting || !setting->is_set ? 0 : setting->is_title_specific() ? 2 : 1;
    if (xuids) {
      // Echo the XUID that was asked about, whatever it was.
      //
      // A caller that asks by XUID matches the records it gets back against the
      // one it asked for, so the only answer that is always right is its own.
      // UserProfile::xuid() was wrong because it is a placeholder matching
      // nothing: the gamercard asked about E030000000A8C189, got six records
      // belonging to B13EBABEBABEBABE and discarded all of them, drawing 0 G and
      // no stars from values that had been served correctly.
      //
      // Answering with our own idea of the signed-in identity instead was no
      // better, only wrong in the other direction: it is right for the caller
      // asking about you and wrong for every caller asking about somebody else,
      // and the Game Library asks about somebody else. Echoing costs nothing and
      // cannot disagree with the question.
      out_setting->xuid = static_cast<uint64_t>(xuids[0]);
    } else {
      out_setting->user_index = user_index;
    }
    out_setting->setting_id = setting_id;

    // Bounded even if the sizing above is ever wrong: a payload that will not
    // fit is reported as unset rather than written past the end of the buffer.
    // The guest already handles an unset record; it does not survive an assert.
    // The Game Library's title count comes through here: guest 0x921FCD50 reads
    // six settings in one call and keeps pSettings[3], 0x10040004, as the number
    // of played titles. That number is what the loader at 0x921FD370 asks the
    // enumerator for, so if it disagrees with what the enumerator will hand back
    // the list is wrong. Record what is served.
    if (setting_id == 0x10040004u) {
      auto* as_int = dynamic_cast<UserProfile::Int32Setting*>(setting);
      REXKRNL_INFO("ReadProfileSettings: titles-played (0x10040004) served as {} (set={})",
                   as_int ? as_int->value : -1, setting && setting->is_set);
    }

    const size_t payload = payload_size(setting_id);
    const bool fits = out_stream.offset() + payload <= buffer_size;
    if (setting && setting->is_set && fits) {
      setting->Append(&out_setting->data, &out_stream);
    } else {
      if (setting && setting->is_set && !fits) {
        out_setting->from = 0;
        REXKRNL_WARN("ReadProfileSettings: setting {:#x} needs {} byte(s) and only {} remain; "
                     "reporting it unset rather than overrunning the buffer",
                     setting_id, payload, buffer_size - out_stream.offset());
      }
      ++absent;
    }
    ++out_setting;
  }

  if (absent) {
    static uint32_t s_last = 0xFFFFFFFFu;
    if (s_last != setting_count) {
      s_last = setting_count;
      REXKRNL_INFO("ReadProfileSettings: {} of {} setting(s) absent, reported as unset", absent,
                   setting_count);
    }
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count, mapped_u64 xuids,
                                     u32 setting_count, mapped_u32 setting_ids,
                                     mapped_u32 buffer_size_ptr, mapped_void buffer_ptr,
                                     u32 overlapped) {
  return ReadProfileSettings(title_id, user_index, xuid_count, xuids.as<rex::be<uint64_t>*>(),
                             setting_count, setting_ids.as<rex::be<uint32_t>*>(), 0,
                             buffer_size_ptr.as<rex::be<uint32_t>*>(), buffer_ptr.as<uint8_t*>(),
                             overlapped);
}

u32 XamUserReadProfileSettingsEx_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                       mapped_u64 xuids, u32 setting_count, mapped_u32 setting_ids,
                                       mapped_u32 buffer_size_ptr, u32 unk_2,
                                       mapped_void buffer_ptr, u32 overlapped) {
  return ReadProfileSettings(title_id, user_index, xuid_count, xuids.as<rex::be<uint64_t>*>(),
                             setting_count, setting_ids.as<rex::be<uint32_t>*>(), unk_2,
                             buffer_size_ptr.as<rex::be<uint32_t>*>(), buffer_ptr.as<uint8_t*>(),
                             overlapped);
}

}  // namespace

REX_EXPORT(__imp__XamUserReadProfileSettings, XamUserReadProfileSettings_entry)
REX_EXPORT(__imp__XamUserReadProfileSettingsEx, XamUserReadProfileSettingsEx_entry)
