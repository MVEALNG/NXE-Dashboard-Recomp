// Replace the homepage channel manifest before it is parsed.
//
// The manifest that decides the top-level tabs and their tiles is NOT the copy
// in homepage.xzp. That package holds it as plain uncompressed text at a known
// offset, which makes it look like the obvious thing to edit, and editing it
// does nothing: with homepage.xzp moved aside entirely the channel list comes up
// byte-identical. The copy that matters is compiled into the executable -- which
// is what the extracted asset name emb_homepage.xml was saying all along.
//
// It is not loaded through the epix resource path either. The definition is
// fetched as epix://homepage.xml (guest 0x922D1640 compares the URL against
// L"homepage.xml" at 0x92027590), and epix:// normally resolves via the local
// resource loader at 0x922D6C48 -- but that function, hooked and instrumented,
// is never called for this document.
//
// Rather than keep guessing at loaders, find the text itself. The document has
// to exist as bytes in guest memory before anything can parse it, so this walks
// the committed regions of the guest address space looking for "<homepage>" at
// the moment the "Epix Homepage Def" thread body (0x922D2060) starts, which is
// the last point before the parse begins.
//
// If an override file is configured and fits, the document is rewritten in
// place and padded with spaces -- the parser stops at </homepage> and never
// reads past it, and keeping the length identical means nothing around the
// buffer moves. If it does not fit, the stock manifest is left alone rather
// than half-written.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

#include <windows.h>

using namespace rex;

extern "C" {
void __imp__sub_922D2060(PPCContext& __restrict ctx, uint8_t* base);  // Epix Homepage Def thread
}

REXCVAR_DEFINE_STRING(homepage_manifest, "homepage_override.xml", "Dashboard",
                      "XML file replacing the dashboard's built-in homepage channel manifest. "
                      "Relative paths resolve against the working directory. Empty disables the "
                      "override.");

namespace {

// The guest address space is sparse; touching a reserved-but-uncommitted page
// would fault. Walk it with VirtualQuery and only read what is committed and
// readable.
struct Region {
  uint8_t* start;
  size_t size;
};

std::vector<Region> CommittedRegions(uint8_t* base, size_t span) {
  std::vector<Region> out;
  uint8_t* p = base;
  uint8_t* end = base + span;
  MEMORY_BASIC_INFORMATION mbi;
  while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
    uint8_t* rb = static_cast<uint8_t*>(mbi.BaseAddress);
    size_t rs = mbi.RegionSize;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & PAGE_GUARD)) {
      out.push_back({rb, rs});
    }
    p = rb + rs;
    if (rs == 0) break;
  }
  return out;
}

// A hit, carrying the end of the region it was found in so that every later
// read stays inside mapped memory -- the buffer is not null-terminated, so an
// unbounded scan walks straight off the end of the mapping.
struct Hit {
  uint8_t* at;
  uint8_t* region_end;
};

uint8_t* FindWithin(uint8_t* from, uint8_t* end, const char* needle) {
  const size_t n = std::strlen(needle);
  if (size_t(end - from) < n) return nullptr;
  for (uint8_t* q = from; q <= end - n; ++q) {
    if (q[0] == uint8_t(needle[0]) && std::memcmp(q, needle, n) == 0) return q;
  }
  return nullptr;
}

std::vector<Hit> FindAll(uint8_t* base, size_t span, const char* needle) {
  const size_t n = std::strlen(needle);
  std::vector<Hit> hits;
  for (const auto& r : CommittedRegions(base, span)) {
    if (r.size < n) continue;
    uint8_t* end = r.start + r.size;
    for (uint8_t* q = r.start; q <= end - n; ++q) {
      if (q[0] == uint8_t(needle[0]) && std::memcmp(q, needle, n) == 0) hits.push_back({q, end});
    }
  }
  return hits;
}

const std::string* OverrideText() {
  static bool s_loaded = false;
  static std::string s_text;
  if (!s_loaded) {
    s_loaded = true;
    const std::string path = REXCVAR_GET(homepage_manifest);
    if (!path.empty()) {
      std::ifstream f(path, std::ios::binary);
      if (f) {
        s_text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        REXKRNL_INFO("[homepage] override loaded from '{}' ({} bytes)", path, s_text.size());
      } else {
        REXKRNL_WARN("[homepage] no override file at '{}'", path);
      }
    }
  }
  return s_text.empty() ? nullptr : &s_text;
}

// How much of the guest address space to sweep. The image sits at 0x92000000
// and the heap the channel objects come from is down at 0x40000000, so a sweep
// of the low 1 GiB covers both; VirtualQuery skips the holes cheaply.
constexpr size_t kGuestSpan = 0x40000000ull;

}  // namespace

extern "C" void sub_922D2060(PPCContext& __restrict ctx, uint8_t* base) {
  static bool s_done = false;
  if (!s_done) {
    s_done = true;
    auto hits = FindAll(base, kGuestSpan, "<homepage>");
    REXKRNL_WARN("[homepage] found {} copies of the manifest in guest memory", hits.size());
    for (const auto& hit : hits) {
      uint8_t* h = hit.at;
      const uint64_t guest = static_cast<uint64_t>(h - base);
      // How much room is there? Measure to the end of the document, bounded by
      // the mapping the hit was found in.
      uint8_t* endp = FindWithin(h, hit.region_end, "</homepage>");
      const size_t doc = endp ? size_t(endp - h) + 11 : 0;
      REXKRNL_WARN("[homepage]   guest {:#x}, document {} bytes", guest, doc);

      const std::string* rep = OverrideText();
      if (!rep || !doc) continue;
      if (rep->size() > doc) {
        REXKRNL_WARN("[homepage]   override is {} bytes, only {} available -- keeping stock",
                     rep->size(), doc);
        continue;
      }
      std::memcpy(h, rep->data(), rep->size());
      std::memset(h + rep->size(), ' ', doc - rep->size());
      REXKRNL_WARN("[homepage]   replaced ({} used, {} spare)", rep->size(), doc - rep->size());
    }
  }
  __imp__sub_922D2060(ctx, base);
}
