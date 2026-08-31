// XConfig: the console's persistent settings store.
//
// Screen Format could not work, and neither could any other settings screen
// that writes something back, because two of the three XConfig entry points
// ship as bare REX_EXPORT_STUB:
//
//     REX_EXPORT_STUB(__imp__ExSetXConfigSetting);
//     REX_EXPORT_STUB(__imp__ExReadModifyWriteXConfigSettingUlong);
//
// A stub logs and never assigns r3, so every write silently did nothing and
// returned garbage. ExGetXConfigSetting is implemented, but as a fixed switch
// over hardcoded constants -- there is nowhere for a written value to go, so a
// setting could never read back as anything other than its constant.
//
// The display screens use it like this (guest 0x92217B70 and 0x92217C28):
//
//     ExGetXConfigSetting(3, 0x29, &v, 4, &sz);      // or 0x2B
//     v = v & ~(3 << (2 * pack)) | (choice << (2 * pack));
//     ExSetXConfigSetting(3, 0x29, &v, 4);
//
// which is a read-modify-write of a packed 2-bit field indexed by AV pack (8
// folds to 4). Settings 0x29 and 0x2B are not in the SDK's switch at all, so
// the read failed and the function bailed before reaching the write.
//
// Worth being precise about what is and is not a guess here: the *encoding* of
// 0x29 and 0x2B belongs entirely to the guest -- it reads its own value back,
// masks it, and writes it. Nothing on this side needs to understand what the
// bits mean. What was missing is storage. Seeding them to zero is not a guessed
// value; it is the state of a console on which nothing has been chosen yet.
//
// So this replaces all three entry points with a real store:
//
//   * every value the SDK returned is seeded identically, including the two
//     that come from cvars, so nothing that worked before changes;
//   * writes are kept and read back;
//   * the store is persisted next to the storage device, because "saving a
//     setting" that does not survive a restart is not saving it. On hardware
//     this lives in NVRAM.
//
// Settings that are still unknown keep the SDK's behaviour exactly: a warning
// and X_STATUS_INVALID_PARAMETER_2. Inventing values for those is how you get a
// screen that looks right and behaves wrong, so they are left to be identified
// properly when something actually needs them.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "storage_device.h"

using namespace rex;

// Defined in the runtime; declaring them here links to the same storage, so the
// language and country seeds below track the cvars exactly as the SDK's version
// did rather than drifting to a second set of defaults.
REXCVAR_DECLARE(uint32_t, user_language);
REXCVAR_DECLARE(uint32_t, user_country);

namespace {

constexpr uint16_t kCategorySecured = 0x0002;
constexpr uint16_t kCategoryUser = 0x0003;

constexpr uint32_t Key(uint16_t category, uint16_t setting) {
  return (static_cast<uint32_t>(category) << 16) | setting;
}

std::mutex g_mutex;
std::map<uint32_t, std::vector<uint8_t>> g_store;
bool g_loaded = false;

std::vector<uint8_t> Be32(uint32_t v) {
  return {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

uint32_t ReadBe32(const std::vector<uint8_t>& v) {
  return (static_cast<uint32_t>(v[0]) << 24) | (static_cast<uint32_t>(v[1]) << 16) |
         (static_cast<uint32_t>(v[2]) << 8) | static_cast<uint32_t>(v[3]);
}

std::filesystem::path StorePath() { return nxe_storage::Root() / "xconfig.bin"; }

// Flat records: u32 key, u32 length, bytes. Written whole on every change --
// the store is a few dozen bytes and changes only when a person picks something
// on a settings screen.
void Save() {
  FILE* f = nullptr;
  if (fopen_s(&f, StorePath().string().c_str(), "wb") != 0 || f == nullptr) {
    return;
  }
  for (const auto& [key, value] : g_store) {
    const uint32_t len = static_cast<uint32_t>(value.size());
    std::fwrite(&key, sizeof(key), 1, f);
    std::fwrite(&len, sizeof(len), 1, f);
    std::fwrite(value.data(), 1, value.size(), f);
  }
  std::fclose(f);
}

void Load() {
  FILE* f = nullptr;
  if (fopen_s(&f, StorePath().string().c_str(), "rb") != 0 || f == nullptr) {
    return;
  }
  uint32_t loaded = 0;
  for (;;) {
    uint32_t key = 0;
    uint32_t len = 0;
    if (std::fread(&key, sizeof(key), 1, f) != 1) break;
    if (std::fread(&len, sizeof(len), 1, f) != 1) break;
    if (len == 0 || len > 0x1000) break;  // corrupt; stop rather than allocate wildly
    std::vector<uint8_t> value(len);
    if (std::fread(value.data(), 1, len, f) != len) break;
    g_store[key] = std::move(value);
    ++loaded;
  }
  std::fclose(f);
  if (loaded != 0) {
    REXKRNL_INFO("XConfig: restored {} saved setting(s) from {}", loaded, StorePath().string());
  }
}

// Seeds every value the SDK's switch returned, so overriding it changes nothing
// that already worked, plus storage for the two the display screens need.
void EnsureLoaded() {
  if (g_loaded) {
    return;
  }
  g_loaded = true;

  g_store[Key(kCategorySecured, 0x0002)] = Be32(0x00001000);  // AV_REGION: USA/Canada

  for (uint16_t s = 0x0001; s <= 0x0007; ++s) {
    g_store[Key(kCategoryUser, s)] = Be32(0);  // TIME_ZONE_*
  }
  g_store[Key(kCategoryUser, 0x0009)] = Be32(REXCVAR_GET(user_language));
  g_store[Key(kCategoryUser, 0x000A)] = Be32(0x00040000);  // VIDEO_FLAGS
  g_store[Key(kCategoryUser, 0x000B)] = Be32(0x00010001);  // AUDIO_FLAGS
  g_store[Key(kCategoryUser, 0x000C)] = Be32(0x40);        // RETAIL_FLAGS
  g_store[Key(kCategoryUser, 0x000E)] = {static_cast<uint8_t>(REXCVAR_GET(user_country))};
  g_store[Key(kCategoryUser, 0x0019)] = {0x03};  // PC_FLAGS

  // Per-AV-pack packed 2-bit fields written by the display screens. Zero is
  // "nothing chosen yet"; the guest owns the encoding.
  g_store[Key(kCategoryUser, 0x0029)] = Be32(0);
  g_store[Key(kCategoryUser, 0x002B)] = Be32(0);

  // Anything previously chosen wins over the seeds.
  Load();
}

u32 GetXConfigSetting_entry(u16 category, u16 setting, mapped_void buffer_ptr, u16 buffer_size,
                            mapped_u16 required_size_ptr) {
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureLoaded();

  uint16_t required = 0;
  X_RESULT result = X_STATUS_SUCCESS;

  const auto it = g_store.find(Key(category, setting));
  if (it == g_store.end()) {
    if (category != kCategorySecured && category != kCategoryUser) {
      REXKRNL_WARN("Unimplemented XConfig category 0x{:04X}", category);
      result = X_STATUS_INVALID_PARAMETER_1;
    } else {
      REXKRNL_WARN("Unimplemented XConfig {} setting 0x{:04X}",
                   category == kCategorySecured ? "SECURED" : "USER", setting);
      result = X_STATUS_INVALID_PARAMETER_2;
    }
  } else {
    const auto& value = it->second;
    required = static_cast<uint16_t>(value.size());
    auto* dst = buffer_ptr.as<uint8_t*>();
    if (dst != nullptr) {
      if (buffer_size < required) {
        result = X_STATUS_BUFFER_TOO_SMALL;
      } else {
        std::memcpy(dst, value.data(), value.size());
      }
    } else if (buffer_size != 0) {
      result = X_STATUS_INVALID_PARAMETER_3;
    }
  }

  if (required_size_ptr) {
    *required_size_ptr = required;
  }
  return result;
}

u32 SetXConfigSetting_entry(u16 category, u16 setting, mapped_void buffer_ptr, u16 buffer_size) {
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureLoaded();

  const auto* src = buffer_ptr.as<const uint8_t*>();
  if (src == nullptr || buffer_size == 0) {
    return X_STATUS_INVALID_PARAMETER_3;
  }

  // Deliberately only accept settings this store already knows about.
  //
  // Accepting anything at all is tempting -- it is what hardware does -- but it
  // is far too wide a change to make as a side effect of fixing one screen.
  // With writes open, the dashboard immediately started committing settings it
  // had never been able to commit before (category 6 blobs of up to 1628 bytes,
  // category 7, and a 0x0015 display-configuration word), taking boot down
  // paths that had never executed in this port. The result was a GPU ring
  // buffer overflow on every startup.
  //
  // So a write to an unknown setting keeps failing exactly as it did before
  // this file existed: same warning, same status. That leaves boot behaviour
  // untouched while the settings screens get real storage. Each additional
  // setting can then be enabled deliberately, once it is understood, instead of
  // a whole subsystem coming online at once.
  const uint32_t key = Key(category, setting);
  if (g_store.find(key) == g_store.end()) {
    REXKRNL_WARN("XConfig: ignoring write to unhandled category 0x{:04X} setting 0x{:04X} ({} bytes)",
                 category, setting, buffer_size);
    return X_STATUS_INVALID_PARAMETER_2;
  }

  g_store[key] = std::vector<uint8_t>(src, src + buffer_size);
  Save();

  if (buffer_size == 4) {
    REXKRNL_INFO("XConfig: set category 0x{:04X} setting 0x{:04X} = {:#010x}", category, setting,
                 ReadBe32(g_store[Key(category, setting)]));
  } else {
    REXKRNL_INFO("XConfig: set category 0x{:04X} setting 0x{:04X} ({} bytes)", category, setting,
                 buffer_size);
  }
  return X_STATUS_SUCCESS;
}

u32 ReadModifyWriteXConfigSettingUlong_entry(u32 category, u32 setting, u32 and_mask,
                                             u32 or_value) {
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureLoaded();

  const uint32_t key =
      Key(static_cast<uint16_t>(category), static_cast<uint16_t>(setting));
  const auto it = g_store.find(key);
  if (it == g_store.end() || it->second.size() != 4) {
    // Only defined for the 32-bit settings; refuse rather than fabricate one.
    REXKRNL_WARN("XConfig RMW on unknown/non-ulong setting 0x{:04X}:0x{:04X}", category, setting);
    return X_STATUS_INVALID_PARAMETER_2;
  }

  const uint32_t updated = (ReadBe32(it->second) & and_mask) | or_value;
  it->second = Be32(updated);
  Save();

  REXKRNL_INFO("XConfig: rmw category 0x{:04X} setting 0x{:04X} -> {:#010x}", category, setting,
               updated);
  return X_STATUS_SUCCESS;
}

}  // namespace

REX_EXPORT(__imp__ExGetXConfigSetting, GetXConfigSetting_entry)
REX_EXPORT(__imp__ExSetXConfigSetting, SetXConfigSetting_entry)
REX_EXPORT(__imp__ExReadModifyWriteXConfigSettingUlong, ReadModifyWriteXConfigSettingUlong_entry)
