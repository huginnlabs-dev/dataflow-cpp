// dataflow_scan.hpp — "dataflow scan": static HTTP route extraction for the
// Dataflow catalog (POST {base}/api/v1/catalog).
//
//   dataflow-scan --dir src --service shop --url http://host:25080 --api-key KEY
//   dataflow-scan --dir . --print        # inspect the JSON, post nothing
//
// Frameworks: Crow (CROW_ROUTE + .methods() chains), cpp-httplib
// (svr.Get/Post/Put/...), Pistache (routes().get / router.get) and Drogon
// (METHOD_ADD / ADD_METHOD_TO, best effort). Extraction is line/regex based:
// paths must be plain string literals and are kept as written (":id" /
// "{id}" params included). The handler is the macro/function name when
// trivially visible, "" otherwise (e.g. anonymous lambdas).

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dataflow {
namespace scan {

struct ScanRoute {
    std::string method;      // uppercase verb, "GET" when undeclared
    std::string path;        // starts with '/'
    std::string handler;     // "" when anonymous (lambda) or not visible
    std::string source_file; // scan-root relative, '/'-separated
};

// Extracts the routes declared in one translation unit. Pure (no I/O):
// results are sorted and de-duplicated so callers can diff catalogs.
std::vector<ScanRoute> extract_routes(const std::string& source,
                                      const std::string& source_file);

// Hand-rolled request body: {"service_name":"...","routes":[...]} — at most
// max_routes entries (server limit: 1000).
std::string build_catalog_json(const std::string& service_name,
                               const std::vector<ScanRoute>& routes,
                               std::size_t max_routes = 1000);

// CLI entry (tools/scan_main.cpp is a main() that just calls this):
//
//   dataflow-scan [--dir DIR] [--service NAME] [--url URL] [--api-key KEY] [--print]
//
// Base URL: --url > $DATAFLOW_HTTP_URL > URL-form $DATAFLOW_ENDPOINT (a bare
// host:port endpoint is skipped with a stderr note). API key: --api-key or
// $DATAFLOW_API_KEY. Returns 0 on success, 1 on post/base-URL failure, 2 on
// usage errors.
int scan_main(int argc, char** argv);

} // namespace scan
} // namespace dataflow
