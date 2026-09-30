// test_scan.cpp — standalone unit tests for the static route scanner
// (dataflow::scan::extract_routes / build_catalog_json). The extraction is
// pure, but it escapes JSON with the helper that lives in the SDK
// translation unit, so the test links it:
//
//   Windows (MinGW):
//     g++ -std=c++17 -Iinclude tests/test_scan.cpp src/scan.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o test_scan
//   Linux:
//     g++ -std=c++17 -Iinclude tests/test_scan.cpp src/scan.cpp src/dataflow.cpp -o test_scan
//
// Fixtures below mirror the server catalog contract: {"method":"GET",
// "path":"/api/orders/:id","handler":"OrderController::show"} with paths and
// params kept as written.

#include "dataflow_scan.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

static void check_eq(const std::string& got, const std::string& want, const char* what) {
    if (got == want) {
        std::printf("ok   %-64s -> %s\n", what, got.c_str());
        return;
    }
    ++g_failures;
    std::printf("FAIL %s\n  got:  \"%s\"\n  want: \"%s\"\n", what, got.c_str(), want.c_str());
}

static std::vector<dataflow::scan::ScanRoute> scan(const char* source) {
    return dataflow::scan::extract_routes(source, "src/main.cpp");
}

static void check_route(const std::vector<dataflow::scan::ScanRoute>& got, size_t idx,
                        const char* method, const char* path, const char* handler,
                        const char* what) {
    if (idx >= got.size()) {
        ++g_failures;
        std::printf("FAIL %s\n  no route at index %d (only %d extracted)\n", what,
                    static_cast<int>(idx), static_cast<int>(got.size()));
        return;
    }
    const dataflow::scan::ScanRoute& r = got[idx];
    if (r.method == method && r.path == path && r.handler == handler) {
        std::printf("ok   %-64s -> %s %s (%s)\n", what, r.method.c_str(), r.path.c_str(),
                    r.handler.empty() ? "anonymous" : r.handler.c_str());
        return;
    }
    ++g_failures;
    std::printf("FAIL %s\n  got:  %s %s handler=\"%s\"\n  want: %s %s handler=\"%s\"\n", what,
                r.method.c_str(), r.path.c_str(), r.handler.c_str(), method, path, handler);
}

// --- fixtures ----------------------------------------------------------------

static const char* kCrowSrc = R"cpp(
// shop service route declarations
CROW_ROUTE(app, "/health")
([]{ return "ok"; });

CROW_ROUTE(app, "/api/orders/:id")
    .methods(crow::HTTPMethod::POST)(order_show);

CROW_ROUTE(app, "/users")
    .name("users_index")
    .methods(CROW_HTTP_METHOD::PUT)
    (users_put);

CROW_ROUTE(app, "/v1/things").methods("PATCH"_method)([](const crow::request& req) {
    return crow::response(200);
});
)cpp";

static const char* kHttplibSrc = R"cpp(
int main() {
    httplib::Server svr;
    svr.Get("/api/orders/:id", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content("order", "text/plain");
    });
    svr.Post("/api/orders", create_order);
    server.Delete("/cache", purge_cache);
    svr.Put("/settings/:key", update_setting);
    return 0;
}
)cpp";

static const char* kPistacheSrc = R"cpp(
class PingService {
  public:
    void setup_routes(Pistache::Rest::Router& router) {
        routes().get("/ping", Routes::Get::ping);
        router.post("/events", [](const Rest::Request& req, Http::ResponseWriter response) {
            response.send(Http::Code::Ok, "queued");
        });
        router.del("/events/:id", Routes::Delete::drop);
    }
};
)cpp";

static const char* kDrogonSrc = R"cpp(
class UserController : public drogon::HttpController<UserController> {
  public:
    METHOD_LIST_BEGIN
    METHOD_ADD(UserController::login, "/token?userId={1}&passwd={2}", Get);
    ADD_METHOD_TO(UserController::detail, "/users/{1}", {Get, Post});
    METHOD_LIST_END
};
)cpp";

static const char* kMixedDupSrc = R"cpp(
CROW_ROUTE(app, "/dup")([]{ return 1; });
CROW_ROUTE(app, "/dup")([]{ return 2; });
svr.Get("/only", only_handler);
)cpp";

static const char* kNoRoutesSrc = R"cpp(
#include <string>
int add(int a, int b) { return a + b; }
config.get("/enable/feature", fallback);   // not a route declaration
)cpp";

int main() {
    // --- Crow ---------------------------------------------------------------
    const std::vector<dataflow::scan::ScanRoute> crow = scan(kCrowSrc);
    check_route(crow, 0, "POST", "/api/orders/:id", "order_show", "crow: methods() + named handler");
    check_route(crow, 1, "GET", "/health", "", "crow: default GET, anonymous lambda");
    check_route(crow, 2, "PUT", "/users", "users_put", "crow: CROW_HTTP_METHOD chain");
    check_route(crow, 3, "PATCH", "/v1/things", "", "crow: \"PATCH\"_method literal");

    // --- cpp-httplib ------------------------------------------------------------
    const std::vector<dataflow::scan::ScanRoute> httplib = scan(kHttplibSrc);
    check_route(httplib, 0, "POST", "/api/orders", "create_order", "httplib: Post named handler");
    check_route(httplib, 1, "GET", "/api/orders/:id", "", "httplib: Get lambda anonymous");
    check_route(httplib, 2, "DELETE", "/cache", "purge_cache", "httplib: Delete named handler");
    check_route(httplib, 3, "PUT", "/settings/:key", "update_setting", "httplib: Put param path");

    // --- Pistache -----------------------------------------------------------------
    const std::vector<dataflow::scan::ScanRoute> pistache = scan(kPistacheSrc);
    check_route(pistache, 0, "POST", "/events", "", "pistache: router.post lambda");
    check_route(pistache, 1, "DELETE", "/events/:id", "Routes::Delete::drop", "pistache: router.del -> DELETE");
    check_route(pistache, 2, "GET", "/ping", "Routes::Get::ping", "pistache: routes().get qualified handler");

    // --- Drogon (best effort) ---------------------------------------------------------
    const std::vector<dataflow::scan::ScanRoute> drogon = scan(kDrogonSrc);
    check_route(drogon, 0, "GET", "/token?userId={1}&passwd={2}", "UserController::login",
                "drogon: METHOD_ADD query path kept as written");
    check_route(drogon, 1, "GET", "/users/{1}", "UserController::detail",
                "drogon: ADD_METHOD_TO {Get, Post} -> first verb");
    check_eq(drogon.empty() ? std::string() : drogon[0].source_file, "src/main.cpp",
             "drogon: source_file recorded");

    // --- sorting, dedup, no false positives ----------------------------------------------
    const std::vector<dataflow::scan::ScanRoute> mixed = scan(kMixedDupSrc);
    if (mixed.size() == 2) {
        std::printf("ok   %-64s -> %d routes\n", "mixed: duplicate collapsed to one",
                    static_cast<int>(mixed.size()));
    } else {
        ++g_failures;
        std::printf("FAIL mixed: duplicate collapsed to one, got %d routes\n",
                    static_cast<int>(mixed.size()));
    }
    check_route(mixed, 0, "GET", "/dup", "", "mixed: sorted first by path");
    check_route(mixed, 1, "GET", "/only", "only_handler", "mixed: second route");

    const std::vector<dataflow::scan::ScanRoute> none = scan(kNoRoutesSrc);
    if (none.empty()) {
        std::printf("ok   %-64s\n", "plain code: no routes extracted");
    } else {
        ++g_failures;
        std::printf("FAIL plain code: no routes extracted, got %d\n", static_cast<int>(none.size()));
    }

    // --- catalog JSON body ----------------------------------------------------------------
    const std::vector<dataflow::scan::ScanRoute> catalog_routes = {
        {"GET", "/health", "", "src/main.cpp"},
        {"POST", "/orders", "create_order", "src/orders.cpp"},
    };
    check_eq(dataflow::scan::build_catalog_json("shop", catalog_routes),
             std::string("{\"service_name\":\"shop\",\"routes\":[")
                 + "{\"method\":\"GET\",\"path\":\"/health\",\"handler\":\"\","
                   "\"source_file\":\"src/main.cpp\"},"
                   "{\"method\":\"POST\",\"path\":\"/orders\",\"handler\":\"create_order\","
                   "\"source_file\":\"src/orders.cpp\"}]}",
             "catalog json shape");

    const std::vector<dataflow::scan::ScanRoute> esc_routes = {
        {"GET", "/x", "quote\"slash\\line\n", "a.cpp"},
    };
    check_eq(dataflow::scan::build_catalog_json("svc", esc_routes),
             std::string("{\"service_name\":\"svc\",\"routes\":[{\"method\":\"GET\",\"path\":\"/x\","
                         "\"handler\":\"quote\\\"slash\\\\line\\n\",\"source_file\":\"a.cpp\"}]}"),
             "catalog json escaping");

    const std::vector<dataflow::scan::ScanRoute> three = {
        {"GET", "/a", "", "f.cpp"},
        {"GET", "/b", "", "f.cpp"},
        {"GET", "/c", "", "f.cpp"},
    };
    const std::string capped = dataflow::scan::build_catalog_json("s", three, 2);
    if (capped.find("\"path\":\"/c\"") == std::string::npos &&
        capped.find("\"path\":\"/a\"") != std::string::npos &&
        capped.find("\"path\":\"/b\"") != std::string::npos) {
        std::printf("ok   %-64s\n", "catalog json: max_routes cap honored");
    } else {
        ++g_failures;
        std::printf("FAIL catalog json: max_routes cap honored -> %s\n", capped.c_str());
    }

    if (g_failures == 0) {
        std::printf("\nall tests passed\n");
        return 0;
    }
    std::printf("\n%d test(s) failed\n", g_failures);
    return 1;
}
