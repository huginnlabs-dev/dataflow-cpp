// dataflow_internal.hpp — declarations for implementation helpers defined in
// src/dataflow.cpp that the scan tool (src/scan.cpp) reuses instead of
// duplicating: URL parsing, JSON string escaping and the JSON POSTer shared
// with the startup service manifest. Not part of the public SDK surface —
// include it only from SDK sources.

#pragma once

#include "dataflow.hpp"

#include <string>
#include <utility>
#include <vector>

namespace dataflow {
namespace detail {

struct ParsedUrl {
    std::string scheme = "http";
    std::string host;
    int port = 80;
    std::string path = "/";
};

// "http://host:8080/base" -> {http, host, 8080, "/base"}
ParsedUrl parse_url(const std::string& url);

// JSON string escaping (quotes, backslashes, control characters) used by
// every hand-built request body.
std::string json_escape(const std::string& s);

// POSTs body as application/json with an x-api-key header; extra headers are
// appended. Returns false on transport failure (text in `error`); otherwise
// `status` carries the HTTP response code.
bool http_post_json(const ParsedUrl& ep, const std::string& path, const std::string& api_key,
                    const std::vector<std::pair<std::string, std::string>>& headers,
                    const std::string& body, long& status, std::string& response,
                    std::string& error, int timeout_secs = 10);

// The span currently active on this thread (nullptr when none) — exposed so
// the crash recorder (src/crash.cpp) can annotate the in-flight span without
// ending it (the public current_span() carries only the ids).
Span::Impl* current_impl();

// Best-effort synchronous flush of buffered spans to the ingest endpoint:
// posts the unacked batches and trims on ack, without the background loop's
// retry/backoff (the crash path cannot rely on the sender thread running
// again). Never throws; a transport failure just returns.
void flush_now();

} // namespace detail
} // namespace dataflow
