// dataflow_bridge.hpp — one-line instrumentation bridge for existing C++
// codebases (built for theants, works anywhere). Sits on top of
// dataflow.hpp and turns "add tracing" into "add one line at function
// entry" — no framework, no base classes, no thread plumbing:
//
//   #include "dataflow_bridge.hpp"
//
//   void App::init() {
//       DATAFLOW_INIT();                       // once, reads DATAFLOW_* env
//   }
//
//   void Core::Tick() {
//       DATAFLOW_SPAN("Core.Tick");            // RAII: ends at scope exit
//       ...
//       DATAFLOW_DATA("ants_active", active);  // payload field (E2E-encrypted)
//       if (bad) DATAFLOW_ERROR("queue full");
//   }
//
//   // HTTP entry point with route + status:
//   DATAFLOW_REQUEST_BEGIN("/api/state");
//   ... handle ...
//   DATAFLOW_REQUEST_END(200);
//
// Everything is a no-op unless DATAFLOW_INIT() ran with a configured
// endpoint (DATAFLOW_API_KEY/DATAFLOW_ENDPOINT env), so shipping the lines
// in production builds is safe: when unconfigured the SDK stays passive.
//
// MSVC 2019+ / C++17, no third-party dependencies on Windows.

#pragma once

#include "dataflow.hpp"

#include <cstdio>
#include <string>
#include <utility>

namespace dataflow {
namespace bridge {

// DATAFLOW_INIT() guard: configure() is cheap but not free; call it once.
inline void init_once() {
    static const bool done = [] {
        configure(); // reads DATAFLOW_* env; passive when unset
        return true;
    }();
    (void)done;
}

// JSON-escape a scalar for set_data.
inline std::string json_string(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

// RAII scoped span used by DATAFLOW_SPAN / DATAFLOW_REQUEST_BEGIN.
class ScopedSpan {
public:
    explicit ScopedSpan(std::string name, const char* type = kFunctionCall)
        : span_(start_span(std::move(name), type)) {}
    ~ScopedSpan() { if (span_) span_.end(); }
    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;
    Span* operator->() { return &span_; }
    Span& get() { return span_; }

private:
    Span span_;
};

} // namespace bridge
} // namespace dataflow

// ---------------------------------------------------------------------------
// Macros — all evaluate to at most one statement, safe at function entry.
// Nested spans resolve through the SDK's thread-local current-span stack.

// Call once (e.g. at startup); reads DATAFLOW_* environment variables.
#define DATAFLOW_INIT() ::dataflow::bridge::init_once()

#define DATAFLOW_SPAN_CAT2(a, b) a##b
#define DATAFLOW_SPAN_CAT(a, b) DATAFLOW_SPAN_CAT2(a, b)

// Scoped function span named like "Core.Tick". Ends automatically at scope
// exit with duration, status and any DATAFLOW_ERROR set inside.
#define DATAFLOW_SPAN(name) \
    ::dataflow::bridge::ScopedSpan DATAFLOW_SPAN_CAT(df_span_, __LINE__)(name)

// The innermost span active on this thread (the DATAFLOW_SPAN just opened,
// or the enclosing HTTP request span).
#define DATAFLOW_CURRENT_SPAN() ::dataflow::current_span()

#define DATAFLOW_ERROR(msg) \
    do { if (::dataflow::Span df_s = DATAFLOW_CURRENT_SPAN()) df_s.record_error(msg); } while (0)

#define DATAFLOW_STATUS(code) \
    do { if (::dataflow::Span df_s = DATAFLOW_CURRENT_SPAN()) df_s.set_status(code); } while (0)

// Payload field (string value); encrypted client-side when a key is set.
#define DATAFLOW_DATA(key, value) \
    do { if (::dataflow::Span df_s = DATAFLOW_CURRENT_SPAN()) df_s.set_data(key, value); } while (0)

// Payload field with a pre-serialized JSON value.
#define DATAFLOW_DATA_JSON(key, json) \
    do { if (::dataflow::Span df_s = DATAFLOW_CURRENT_SPAN()) df_s.set_data_json(key, json); } while (0)

// Payload field from a numeric value.
#define DATAFLOW_DATA_NUM(key, num) \
    do { if (::dataflow::Span df_s = DATAFLOW_CURRENT_SPAN()) df_s.set_data(key, std::to_string(num)); } while (0)

// HTTP entry point: opens an HTTP_SERVER span named after the route.
#define DATAFLOW_REQUEST_BEGIN(route) DATAFLOW_SPAN_CAT(df_req_, __LINE__)((route), ::dataflow::kHttpServer)

// Close the request explicitly with its HTTP status (also happens on scope
// exit; the last call wins).
#define DATAFLOW_REQUEST_END(status_code) DATAFLOW_STATUS(status_code)
