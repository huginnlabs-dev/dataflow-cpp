// crash.cpp — crash capture for the Dataflow C++ SDK: backtrace capture,
// crash recording on the current/synthetic span, and a std::terminate
// handler. Kept in its own translation unit with platform guards so the rest
// of the SDK never pays for it; dbghelp is loaded at RUNTIME via
// GetProcAddress (never linked), so MinGW builds need no import library and
// every symbolization failure degrades to raw addresses.
//
// The crash path must never throw: every recording step is try/catch(...)
// wrapped and allocates only small strings.

#include "dataflow_crash.hpp"
#include "dataflow_internal.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    if defined(__has_include)
#        if __has_include(<execinfo.h>)
#            include <execinfo.h>
#            define DATAFLOW_HAS_EXECINFO 1
#        endif
#    elif defined(__GLIBC__) || defined(__APPLE__)
#        include <execinfo.h>
#        define DATAFLOW_HAS_EXECINFO 1
#    endif
#endif

namespace dataflow {
namespace detail {

// --- test seams -------------------------------------------------------------

StackCaptureFn g_stack_capture_for_test = nullptr;
CrashObserverFn g_crash_observer_for_test = nullptr;

namespace {

std::string clip_bytes(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    return s.substr(0, max_bytes);
}

// --- platform backtrace ------------------------------------------------------

#ifdef _WIN32

// Minimal ANSI SYMBOL_INFO mirror (dbghelp.h is deliberately not included).
struct SymInfoAnsi {
    unsigned long size_of_struct;
    unsigned long type_index;
    unsigned long long reserved[2];
    unsigned long index;
    unsigned long size;
    unsigned long long mod_base;
    unsigned long long flags;
    unsigned long long value;
    unsigned long long address;
    unsigned long reg;
    unsigned long scope;
    unsigned long tag;
    unsigned long name_len;
    unsigned long max_name_len;
    char name[1];
};

using RtlCaptureStackBackTraceFn = unsigned short(__stdcall*)(unsigned long, unsigned long,
                                                              void**, unsigned long*);
using SymInitializeFn = int(__stdcall*)(void*, const char*, int);
using SymFromAddrFn = int(__stdcall*)(void*, unsigned long long, unsigned long long*,
                                      SymInfoAnsi*);
using SymSetOptionsFn = unsigned long(__stdcall*)(unsigned long);

RtlCaptureStackBackTraceFn capture_backtrace_fn() {
    // Resolved once. kernel32 exports RtlCaptureStackBackTrace on Windows 7+;
    // ntdll is the fallback for older/trimmed systems.
    static RtlCaptureStackBackTraceFn fn = []() -> RtlCaptureStackBackTraceFn {
        HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
        if (k32) {
            if (void* p = reinterpret_cast<void*>(::GetProcAddress(k32, "RtlCaptureStackBackTrace")))
                return reinterpret_cast<RtlCaptureStackBackTraceFn>(p);
        }
        HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
        if (nt) {
            if (void* p = reinterpret_cast<void*>(::GetProcAddress(nt, "RtlCaptureStackBackTrace")))
                return reinterpret_cast<RtlCaptureStackBackTraceFn>(p);
        }
        return nullptr;
    }();
    return fn;
}

// dbghelp symbolization state: loaded lazily, initialized once, and kept
// behind a mutex (dbghelp is single-threaded). Guard failures everywhere —
// without symbols the backtrace still carries raw module addresses.
struct SymEngine {
    std::mutex mu;
    SymFromAddrFn from_addr = nullptr;
    bool ready = false;
};

SymEngine& sym_engine() {
    static SymEngine engine;
    // The lambda references the static directly (no capture: it has static
    // storage duration) and runs exactly once.
    [[maybe_unused]] static const bool initialized = []() -> bool {
        HMODULE h = ::LoadLibraryW(L"dbghelp.dll");
        if (!h) return false; // keep raw-address mode
        engine.from_addr = reinterpret_cast<SymFromAddrFn>(
            reinterpret_cast<void*>(::GetProcAddress(h, "SymFromAddr")));
        auto init = reinterpret_cast<SymInitializeFn>(
            reinterpret_cast<void*>(::GetProcAddress(h, "SymInitialize")));
        auto set_opts = reinterpret_cast<SymSetOptionsFn>(
            reinterpret_cast<void*>(::GetProcAddress(h, "SymSetOptions")));
        if (!init || !engine.from_addr) {
            engine.from_addr = nullptr;
            return false;
        }
        if (set_opts)
            set_opts(0x2 /*SYMOPT_UNDNAME*/ | 0x4 /*SYMOPT_DEFERRED_LOADS*/);
        // Invade: load symbols for already-loaded modules while healthy.
        engine.ready = init(::GetCurrentProcess(), nullptr, TRUE) != 0;
        return engine.ready;
    }();
    return engine;
}

std::string capture_stack_platform() {
    RtlCaptureStackBackTraceFn rtl = capture_backtrace_fn();
    if (!rtl) return "";
    void* frames[62];
    // Skip this frame + capture_stack_text so #0 is the crash site's caller.
    const unsigned short n = rtl(2, 62, frames, nullptr);
    if (n == 0) return "";

    SymEngine& engine = sym_engine();
    std::string out;
    char line[128];
    for (unsigned short i = 0; i < n && out.size() < kMaxStackBytes; ++i) {
        std::snprintf(line, sizeof(line), "#%02u 0x%p", static_cast<unsigned>(i), frames[i]);
        out += line;
        if (engine.ready) {
            std::lock_guard<std::mutex> lock(engine.mu);
            alignas(8) char buf[sizeof(SymInfoAnsi) + 512] = {};
            SymInfoAnsi* sym = reinterpret_cast<SymInfoAnsi*>(buf);
            sym->size_of_struct = sizeof(SymInfoAnsi);
            sym->max_name_len = 512;
            unsigned long long disp = 0;
            if (engine.from_addr(::GetCurrentProcess(),
                                 reinterpret_cast<unsigned long long>(frames[i]), &disp, sym) &&
                sym->name[0]) {
                out += ' ';
                out += sym->name;
                std::snprintf(line, sizeof(line), " +0x%llx",
                              static_cast<unsigned long long>(disp));
                out += line;
            }
        }
        out += '\n';
    }
    return out;
}

#else // !_WIN32

#ifdef DATAFLOW_HAS_EXECINFO

std::string capture_stack_platform() {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    if (n <= 0) return "";
    char** syms = ::backtrace_symbols(frames, n);
    if (!syms) return "";
    std::string out;
    for (int i = 0; i < n && out.size() < kMaxStackBytes; ++i) {
        char line[32];
        std::snprintf(line, sizeof(line), "#%02d ", i);
        out += line;
        out += syms[i];
        out += '\n';
    }
    std::free(syms);
    return out;
}

#else

std::string capture_stack_platform() { return ""; }

#endif // DATAFLOW_HAS_EXECINFO

#endif // _WIN32

} // namespace

std::string capture_stack_text() {
    try {
        if (g_stack_capture_for_test) return g_stack_capture_for_test();
        return capture_stack_platform();
    } catch (...) {
        return ""; // recorded as "<backtrace unavailable>"
    }
}

void record_crash(const std::string& message, const std::string& stack,
                  const char* synthetic_name) {
    try {
        if (!enabled()) return;
        const std::string msg = clip_bytes(message, kMaxErrorMessageBytes);
        const std::string st = stack.empty() ? std::string("<backtrace unavailable>")
                                             : clip_bytes(stack, kMaxStackBytes);
        std::string where = synthetic_name;
        if (Span::Impl* cur = current_impl()) {
            // Active span on this thread: annotate it in place — it ends (and
            // is enqueued) by its owning Trace as usual.
            where = cur->name;
            std::lock_guard<std::mutex> lock(cur->mu);
            cur->error_message = msg;
            cur->status_code = 500;
            cur->metadata.emplace_back("error.stack", st);
        } else {
            Span span = start_span(synthetic_name, kFunctionCall);
            if (span) {
                span.set_status(500);
                span.record_error(msg);
                span.set_attr("error.stack", st);
                span.end();
            }
        }
        if (CrashObserverFn observe = g_crash_observer_for_test) {
            observe(where.c_str(), 500, msg, st);
        }
    } catch (...) {
        // The crash path must never throw.
    }
}

} // namespace detail

// --- CaptureException + terminate handler -----------------------------------

namespace {

std::atomic<bool> g_terminate_installed{false};
std::atomic<std::terminate_handler> g_prev_terminate{nullptr};

void terminate_trampoline() {
    // Chain bookkeeping first: nothing below may break the handoff.
    const std::terminate_handler prev = g_prev_terminate.load();

    if (enabled()) {
        try {
            std::string msg = "terminate";
            // Best effort: recover the active exception's text. Only rethrow
            // when the runtime still holds one (rethrowing a null
            // exception_ptr would call terminate recursively).
            if (std::exception_ptr cur = std::current_exception()) {
                try {
                    std::rethrow_exception(cur);
                } catch (const std::exception& e) {
                    try {
                        msg = e.what();
                    } catch (...) {
                    }
                } catch (...) {
                    // Active but not a std::exception: keep "terminate".
                }
            }
            detail::record_crash(msg, detail::capture_stack_text(), "terminate");
        } catch (...) {
        }
        try {
            detail::flush_now();
        } catch (...) {
        }
    }

    if (prev) {
        prev();
        // A terminate handler must not return: when we got here through
        // std::terminate, the runtime aborts the process if we return.
        // Direct calls (tests) fall through harmlessly.
    } else {
        std::abort();
    }
}

} // namespace

void CaptureException(const std::exception& e) {
    try {
        detail::record_crash(e.what() ? e.what() : "exception",
                             detail::capture_stack_text());
    } catch (...) {
    }
}

void CaptureTerminate() {
    if (g_terminate_installed.exchange(true)) return; // idempotent install
    if (enabled()) {
        try {
            detail::capture_stack_text(); // warm dbghelp/SymInitialize while healthy
        } catch (...) {
        }
    }
    g_prev_terminate.store(std::set_terminate(&terminate_trampoline));
}

} // namespace dataflow
