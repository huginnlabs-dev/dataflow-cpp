// dataflow_crash.hpp — crash capture for the Dataflow C++ SDK (0.5.0+).
//
// C++ has no panics; the crash targets are uncaught exceptions and
// std::terminate. Everything recorded here follows the SDK error wire
// convention: the record lands on the currently-active span (or a synthetic
// "exception" span when none is open) with status 500, error_message =
// the exception message (clipped to 500 bytes) and metadata "error.stack" =
// the captured backtrace text (clipped to 8192 bytes).
//
//   int main() {
//       dataflow::configure();            // reads DATAFLOW_* env
//       dataflow::CaptureTerminate();     // once, first thing in main()
//
//       dataflow::Capture([] {            // returns false when fn threw
//           dataflow::Trace trace("scheduler.Run");
//           run_scheduler();              // a throw is recorded, not lost
//       });
//   }
//
// Stack capture is platform best-effort — Windows: RtlCaptureStackBackTrace +
// SymFromAddr (dbghelp loaded at runtime via GetProcAddress, never linked;
// every failure is guarded); POSIX: backtrace()/backtrace_symbols() from
// <execinfo.h>. When the platform refuses to cooperate, "error.stack" is
// "<backtrace unavailable>". The whole crash path is wrapped so it can never
// throw, and with the SDK disabled everything is a passthrough (fn runs
// bare; the terminate handler chains immediately).
//
// Requires linking src/crash.cpp alongside src/dataflow.cpp.

#pragma once

#include <cstddef>
#include <exception>
#include <string>
#include <utility>

#include "dataflow.hpp"

namespace dataflow {

// Wire caps for the crash record (byte clips, mirroring the other SDKs).
inline constexpr std::size_t kMaxErrorMessageBytes = 500;
inline constexpr std::size_t kMaxStackBytes = 8192;

namespace detail {

// --- test seams (internal; not part of the stable surface) -----------------

// Overrides platform stack capture when set. Returning "" simulates an
// unavailable backtrace (recorded as "<backtrace unavailable>").
using StackCaptureFn = std::string (*)();
extern StackCaptureFn g_stack_capture_for_test;

// Receives every crash record (span name — "exception" for the synthetic
// span, or the active span's name — status, message, stack) for inspection.
using CrashObserverFn = void (*)(const char* span_name, int status,
                                 const std::string& message, const std::string& stack);
extern CrashObserverFn g_crash_observer_for_test;

// Defined in src/crash.cpp; used by the Capture template below.
std::string capture_stack_text();                      // "" on any failure
// Records per the wire convention; synthetic_name names the span created
// when no span is active on the thread ("exception" for Capture/
// CaptureException, "terminate" for the terminate handler).
void record_crash(const std::string& message, const std::string& stack,
                  const char* synthetic_name = "exception"); // never throws

} // namespace detail

// Runs fn() inside a catch-all: on a clean run returns true. When fn throws,
// the backtrace is captured INSIDE the catch (so the failing call path is
// still on the stack), the crash is recorded per the wire convention above
// and false is returned — the exception itself is consumed. With the SDK
// disabled, fn runs bare: nothing is recorded and exceptions propagate.
template <typename F>
bool Capture(F&& fn) {
    if (!enabled()) {
        fn(); // passthrough: exceptions escape uncaught, exactly as without the SDK
        return true;
    }
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        try {
            detail::record_crash(e.what() ? e.what() : "exception",
                                 detail::capture_stack_text());
        } catch (...) {
        }
        return false;
    } catch (...) {
        try {
            detail::record_crash("unknown non-standard exception",
                                 detail::capture_stack_text());
        } catch (...) {
        }
        return false;
    }
}

// Records an exception you have already caught yourself. Because the throw
// site's stack is gone by then, the backtrace captured is the caller's
// current one — call it as close to the catch as possible, or use Capture(),
// which captures inside its catch. Never throws.
void CaptureException(const std::exception& e);

// Installs a std::terminate handler (once; further calls are no-ops) that
// records a synthetic "terminate" span — error_message is the currently
// active exception's what() when the runtime still hands it over, else
// "terminate" — synchronously flushes the submission pipeline (no sender
// thread will run again), then chains to the PREVIOUS terminate handler (or
// abort() when none was installed). Warms the symbolizer at install time so
// the crash path stays cheap.
void CaptureTerminate();

} // namespace dataflow
