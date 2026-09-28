// dataflow.hpp — HuginnLabs Dataflow SDK for C++.
//
// RAII spans, a thread-local trace context and a background sender that
// ships events to the SaaS over the REST ingestion endpoint
// (POST /api/v1/ingest) — wire-compatible with the Go/Python SDKs, but with
// zero third-party dependencies on Windows (WinHTTP + BCrypt); on Linux it
// uses cpp-httplib + OpenSSL.
//
//   int main() {
//       dataflow::configure();                       // reads DATAFLOW_* env
//       dataflow::Trace trace("scheduler.Run");      // RAII span, auto-end
//       trace.span().set_data("job", "42");
//       ...                                          // nested Trace() calls
//   }                                                // join the same trace
//
// Env: DATAFLOW_API_KEY, DATAFLOW_ENDPOINT (http(s)://base), 
//      DATAFLOW_SERVICE_NAME, DATAFLOW_ENCRYPTION_KEY, DATAFLOW_SAMPLE_RATIO,
//      DATAFLOW_BUFFER_SIZE, DATAFLOW_DISABLED.

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dataflow {

inline constexpr const char* kVersion = "0.1.0";

// Event types, mirroring proto/dataflow.proto string names.
inline constexpr const char* kHttpServer = "HTTP_SERVER";
inline constexpr const char* kHttpClient = "HTTP_CLIENT";
inline constexpr const char* kFunctionCall = "FUNCTION_CALL";
inline constexpr const char* kGrpc = "GRPC";

struct Settings {
    std::string api_key;
    std::string endpoint;       // base URL, e.g. "http://localhost:25080"
    std::string service_name;
    std::string encryption_key; // empty -> payloads are plaintext JSON
    double sample_ratio = 1.0;
    int buffer_size = 10000;
    bool disabled = false;
};

// Reads DATAFLOW_* environment variables and starts the background sender
// (unless disabled or unconfigured). Safe to call again to override.
void configure();
void configure(const Settings& settings);

const Settings& settings();
bool enabled();

std::string version();

// ---------------------------------------------------------------------------
// Span — one measured unit of work. Move-only; ends automatically on scope
// exit when end() was not called explicitly.
class Span {
public:
    // Shared with the background sender; public so the internal helpers in
    // the translation unit can hold it.
    struct Impl {
        std::mutex mu;
        std::vector<std::pair<std::string, std::string>> metadata;
        std::vector<std::pair<std::string, std::string>> payload; // JSON-encoded values
        std::string error_message;
        std::string plaintext_payload; // used when no encryption key is set
        bool encrypted_payload = false;
        std::string payload_b64, iv_b64, key_salt;
        int status_code = 0;
        std::string event_id;
        long long seq = 0;
        std::string trace_id;
        std::string span_id;
        std::string parent_span_id;
        std::string type;
        std::string service_name;
        std::string name;
        std::string caller_package;
        std::string callee_package;
        long long timestamp_ms = 0;
        long long duration_ms = 0;
        std::chrono::steady_clock::time_point started;
    };

    Span() = default;
    Span(Span&& other) noexcept;
    Span& operator=(Span&& other) noexcept;
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    ~Span();

    // Metric-grade plaintext attributes.
    Span& set_attr(const std::string& key, const std::string& value);
    // Payload data: values are stored as JSON strings (encrypted at end()
    // when an encryption key is configured).
    Span& set_data(const std::string& key, const std::string& value);
    // Store an already-serialized JSON value.
    Span& set_data_json(const std::string& key, const std::string& json_value);
    Span& record_error(const std::string& message);
    Span& set_status(int code);
    Span& set_callee(const std::string& package);
    void end();

    const std::string& trace_id() const { return trace_id_; }
    const std::string& span_id() const { return span_id_; }
    explicit operator bool() const { return !span_id_.empty(); }

private:
    friend Span start_span(const std::string& name, const char* type);
    friend Span current_span();
    friend class Trace;
    std::shared_ptr<Impl> impl_; // shared with the async sender
    std::string trace_id_;
    std::string span_id_;
    bool sampled_ = true;
    bool ended_ = false;
};

// Opens a child of the current thread's span (if any).
Span start_span(const std::string& name, const char* type = kFunctionCall);

// The span currently active on this thread (or an invalid Span).
Span current_span();

// RAII helper: opens a span named "pkg.Func" and ends it on scope exit,
// making it the thread's current span for the duration.
class Trace {
public:
    explicit Trace(const std::string& name, const char* type = kFunctionCall);
    ~Trace();
    Trace(const Trace&) = delete;
    Trace& operator=(const Trace&) = delete;
    Span& span() { return span_; }

private:
    Span span_;
    void* previous_ = nullptr; // opaque previous-context token
};

// The span currently active on this thread (or an invalid Span).
Span current_span();

// ---------------------------------------------------------------------------
// Traced HTTP client: issues the request and records an HTTP_CLIENT span
// joined to the current trace, carrying the X-Dataflow-Trace-Id header.
struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error; // non-empty on transport failure
};

HttpResponse http_get(const std::string& url, int timeout_ms = 10000);
HttpResponse http_post(const std::string& url, const std::string& body,
                       const std::string& content_type = "application/json",
                       int timeout_ms = 10000);

} // namespace dataflow
