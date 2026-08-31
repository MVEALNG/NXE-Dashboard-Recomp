// Recovering the gamertag from the profile's encrypted Account blob.
//
// The runtime has no account decryption and no host-side crypto, so the
// gamertag was unreachable and every identity API reported UserProfile's
// hardcoded placeholder ("User", xuid 0xB13EBABEBABEBABE). The gamertag is the
// one thing that makes a profile recognisably someone's, so it is worth the
// self-contained SHA-1 / HMAC / RC4 below.
//
// The scheme is the standard one for a 404-byte Account file:
//
//     confounder = account[0..16]
//     rc4_key    = HMAC_SHA1(console_key, confounder)[0..16]
//     plaintext  = RC4(rc4_key, account[16..404])
//
// On trusting the key
// -------------------
// The console key is a published constant, and one this machine has no way to
// verify against a reference. That would normally make it a guess of exactly the
// kind that has misfired repeatedly in this port -- except that this particular
// guess checks itself. A correct decrypt of a mostly-empty account yields almost
// all zero bytes and readable UTF-16; a wrong key yields high-entropy noise. The
// two candidate keys separate cleanly on that test:
//
//     retail   367 of 388 bytes zero, UTF-16 "testing", ASCII "Velocity"/"PROD"
//     devkit     1 of 388 bytes zero, no readable runs
//     (raw)      2 of 404 bytes zero
//
// so the key below is not taken on faith, and VerifyPlaintext re-applies the
// same test at runtime. If a future account fails it, the gamertag is simply not
// reported rather than a garbage string being shown.
//
// On the gamertag offset
// ----------------------
// XAMACCOUNTINFO places szGamerTag at +0x08, but in this file the tag sits at
// +0x10, with the editor that produced it ("Velocity") having written its own
// signature across the leading reserved fields. Rather than hardcode either
// offset, the tag is located by scanning for the first plausible UTF-16 run,
// which tolerates both layouts.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "profile_list.h"
#include "storage_device.h"

namespace nxe_profile {

// Defined in profile_settings.cpp: the staged profile directory, honouring the
// profile_xuid selection.
const std::filesystem::path& ProfileDirectory();

namespace {

// Published retail console key. See the note above on why this is testable
// rather than trusted.
constexpr uint8_t kConsoleKey[16] = {0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1, 0xEA, 0xE9,
                                     0xAB, 0x31, 0x70, 0xF3, 0xAD, 0x47, 0xEB, 0xF3};

constexpr size_t kAccountSize = 404;

// XAMACCOUNTINFO does not start at the beginning of the decrypted blob; there
// are 8 bytes in front of it. See the note on AccountInfoOf for how that is
// pinned down. 388 decrypted bytes minus those 8 is 380, the size of the
// account_info field in _PROFILEENUMRESULT.
constexpr size_t kAccountInfoOffset = 8;
constexpr size_t kAccountInfoSize = 380;
constexpr size_t kConfounderSize = 16;
constexpr size_t kGamerTagMaxChars = 16;

//=============================================================================
// SHA-1 / HMAC-SHA1 / RC4
//=============================================================================

struct Sha1 {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint8_t block[64] = {};
  size_t block_len = 0;
  uint64_t total = 0;

  static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

  void Compress(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
             (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t t = Rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = Rol(b, 30);
      b = a;
      a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }

  void Update(const uint8_t* data, size_t len) {
    total += len;
    while (len > 0) {
      const size_t take = std::min(len, sizeof(block) - block_len);
      std::memcpy(block + block_len, data, take);
      block_len += take;
      data += take;
      len -= take;
      if (block_len == sizeof(block)) {
        Compress(block);
        block_len = 0;
      }
    }
  }

  void Final(uint8_t out[20]) {
    const uint64_t bits = total * 8;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (block_len != 56) {
      Update(&zero, 1);
    }
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) {
      len_be[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    }
    Update(len_be, 8);
    for (int i = 0; i < 5; ++i) {
      out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
  }
};

void HmacSha1(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len,
              uint8_t out[20]) {
  uint8_t k[64] = {};
  if (key_len > 64) {
    Sha1 s;
    s.Update(key, key_len);
    s.Final(k);
  } else {
    std::memcpy(k, key, key_len);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5C;
  }
  uint8_t inner[20];
  {
    Sha1 s;
    s.Update(ipad, 64);
    s.Update(data, data_len);
    s.Final(inner);
  }
  Sha1 s;
  s.Update(opad, 64);
  s.Update(inner, 20);
  s.Final(out);
}

void Rc4(const uint8_t* key, size_t key_len, uint8_t* data, size_t len) {
  uint8_t s[256];
  for (int i = 0; i < 256; ++i) {
    s[i] = static_cast<uint8_t>(i);
  }
  for (int i = 0, j = 0; i < 256; ++i) {
    j = (j + s[i] + key[i % key_len]) & 0xFF;
    std::swap(s[i], s[j]);
  }
  int i = 0, j = 0;
  for (size_t n = 0; n < len; ++n) {
    i = (i + 1) & 0xFF;
    j = (j + s[i]) & 0xFF;
    std::swap(s[i], s[j]);
    data[n] ^= s[(s[i] + s[j]) & 0xFF];
  }
}

//=============================================================================

// A correctly decrypted account is overwhelmingly zero. Noise is not.
bool VerifyPlaintext(const std::vector<uint8_t>& plain) {
  size_t zeros = 0;
  for (uint8_t b : plain) {
    if (b == 0) ++zeros;
  }
  return plain.size() > 64 && zeros * 2 > plain.size();
}

// First plausible UTF-16BE run. Tolerates the tag sitting at +0x08 or +0x10.
std::string FindGamerTag(const std::vector<uint8_t>& plain) {
  for (size_t off = 0; off + 4 < plain.size(); off += 2) {
    if (plain[off] != 0 || plain[off + 1] < 0x20 || plain[off + 1] >= 0x7F) {
      continue;
    }
    std::string tag;
    size_t p = off;
    while (p + 1 < plain.size() && tag.size() < kGamerTagMaxChars) {
      if (plain[p] != 0) break;
      const uint8_t ch = plain[p + 1];
      if (ch == 0) break;
      if (ch < 0x20 || ch >= 0x7F) {
        tag.clear();
        break;
      }
      tag.push_back(static_cast<char>(ch));
      p += 2;
    }
    if (tag.size() >= 3) {
      return tag;
    }
  }
  return {};
}

// The Account blob of the profile that is actually signed in.
//
// This used to run its own scan and take the first profile it found, which was
// the same answer as the GPD search for as long as only one profile was staged.
// With two staged they disagreed: selecting ECF094C2048FC0CD gave that
// profile's settings, game library and avatar, but the gamertag of whichever
// directory happened to sort first -- the dashboard showed REALmjoct's library
// under the name MVEALNG.
//
// Asking ProfileDirectory() keeps every part of the identity coming from one
// decision, so the two cannot drift apart again.
std::filesystem::path FindAccountFile() {
  const auto& dir = nxe_profile::ProfileDirectory();
  if (dir.empty()) {
    return {};
  }
  std::error_code ec;
  const auto account = dir / "Account";
  return std::filesystem::exists(account, ec) ? account : std::filesystem::path{};
}

// The decrypted Account record. Everything else here is derived from it, so it
// is decrypted once and kept.
std::vector<uint8_t> DecryptAccountAt(const std::filesystem::path& path) {
  if (path.empty()) return {};

  std::vector<uint8_t> account;
  FILE* f = nullptr;
  if (fopen_s(&f, path.string().c_str(), "rb") != 0 || f == nullptr) return {};
  account.resize(kAccountSize);
  const size_t read = std::fread(account.data(), 1, account.size(), f);
  std::fclose(f);
  if (read < kAccountSize) return {};

  uint8_t mac[20];
  HmacSha1(kConsoleKey, sizeof(kConsoleKey), account.data(), kConfounderSize, mac);

  std::vector<uint8_t> plain(account.begin() + kConfounderSize, account.end());
  Rc4(mac, 16, plain.data(), plain.size());

  if (!VerifyPlaintext(plain)) {
    REXLOG_WARN("Profile: Account did not decrypt with the known console key; gamertag unavailable");
    return {};
  }
  return plain;
}

std::vector<uint8_t> DecryptAccount() { return DecryptAccountAt(FindAccountFile()); }

}  // namespace

// The gamertag of an arbitrary staged profile.
//
// Separate from GamerTag() below, which caches the signed-in profile's. The
// sign-in list needs every profile to name itself, and those are read once
// while building the list rather than held.
// A profile's XAMACCOUNTINFO exactly as the console stored it.
//
// Preferred over rebuilding one field by field: the guest reads parts of this
// structure that were never being filled, and it decides what a profile *is*
// from them -- the online XUID at +0x28 tells it whether this is an Xbox LIVE
// account, and the cached user flags at +0x30 carry the rest.
std::vector<uint8_t> AccountInfoOf(const std::filesystem::path& profile_dir) {
  std::error_code ec;
  const auto account = profile_dir / "Account";
  if (!std::filesystem::exists(account, ec)) {
    return {};
  }
  const auto blob = DecryptAccountAt(account);
  if (blob.size() < kAccountInfoOffset + kAccountInfoSize) {
    return {};
  }
  return std::vector<uint8_t>(blob.begin() + kAccountInfoOffset,
                              blob.begin() + kAccountInfoOffset + kAccountInfoSize);
}

uint64_t OnlineXuidOf(const std::filesystem::path& profile_dir) {
  const auto info = AccountInfoOf(profile_dir);
  if (info.size() < 0x28 + 8) {
    return 0;
  }
  uint64_t xuid = 0;
  for (int i = 0; i < 8; ++i) {
    xuid = (xuid << 8) | info[0x28 + i];
  }
  return xuid;
}

std::string GamerTagOf(const std::filesystem::path& profile_dir) {
  std::error_code ec;
  const auto account = profile_dir / "Account";
  if (!std::filesystem::exists(account, ec)) {
    return {};
  }
  const auto blob = DecryptAccountAt(account);
  return blob.empty() ? std::string() : FindGamerTag(blob);
}

const std::vector<uint8_t>& AccountBlob() {
  return nxe_profile::ProfileScoped([] {
    auto blob = DecryptAccount();
    if (!blob.empty()) {
      REXLOG_INFO("Profile: Account decrypted, {} byte record", blob.size());
    }
    return blob;
  });
}

const std::string& GamerTag() {
  return nxe_profile::ProfileScoped([] {
    const auto& blob = AccountBlob();
    const std::string tag = blob.empty() ? std::string() : FindGamerTag(blob);
    if (!tag.empty()) {
      REXLOG_INFO("Profile: gamertag '{}' recovered from Account", tag);
    }
    return tag;
  });
}

}  // namespace nxe_profile
