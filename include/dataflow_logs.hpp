// dataflow_logs.hpp — application log shipping for the Dataflow C++ SDK
// (0.6.0+).
//
// LogDebug/LogInfo/LogWarn/LogError buffer log lines in-process and a
// background flusher POSTs them to the server's REST log endpoint in batches
// (POST /api/v1/logs, X-Api-Key header). Every line is stamped with the
// current span's trace/span ids (when one is active on the calling thread)
// so logs line up with traces in the dashboard's Logs tab.
//
//   int main() {
//       dataflow::configure();                  // reads DATAFLOW_* env
//       dataflow::Trace trace("scheduler.Run");
//       dataflow::LogInfo("job started", {{"job", "42"}});
//       ...                                     // ids correlate with the span
//       dataflow::FlushLogs();                  // optional: ship before exit
//   }
//
// Logging is strictly best-effort: it never throws, never blocks for I/O on
// the caller's thread, and drops the oldest lines when the in-process buffer
// (1024 lines) overflows. It uses the manifest's base-URL resolution —
// DATAFLOW_HTTP_URL wins; otherwise the configured endpoint when it carries
// an http(s) scheme (configure() derives "http://" for bare host:port
// values, and that derived scheme is honored, exactly like the startup
// manifest). With the SDK disabled every call is a no-op.
//
// Wire caps mirrored client-side (same as the Go SDK): messages clip to
// 8192 bytes, at most 50 fields per line with values clipped to 512 bytes,
// and at most 1000 lines per POST batch (one retry, then the batch is
// dropped).
//
// Requires linking src/logs.cpp alongside src/dataflow.cpp.

#pragma once

#include <cstddef>
#include <map>
#include <string>

#include "dataflow.hpp"

namespace dataflow {

// Client-side clamps mirroring the server's limits.
inline constexpr std::size_t kMaxLogMessageBytes = 8192;
inline constexpr std::size_t kMaxLogFields = 50;
inline constexpr std::size_t kMaxLogFieldValue = 512;

// Ships an application log line at a fixed level. The record is stamped with
// the current span's trace/span ids (empty when no trace is active on the
// calling thread), the current time (unix ms) and the configured service
// name. Field values are stringified as given; empty field keys are
// ignored. A no-op when the SDK is disabled. Never throws.
void LogDebug(const std::string& message,
              const std::map<std::string, std::string>& fields = {});
void LogInfo(const std::string& message,
             const std::map<std::string, std::string>& fields = {});
void LogWarn(const std::string& message,
             const std::map<std::string, std::string>& fields = {});
void LogError(const std::string& message,
              const std::map<std::string, std::string>& fields = {});

// Ships a log line at an explicit level: "debug" | "info" | "warn" |
// "error" (case-insensitive, surrounding whitespace ignored; "warning"
// maps to warn; anything unknown degrades to info rather than being
// dropped).
void Log(const std::string& level, const std::string& message,
         const std::map<std::string, std::string>& fields = {});

// Synchronously ships every buffered log line (batches of at most 1000,
// one retry per batch, failures dropped). A no-op when the SDK is disabled.
// The background flusher otherwise handles shipping on its own (500ms
// interval / 50-line threshold); explicit shutdown paths can call this
// before exit.
void FlushLogs();

namespace detail {

// --- test seams (internal; not part of the stable surface) -----------------

// When set, replaces the HTTP POST the log flusher issues: it receives the
// endpoint path and the JSON body and returns the HTTP status the flusher
// should act on (2xx = delivered; anything else — including 0, i.e.
// transport failure — is retried once and the batch then dropped).
using LogPostFn = long (*)(const std::string& path, const std::string& body);
extern LogPostFn g_log_post_for_test;

// Test-only gate: when paused, the background flusher stops taking lines
// (FlushLogs still drains) so tests can fill the buffer deterministically.
void pause_log_flusher_for_test(bool paused);

} // namespace detail

} // namespace dataflow
