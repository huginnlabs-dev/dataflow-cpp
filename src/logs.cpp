// logs.cpp — application log shipping for the Dataflow C++ SDK: a
// mutex-protected bounded buffer of log lines, a background flusher thread
// (500ms interval / 50-line threshold) and the REST poster for
// POST /api/v1/logs. Kept in its own translation unit so processes that
// never log pay nothing: the flusher thread starts lazily on the first
// buffered line.
//
// The whole path is best-effort: logging never throws, never blocks the
// caller on I/O, and drops the oldest lines when the buffer overflows.

#include "dataflow_logs.hpp"
#include "dataflow_internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dataflow {
namespace {

// Tuning constants, mirroring dataflow-go's log pipeline.
constexpr std::size_t kLogBufferSize = 1024; // drop-oldest beyond this
constexpr std::size_t kLogFlushLines = 50;   // notify the flusher at this many
constexpr auto kLogFlushInterval = std::chrono::milliseconds(500);
constexpr std::size_t kLogMaxBatch = 1000; // server cap per POST

// One shipped application log line (wire shape of POST /api/v1/logs
// entries).
struct LogRecord {
    long long timestamp_ms = 0;
    std::string level;
    std::string message;
    std::string trace_id;
    std::string span_id;
    std::string service_name;
    std::map<std::string, std::string> fields;
};

std::mutex g_log_mu;
std::deque<LogRecord> g_log_buf;
std::condition_variable g_log_wake;
std::once_flag g_log_thread_once;
std::atomic<bool> g_log_flusher_paused{false};
std::atomic<long long> g_log_dropped{0};

long long log_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string clip_bytes(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    return s.substr(0, max_bytes);
}

// Maps input onto the wire vocabulary debug|info|warn|error (trimmed,
// case-insensitive; "warning" is an alias for warn). Unknown levels degrade
// to info rather than being dropped.
std::string normalize_level(const std::string& level) {
    static const char* kSpaces = " \t\r\n\f\v";
    const size_t begin = level.find_first_not_of(kSpaces);
    if (begin == std::string::npos) return "info";
    const size_t end = level.find_last_not_of(kSpaces);
    std::string lower;
    lower.reserve(end - begin + 1);
    for (size_t i = begin; i <= end; ++i) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(level[i])));
    }
    if (lower == "debug") return "debug";
    if (lower == "warn" || lower == "warning") return "warn";
    if (lower == "error") return "error";
    return "info";
}

// Base URL for the log endpoint — the manifest's resolution rule: env
// DATAFLOW_HTTP_URL wins (trailing slash trimmed); otherwise the configured
// endpoint when it carries an http(s) scheme. configure() derives "http://"
// for bare host:port values and that derived scheme is honored here, exactly
// like the startup manifest; with neither, there is no derivable base and
// logging stays silently off.
std::string logs_http_base() {
    std::string base;
    const char* over = std::getenv("DATAFLOW_HTTP_URL");
    if (over && *over) base = over;
    if (base.empty()) {
        const std::string& ep = settings().endpoint;
        if (ep.rfind("http://", 0) == 0 || ep.rfind("https://", 0) == 0) base = ep;
    }
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

// One JSON record in the contract's key order.
std::string record_to_json(const LogRecord& r) {
    std::string out = "{\"timestamp\":" + std::to_string(r.timestamp_ms) + ",";
    out += "\"level\":\"" + detail::json_escape(r.level) + "\",";
    out += "\"message\":\"" + detail::json_escape(r.message) + "\",";
    out += "\"trace_id\":\"" + detail::json_escape(r.trace_id) + "\",";
    out += "\"span_id\":\"" + detail::json_escape(r.span_id) + "\",";
    out += "\"service_name\":\"" + detail::json_escape(r.service_name) + "\",";
    out += "\"fields\":{";
    bool first = true;
    for (const auto& kv : r.fields) {
        if (!first) out += ",";
        first = false;
        out += "\"" + detail::json_escape(kv.first) + "\":\"" + detail::json_escape(kv.second) + "\"";
    }
    out += "}}";
    return out;
}

std::string batch_to_json(const std::vector<LogRecord>& batch) {
    std::string out = "{\"logs\":[";
    for (size_t i = 0; i < batch.size(); ++i) {
        if (i) out += ",";
        out += record_to_json(batch[i]);
    }
    out += "]}";
    return out;
}

// Sends one batch; one retry on failure, then the batch is dropped and
// counted. 2xx counts as delivered.
bool post_batch(const std::string& body) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        long status = 0;
        std::string response, error;
        if (detail::g_log_post_for_test) {
            status = detail::g_log_post_for_test("/api/v1/logs", body);
        } else {
            const std::string base = logs_http_base();
            if (base.empty()) return false; // no derivable base: nothing ships
            const detail::ParsedUrl ep = detail::parse_url(base);
            detail::http_post_json(ep, "/api/v1/logs", settings().api_key, {}, body, status,
                                   response, error);
        }
        if (status >= 200 && status < 300) return true;
    }
    return false;
}

// Drains the buffer in batches of at most kLogMaxBatch and POSTs them.
// Returns when the buffer is empty.
void flush_pending() {
    for (;;) {
        std::vector<LogRecord> batch;
        {
            std::lock_guard<std::mutex> lock(g_log_mu);
            if (g_log_buf.empty()) return;
            const size_t n = std::min(g_log_buf.size(), kLogMaxBatch);
            batch.assign(std::make_move_iterator(g_log_buf.begin()),
                         std::make_move_iterator(g_log_buf.begin() + static_cast<long>(n)));
            g_log_buf.erase(g_log_buf.begin(), g_log_buf.begin() + static_cast<long>(n));
        }
        if (!post_batch(batch_to_json(batch))) {
            g_log_dropped += static_cast<long long>(batch.size());
            std::fprintf(stderr, "dataflow: logs: dropped %d line(s) (post failed after retry)\n",
                         static_cast<int>(batch.size()));
        }
    }
}

void flusher_loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_log_mu);
            g_log_wake.wait_for(lock, kLogFlushInterval); // tick or threshold signal
        }
        if (g_log_flusher_paused.load(std::memory_order_relaxed)) continue;
        if (!enabled()) continue;
        flush_pending();
    }
}

void ensure_flusher_started() {
    std::call_once(g_log_thread_once, [] { std::thread(flusher_loop).detach(); });
}

// Buffers one record; on overflow the oldest line is dropped and counted.
void buffer_line(LogRecord&& rec) {
    bool full = false;
    {
        std::lock_guard<std::mutex> lock(g_log_mu);
        if (g_log_buf.size() >= kLogBufferSize) {
            g_log_buf.pop_front();
            g_log_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        g_log_buf.push_back(std::move(rec));
        full = g_log_buf.size() >= kLogFlushLines;
    }
    if (full) g_log_wake.notify_one();
}

} // namespace

namespace detail {

LogPostFn g_log_post_for_test = nullptr;

void pause_log_flusher_for_test(bool paused) { g_log_flusher_paused.store(paused); }

} // namespace detail

// ---------------------------------------------------------------------------
// public API

void Log(const std::string& level, const std::string& message,
         const std::map<std::string, std::string>& fields) {
    if (!enabled()) return; // disabled: no-op
    try {
        LogRecord rec;
        rec.timestamp_ms = log_now_ms();
        rec.level = normalize_level(level);
        rec.message = clip_bytes(message, kMaxLogMessageBytes);
        rec.service_name = settings().service_name;
        for (const auto& kv : fields) {
            if (kv.first.empty()) continue;
            if (rec.fields.size() >= kMaxLogFields) break;
            rec.fields.emplace(kv.first, clip_bytes(kv.second, kMaxLogFieldValue));
        }
        // Trace correlation: the span active on the calling thread (ids are
        // assigned before the span is published, but lock like crash.cpp).
        if (Span::Impl* cur = detail::current_impl()) {
            std::lock_guard<std::mutex> lock(cur->mu);
            rec.trace_id = cur->trace_id;
            rec.span_id = cur->span_id;
        }
        ensure_flusher_started();
        buffer_line(std::move(rec));
    } catch (...) {
        // Logging must never take the process down.
    }
}

void LogDebug(const std::string& message, const std::map<std::string, std::string>& fields) {
    Log("debug", message, fields);
}

void LogInfo(const std::string& message, const std::map<std::string, std::string>& fields) {
    Log("info", message, fields);
}

void LogWarn(const std::string& message, const std::map<std::string, std::string>& fields) {
    Log("warn", message, fields);
}

void LogError(const std::string& message, const std::map<std::string, std::string>& fields) {
    Log("error", message, fields);
}

void FlushLogs() {
    if (!enabled()) return;
    try {
        flush_pending();
    } catch (...) {
        // Best-effort: a flush failure must never surface.
    }
}

} // namespace dataflow
