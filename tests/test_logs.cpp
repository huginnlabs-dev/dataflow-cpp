// test_logs.cpp — standalone unit tests for application log shipping
// (dataflow::LogDebug/LogInfo/LogWarn/LogError/Log/FlushLogs). Posts are
// captured through the g_log_post_for_test seam in dataflow_logs.hpp and the
// background flusher is held behind the pause seam, so every assertion below
// is deterministic — the network is never touched except by the one test
// that exercises the real poster against 127.0.0.1:1 (nothing listens,
// connections fail fast):
//
//   Windows (MinGW):
//     g++ -std=c++17 -Iinclude tests/test_logs.cpp src/logs.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o test_logs
//   Linux:
//     g++ -std=c++17 -Iinclude tests/test_logs.cpp src/logs.cpp src/dataflow.cpp -o test_logs

#include "dataflow_logs.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    std::printf("FAIL %s\n  got:  \"%.400s\"\n  want: \"%.400s\"\n", what, got.c_str(),
                want.c_str());
}

// --- posted-body capture (the g_log_post_for_test seam) -----------------------

static std::mutex g_posts_mu;
static std::vector<std::string> g_paths;
static std::vector<std::string> g_bodies;
static long g_next_status = 200; // what the fake server answers
static int g_fail_next = 0;      // answer this many requests 500 first

static long fake_post(const std::string& path, const std::string& body) {
    std::lock_guard<std::mutex> lock(g_posts_mu);
    g_paths.push_back(path);
    g_bodies.push_back(body);
    if (g_fail_next > 0) {
        --g_fail_next;
        return 500;
    }
    return g_next_status;
}

static size_t total_posts() {
    std::lock_guard<std::mutex> lock(g_posts_mu);
    return g_bodies.size();
}

static std::string body_at(size_t i) {
    std::lock_guard<std::mutex> lock(g_posts_mu);
    return i < g_bodies.size() ? g_bodies[i] : std::string();
}

static bool all_paths_logs() {
    std::lock_guard<std::mutex> lock(g_posts_mu);
    for (const auto& p : g_paths) {
        if (p != "/api/v1/logs") return false;
    }
    return true;
}

// --- tiny JSON helpers (records always lead with "timestamp":) -----------------

// Splits a {"logs":[...]} body into its record fragments.
static std::vector<std::string> records_of(const std::string& body) {
    std::vector<std::string> out;
    const std::string marker = "{\"timestamp\":";
    size_t pos = body.find(marker);
    while (pos != std::string::npos) {
        const size_t start = pos + 1; // keep the leading '{'
        size_t end = body.find(marker, start);
        out.push_back(body.substr(start, end == std::string::npos ? end : end - start));
        pos = end;
    }
    return out;
}

static std::vector<std::string> records_since(size_t base) {
    std::vector<std::string> out;
    for (size_t i = base; i < total_posts(); ++i) {
        for (auto& r : records_of(body_at(i))) out.push_back(r);
    }
    return out;
}

// Value of a top-level string field ("key":"value") with \" skips.
static std::string json_field(const std::string& record, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t pos = record.find(needle);
    if (pos == std::string::npos) return "";
    std::string out;
    for (size_t i = pos + needle.size(); i < record.size(); ++i) {
        if (record[i] == '\\' && i + 1 < record.size()) {
            out += record[i];
            out += record[i + 1];
            ++i;
            continue;
        }
        if (record[i] == '"') break;
        out += record[i];
    }
    return out;
}

// Number immediately following a key with a numeric value.
static long long json_number(const std::string& record, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = record.find(needle);
    if (pos == std::string::npos) return -1;
    size_t i = pos + needle.size();
    std::string digits;
    while (i < record.size() && std::isdigit(static_cast<unsigned char>(record[i]))) {
        digits += record[i++];
    }
    return digits.empty() ? -1 : std::atoll(digits.c_str());
}

// The fields object contents of a record (fields is the record's last key).
static std::string fields_obj(const std::string& record) {
    const std::string key = "\"fields\":{";
    const size_t pos = record.find(key);
    if (pos == std::string::npos) return "";
    const size_t start = pos + key.size();
    const size_t end = record.find("}}", start);
    return record.substr(start, end == std::string::npos ? end : end - start);
}

static int fields_count(const std::string& record) {
    const std::string obj = fields_obj(record);
    if (obj.empty()) return 0;
    int n = 1;
    for (size_t i = 0; i + 1 < obj.size(); ++i) {
        if (obj[i] == ',' && obj[i + 1] == '"') ++n;
    }
    return n;
}

// Replaces each "timestamp":<digits> with "timestamp":T so bodies can be
// compared against a fixed expectation.
static std::string mask_timestamps(std::string body) {
    const std::string key = "\"timestamp\":";
    size_t pos = body.find(key);
    while (pos != std::string::npos) {
        size_t s = pos + key.size();
        size_t e = s;
        while (e < body.size() && std::isdigit(static_cast<unsigned char>(body[e]))) ++e;
        if (e == s) break; // not a number; stop masking
        body.replace(s, e - s, "T");
        pos = body.find(key, s + 1);
    }
    return body;
}

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

int main() {
    dataflow::detail::g_log_post_for_test = &fake_post;
    dataflow::detail::pause_log_flusher_for_test(true); // deterministic: FlushLogs posts

    // --- disabled SDK: everything is a no-op ---------------------------------
    dataflow::Settings off;
    off.disabled = true;
    dataflow::configure(off);

    dataflow::LogDebug("d");
    dataflow::LogInfo("i", {{"k", "v"}});
    dataflow::LogWarn("w");
    dataflow::LogError("e");
    dataflow::Log("info", "generic");
    dataflow::FlushLogs();
    check(total_posts() == 0, "disabled: Log*/Log/FlushLogs post nothing");

    // --- enabled SDK ----------------------------------------------------------
    dataflow::Settings on;
    on.api_key = "test-key";
    on.endpoint = "http://127.0.0.1:1"; // nothing listens; only the seam posts
    on.service_name = "log-tests";
    dataflow::configure(on);

    // Trace correlation + exact wire shape: one record inside a Trace (ids
    // attached), one outside (ids empty).
    std::string want_trace, want_span;
    {
        dataflow::Trace trace("log.Span");
        want_trace = trace.span().trace_id();
        want_span = trace.span().span_id();
        dataflow::LogInfo("order placed", {{"order_id", "42"}});
    }
    dataflow::LogInfo("bare");
    const long long stamp_floor = now_ms() - 60000;
    dataflow::FlushLogs();

    check(all_paths_logs(), "posts go to /api/v1/logs");
    const size_t shape_post = total_posts();
    check(shape_post == 1, "flush ships the pending lines in one batch");
    const std::string shape_body = body_at(shape_post - 1);
    check_eq(mask_timestamps(shape_body),
             std::string("{\"logs\":[{\"timestamp\":T,\"level\":\"info\","
                         "\"message\":\"order placed\",\"trace_id\":\"") +
                 want_trace + "\",\"span_id\":\"" + want_span +
                 "\",\"service_name\":\"log-tests\",\"fields\":{\"order_id\":\"42\"}},"
                 "{\"timestamp\":T,\"level\":\"info\",\"message\":\"bare\","
                 "\"trace_id\":\"\",\"span_id\":\"\",\"service_name\":\"log-tests\","
                 "\"fields\":{}}]}",
             "wire shape: ids, service_name, fields, empty-id record");
    {
        const std::vector<std::string> recs = records_of(shape_body);
        const long long ts = recs.empty() ? -1 : json_number(recs[0], "timestamp");
        check(ts >= stamp_floor && ts <= now_ms() + 60000, "timestamp is fresh epoch ms");
    }

    // Level normalization.
    dataflow::LogDebug("d");
    dataflow::LogInfo("i");
    dataflow::LogWarn("w");
    dataflow::LogError("e");
    dataflow::Log("WARN", "upper");
    dataflow::Log("  info ", "spaced");
    dataflow::Log("warning", "alias");
    dataflow::Log("banana", "unknown");
    dataflow::FlushLogs();
    {
        const std::vector<std::string> recs = records_since(shape_post);
        const char* want[] = {"debug", "info", "warn", "error", "warn", "info", "warn", "info"};
        bool ok = recs.size() == 8;
        for (size_t i = 0; ok && i < 8; ++i) ok = json_field(recs[i], "level") == want[i];
        check(ok, "levels normalize to debug|info|warn|error (unknown -> info)");
    }

    // Message clipped to 8192 bytes.
    dataflow::LogInfo(std::string(9000, 'x'));
    dataflow::FlushLogs();
    {
        const std::vector<std::string> recs = records_since(total_posts() - 1);
        check(recs.size() == 1 && json_field(recs[0], "message") == std::string(8192, 'x'),
              "message clipped to 8192 bytes");
    }

    // Fields: at most 50 entries, values clipped to 512 bytes.
    {
        const size_t base = total_posts();
        std::map<std::string, std::string> many;
        for (int i = 0; i < 60; ++i) many["k" + std::to_string(i)] = "v";
        dataflow::LogInfo("clamped", many);
        std::map<std::string, std::string> one;
        one["big"] = std::string(1000, 'y');
        dataflow::LogInfo("big value", one);
        dataflow::FlushLogs();
        const std::vector<std::string> recs = records_since(base);
        check(recs.size() == 2 && fields_count(recs[0]) == 50, "fields clamped to 50 entries");
        const std::string obj = recs.size() == 2 ? fields_obj(recs[1]) : std::string();
        check(obj.find(std::string(512, 'y')) != std::string::npos &&
                  obj.find(std::string(513, 'y')) == std::string::npos,
              "field value clipped to 512 bytes");
    }

    // JSON escaping rides on detail::json_escape.
    dataflow::LogInfo("quote\"back\\slash\nnewline");
    dataflow::FlushLogs();
    check(body_at(total_posts() - 1)
                  .find("\"message\":\"quote\\\"back\\\\slash\\nnewline\"") != std::string::npos,
          "message is JSON-escaped");

    // Drop-oldest overflow: 1074 lines through a 1024-line buffer.
    {
        const size_t base = total_posts();
        for (int i = 0; i < 1074; ++i) dataflow::LogInfo("m" + std::to_string(i));
        dataflow::FlushLogs();
        const std::vector<std::string> recs = records_since(base);
        bool capped = true;
        for (size_t i = base; i < total_posts(); ++i) {
            if (records_of(body_at(i)).size() > 1000) capped = false;
        }
        check(capped, "no batch exceeds the 1000-line server cap");
        check(recs.size() == 1024, "overflow keeps exactly 1024 lines (50 dropped)");
        check(!recs.empty() && json_field(recs.front(), "message") == "m50",
              "drop-oldest: oldest lines made room (first survivor m50)");
        check(!recs.empty() && json_field(recs.back(), "message") == "m1073",
              "drop-oldest: newest line kept");
    }

    // Retry once, then deliver.
    {
        const size_t base = total_posts();
        g_fail_next = 1;
        dataflow::LogInfo("survivor");
        dataflow::FlushLogs();
        check(total_posts() - base == 2, "failed batch is retried once");
        check_eq(body_at(base), body_at(base + 1), "retry resends the identical body");
    }

    // Always failing: two attempts, then the batch is dropped for good.
    {
        const size_t base = total_posts();
        g_next_status = 500;
        dataflow::LogWarn("doomed");
        dataflow::FlushLogs();
        check(total_posts() - base == 2, "always-failing batch attempts twice");
        g_next_status = 200;
        const size_t quiet = total_posts();
        dataflow::FlushLogs();
        check(total_posts() == quiet, "failed batch dropped, not re-queued");
    }

    // Background flusher: ships on its own without FlushLogs.
    dataflow::detail::pause_log_flusher_for_test(false);
    {
        const size_t base = total_posts();
        dataflow::LogInfo("background line");
        bool seen = false;
        for (int i = 0; i < 100 && !seen; ++i) { // up to ~5s, interval is 500ms
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> lock(g_posts_mu);
            for (size_t j = base; j < g_bodies.size(); ++j) {
                if (g_bodies[j].find("background line") != std::string::npos) seen = true;
            }
        }
        check(seen, "background flusher ships on its own (500ms interval)");
    }
    dataflow::detail::pause_log_flusher_for_test(true);

    // Real poster path (seam unset): the transport failure is swallowed and
    // the batch dropped — no hang, no throw.
    dataflow::detail::g_log_post_for_test = nullptr;
    dataflow::LogInfo("real post");
    dataflow::FlushLogs();
    dataflow::detail::g_log_post_for_test = &fake_post;
    {
        const size_t quiet = total_posts();
        dataflow::FlushLogs();
        check(total_posts() == quiet, "real-post failure dropped, buffer empty");
    }

    if (g_failures == 0) {
        std::printf("\nall tests passed\n");
        return 0;
    }
    std::printf("\n%d test(s) failed\n", g_failures);
    return 1;
}
