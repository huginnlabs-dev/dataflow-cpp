// test_crash.cpp — standalone unit tests for crash capture
// (dataflow::Capture / CaptureException / CaptureTerminate). The recorder is
// exercised through the test seams in dataflow_crash.hpp (stack-capture
// override + crash observer), so the platform backtrace never makes the
// assertions flaky:
//
//   Windows (MinGW):   // no -ldbghelp: dbghelp is loaded at runtime
//     g++ -std=c++17 -Iinclude tests/test_crash.cpp src/crash.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o test_crash
//   Linux:
//     g++ -std=c++17 -Iinclude tests/test_crash.cpp src/crash.cpp src/dataflow.cpp -o test_crash
//
// The enabled run points the endpoint at 127.0.0.1:1 — recording happens
// locally, posts fail fast, and the process never talks to a real server.

#include "dataflow_crash.hpp"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

static int g_failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        std::printf("ok   %s\n", what);
        return;
    }
    ++g_failures;
    std::printf("FAIL %s\n", what);
}

static void check_eq(const std::string& got, const std::string& want, const char* what) {
    if (got == want) {
        std::printf("ok   %-64s -> %s\n", what, got.c_str());
        return;
    }
    ++g_failures;
    std::printf("FAIL %s\n  got:  \"%s\"\n  want: \"%s\"\n", what, got.c_str(), want.c_str());
}

// --- observer capture ---------------------------------------------------------

static int g_observed = 0;
static std::string g_span_name;
static int g_status = 0;
static std::string g_msg;
static std::string g_stack;

static void reset_observer() {
    g_observed = 0;
    g_span_name.clear();
    g_status = 0;
    g_msg.clear();
    g_stack.clear();
}

static void observe(const char* span_name, int status, const std::string& message,
                    const std::string& stack) {
    ++g_observed;
    g_span_name = span_name;
    g_status = status;
    g_msg = message;
    g_stack = stack;
}

// --- seams ---------------------------------------------------------------------

static std::string synthetic_stack() { return "#00 synthetic\n#01 frames\n"; }
static std::string unavailable_stack() { return ""; }
static std::string huge_stack() { return std::string(10000, 's'); }

// --- terminate chaining ----------------------------------------------------------

static int g_prev_called = 0;
static void fake_prev_terminate() { ++g_prev_called; }

int main() {
    dataflow::detail::g_crash_observer_for_test = &observe;
    dataflow::detail::g_stack_capture_for_test = &synthetic_stack;

    // --- disabled SDK: pure passthrough -------------------------------------
    dataflow::Settings off;
    off.disabled = true;
    dataflow::configure(off);

    check(dataflow::Capture([] {}), "disabled: clean function runs and returns true");

    bool escaped = false;
    try {
        dataflow::Capture([] { throw std::runtime_error("disabled boom"); });
    } catch (const std::exception& e) {
        escaped = std::string(e.what()) == "disabled boom";
    }
    check(escaped, "disabled: Capture lets the exception escape bare");
    check(g_observed == 0, "disabled: Capture records nothing");

    // --- enabled SDK ----------------------------------------------------------
    dataflow::Settings on;
    on.api_key = "test-key";
    on.endpoint = "http://127.0.0.1:1"; // nothing listens; posts fail fast
    on.service_name = "crash-tests";
    dataflow::configure(on);

    // Capture: clean function
    reset_observer();
    check(dataflow::Capture([] {}), "Capture: clean function returns true");
    check(g_observed == 0, "Capture: clean function records nothing");

    // Capture: throwing function -> synthetic "exception" span
    reset_observer();
    const bool ok = dataflow::Capture([] { throw std::runtime_error("boom"); });
    check(!ok, "Capture: throwing function returns false");
    check(g_observed == 1, "Capture: throwing function records one crash");
    check_eq(g_span_name, "exception", "Capture: synthetic span is named \"exception\"");
    check_eq(g_msg, "boom", "Capture: error_message is what()");
    check(g_status == 500, "Capture: status 500");
    check_eq(g_stack, synthetic_stack(), "Capture: error.stack captured inside the catch");

    // Capture: real platform backtrace (seam unset)
    reset_observer();
    dataflow::detail::g_stack_capture_for_test = nullptr;
    dataflow::Capture([] { throw std::runtime_error("real stack"); });
    check(g_observed == 1 && !g_stack.empty() && g_stack.compare(0, 3, "#00") == 0,
          "Capture: platform backtrace captured (frame #00 present)");
    if (g_failures == 0 && g_observed == 1) {
        std::printf("     stack head: %.80s\n", g_stack.c_str());
    }

    // Capture: message clipped to 500 bytes
    reset_observer();
    const std::string long_what(1000, 'x');
    dataflow::Capture([&long_what] { throw std::runtime_error(long_what); });
    check(g_observed == 1 && g_msg.size() == dataflow::kMaxErrorMessageBytes,
          "Capture: error_message clipped to 500 bytes");

    // Capture: stack clipped to 8192 bytes
    reset_observer();
    dataflow::detail::g_stack_capture_for_test = &huge_stack;
    dataflow::Capture([] { throw std::runtime_error("huge stack"); });
    check(g_observed == 1 && g_stack.size() == dataflow::kMaxStackBytes,
          "Capture: error.stack clipped to 8192 bytes");

    // Capture: stack unavailable path
    reset_observer();
    dataflow::detail::g_stack_capture_for_test = &unavailable_stack;
    dataflow::Capture([] { throw std::runtime_error("no stack"); });
    check_eq(g_stack, "<backtrace unavailable>", "Capture: unavailable stack reported");
    dataflow::detail::g_stack_capture_for_test = &synthetic_stack;

    // Capture: records on the active span when one is open
    reset_observer();
    {
        dataflow::Trace trace("svc.Work");
        dataflow::Capture([] { throw std::runtime_error("on span"); });
    }
    check(g_observed == 1 && g_span_name == "svc.Work" && g_msg == "on span",
          "Capture: records on the currently-active span");

    // CaptureException: no active span -> synthetic "exception" span
    reset_observer();
    try {
        throw std::runtime_error("explicit");
    } catch (const std::exception& e) {
        dataflow::CaptureException(e);
    }
    check(g_observed == 1 && g_span_name == "exception" && g_msg == "explicit" && g_status == 500,
          "CaptureException: synthetic span, what(), status 500");

    // CaptureException: active span -> annotated in place
    reset_observer();
    try {
        throw std::runtime_error("handled upstream");
    } catch (const std::exception& e) {
        dataflow::Trace trace("svc.Handler");
        dataflow::CaptureException(e);
    }
    check(g_observed == 1 && g_span_name == "svc.Handler",
          "CaptureException: records on the active span");

    // --- CaptureTerminate --------------------------------------------------------
    std::set_terminate(&fake_prev_terminate);
    dataflow::CaptureTerminate();
    const std::terminate_handler installed = std::get_terminate();
    dataflow::CaptureTerminate(); // second call must be a no-op
    check(std::get_terminate() == installed, "CaptureTerminate: install is idempotent");

    // No active exception: records "terminate", chains to the previous handler
    reset_observer();
    std::get_terminate()();
    check(g_prev_called == 1, "CaptureTerminate: chains to the previous handler");
    check(g_observed == 1 && g_span_name == "terminate" && g_msg == "terminate" && g_status == 500,
          "CaptureTerminate: synthetic \"terminate\" span recorded");

    // Active exception: its what() is recovered best-effort
    reset_observer();
    try {
        throw std::runtime_error("fatal: bad state");
    } catch (...) {
        std::get_terminate()();
    }
    check(g_prev_called == 2, "CaptureTerminate: chained again");
    check(g_observed == 1 && g_msg == "fatal: bad state",
          "CaptureTerminate: active exception's what() recorded");

    // Disabled SDK: the handler chains immediately, records nothing
    dataflow::configure(off);
    reset_observer();
    std::get_terminate()();
    check(g_prev_called == 3, "CaptureTerminate: disabled SDK still chains");
    check(g_observed == 0, "CaptureTerminate: disabled SDK records nothing");

    if (g_failures == 0) {
        std::printf("\nall tests passed\n");
        return 0;
    }
    std::printf("\n%d test(s) failed\n", g_failures);
    return 1;
}
