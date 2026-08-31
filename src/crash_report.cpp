// Last-resort crash reporter.
//
// The intermittent segfault leaves no trace: the SDK installs a VECTORED handler
// (core/exception_handler_win.cpp) but that exists to service guest memory
// emulation -- it classifies the fault, offers it to registered handlers, and
// returns EXCEPTION_CONTINUE_SEARCH when none claim it. Nothing logs. The
// process then dies with the log simply stopping mid-frame.
//
// lldb is not usable here either (the LLVM install is missing python311.dll), so
// install a top-level unhandled-exception filter and write the fault out
// ourselves. The RVA is the useful part: subtract the module base from RIP and
// the result maps back to a generated sub_XXXXXXXX via the .map/.pdb, which is
// how the earlier unregistered-function faults were pinned down.

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")

namespace {

LONG WINAPI InfCrashFilter(EXCEPTION_POINTERS* ep) {
  auto* rec = ep->ExceptionRecord;
  auto* ctx = ep->ContextRecord;
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const auto rip = static_cast<uintptr_t>(ctx->Rip);

  FILE* f = std::fopen("crash_report.txt", "a");
  if (f) {
    std::fprintf(f, "=== unhandled exception ===\n");
    std::fprintf(f, "code           0x%08lX\n", rec->ExceptionCode);
    std::fprintf(f, "rip            0x%016llX\n", (unsigned long long)rip);
    std::fprintf(f, "module base    0x%016llX\n", (unsigned long long)base);
    std::fprintf(f, "RVA            0x%llX\n",
                 (unsigned long long)(rip >= base ? rip - base : 0));
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        rec->NumberParameters >= 2) {
      std::fprintf(f, "access         %s\n",
                   rec->ExceptionInformation[0] == 1 ? "write" :
                   rec->ExceptionInformation[0] == 0 ? "read" : "execute");
      std::fprintf(f, "fault address  0x%016llX\n",
                   (unsigned long long)rec->ExceptionInformation[1]);
    }
    // Per-frame module attribution.
    //
    // A bare RVA is only meaningful against the module the frame actually lives
    // in, and these stacks cross freely between nxe_dash.exe, rexruntimed.dll
    // and the system DLLs. Subtracting the main module base from every frame
    // (as this used to) gave correct numbers for the exe and garbage for
    // everything else -- which is precisely what made the abort() underneath the
    // storage-device list builder unreadable. Resolve each frame against its own
    // module instead, so every line can be handed to llvm-symbolizer as-is.
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    std::fprintf(f, "stack (innermost first):\n");
    for (USHORT i = 0; i < n; ++i) {
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
      // Symbol name where the PDB can give one.
      //
      // Module+offset attributes a frame to a binary but not to a function, and
      // with 26,730 generated functions in the exe that is the only detail that
      // matters. The symbol handler is initialised lazily -- doing it at start-up
      // costs a visible pause on an 80MB executable.
      char symline[512] = "";
      {
        static bool s_sym_ready = false;
        static bool s_sym_tried = false;
        if (!s_sym_tried) {
          s_sym_tried = true;
          SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
          s_sym_ready = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        }
        if (s_sym_ready) {
          alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 512] = {};
          auto* si = reinterpret_cast<SYMBOL_INFO*>(sbuf);
          si->SizeOfStruct = sizeof(SYMBOL_INFO);
          si->MaxNameLen = 500;
          DWORD64 disp = 0;
          if (SymFromAddr(GetCurrentProcess(), a, &disp, si)) {
            std::snprintf(symline, sizeof(symline), "  %s+0x%llX", si->Name,
                          (unsigned long long)disp);
          }
        }
      }
      std::fprintf(f, "  [%2u] %-22s +0x%llX%s\n", i, name,
                   (unsigned long long)(mbase != 0 ? a - mbase : a), symline);
    }
    std::fprintf(f, "\n");
    std::fclose(f);
  }
  return EXCEPTION_EXECUTE_HANDLER;  // terminate, having recorded the fault
}

struct InstallCrashFilter {
  InstallCrashFilter() { SetUnhandledExceptionFilter(InfCrashFilter); }
} g_install_crash_filter;

}  // namespace
