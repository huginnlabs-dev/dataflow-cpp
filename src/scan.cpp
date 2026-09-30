// scan.cpp — "dataflow scan": a static route scanner that extracts the
// declared HTTP endpoints of a C++ codebase and publishes them to the server
// catalog (POST {base}/api/v1/catalog, X-Api-Key header).
//
// Extraction is deliberately line/regex based — this is a cold developer
// tool, not a hot-path SDK feature — so routes must be declared with plain
// string literal paths. Supported shapes:
//
//   Crow        CROW_ROUTE(app, "/path") [.methods(Crow::HTTPMethod::POST |
//               CROW_HTTP_METHOD::POST | "POST"_method)] (handler)
//               — "GET" when no .methods() chain is present
//   cpp-httplib svr.Get("/path", handler) / Post / Put / Delete / ...
//   Pistache    routes().get("/path", handler) / router.get(...) (lowercase)
//   Drogon      METHOD_ADD(User::login, "/token", Get) /
//               ADD_METHOD_TO(User::detail, "/users/{1}", {Get, Post})
//
// The JSON body is hand-rolled with the SDK's shared json_escape; posting
// reuses the manifest's http_post_json + parse_url internals (see
// src/dataflow_internal.hpp).

#include "dataflow_scan.hpp"

#include "dataflow.hpp"
#include "dataflow_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <dirent.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

namespace dataflow {
namespace scan {
namespace {

constexpr std::size_t kMaxRoutes = 1000;
constexpr std::size_t kChainLimit = 800; // chars scanned after a route declaration

// --- small text helpers ------------------------------------------------------

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// Whitespace plus comments: the chain between CROW_ROUTE and its handler is
// allowed to be commented out around.
size_t skip_ws(const std::string& s, size_t i, size_t limit) {
    for (;;) {
        while (i < limit && is_space(s[i])) ++i;
        if (i + 1 < limit && s[i] == '/' && s[i + 1] == '/') {
            i += 2;
            while (i < limit && s[i] != '\n') ++i;
            continue;
        }
        if (i + 1 < limit && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < limit && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i = (i + 1 < limit) ? i + 1 : limit;
            continue;
        }
        return i;
    }
}

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string read_ident(const std::string& s, size_t& i, size_t limit) {
    const size_t start = i;
    while (i < limit && is_ident_char(s[i])) ++i;
    return s.substr(start, i - start);
}

// s[i] == '(' : returns the content range of the balanced group (string and
// comment aware) and the index just past ')'; false when unbalanced.
bool match_parens(const std::string& s, size_t i, size_t limit, size_t& content_begin,
                  size_t& content_end, size_t& after) {
    if (i >= limit || s[i] != '(') return false;
    int depth = 0;
    for (size_t j = i; j < limit; ++j) {
        const char c = s[j];
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++j;
            while (j < limit && s[j] != quote) {
                if (s[j] == '\\' && j + 1 < limit) ++j;
                ++j;
            }
            continue;
        }
        if (c == '/' && j + 1 < limit && s[j + 1] == '/') {
            while (j < limit && s[j] != '\n') ++j;
            continue;
        }
        if (c == '/' && j + 1 < limit && s[j + 1] == '*') {
            j += 2;
            while (j + 1 < limit && !(s[j] == '*' && s[j + 1] == '/')) ++j;
            j = (j + 1 < limit) ? j + 1 : limit;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (--depth == 0) {
                content_begin = i + 1;
                content_end = j;
                after = j + 1;
                return true;
            }
        }
    }
    return false;
}

std::string upper_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool known_method(const std::string& m) {
    static const char* const kMethods[] = {
        "GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS", "PATCH",
    };
    for (const char* v : kMethods) {
        if (m == v) return true;
    }
    return false;
}

// First Crow-style method token in a chain region: HTTPMethod::POST,
// HTTP_METHOD::POST or "POST"_method — first valid match wins.
std::string crow_method_in(const std::string& region) {
    static const std::regex scoped(R"((?:HTTPMethod|HTTP_METHOD)\s*::\s*([A-Za-z]+))");
    static const std::regex literal(R"("\s*([A-Za-z]+)\s*"\s*_method)");
    for (const std::regex* re : {&scoped, &literal}) {
        for (std::sregex_iterator it = std::sregex_iterator(region.begin(), region.end(), *re);
             it != std::sregex_iterator(); ++it) {
            std::string m = upper_ascii((*it)[1].str());
            if (known_method(m)) return m;
        }
    }
    return "";
}

// One call argument at pos: a plain (possibly ::-qualified) identifier that
// is not itself a call — the handler name. Lambdas ([...](...) / (...){},
// string literals, & / * bound names) report "".
std::string handler_arg_at(const std::string& s, size_t pos, size_t limit) {
    pos = skip_ws(s, pos, limit);
    if (pos >= limit) return "";
    const char c = s[pos];
    if (c == '[' || c == '(' || c == '{' || c == '"' || c == '\'' || c == '&' || c == '*' ||
        c == '.') {
        return "";
    }
    if (!is_ident_start(c)) return "";
    std::string name;
    for (;;) {
        name += read_ident(s, pos, limit);
        const size_t p = skip_ws(s, pos, limit);
        if (p + 1 < limit && s[p] == ':' && s[p + 1] == ':') {
            name += "::";
            pos = skip_ws(s, p + 2, limit);
            continue;
        }
        pos = p;
        break;
    }
    if (pos < limit && (s[pos] == '(' || s[pos] == '<')) return ""; // call/template expression
    if (pos < limit && s[pos] != ',' && s[pos] != ')') return "";
    return name;
}

// Handler argument following a route path literal: ", name" -> name.
std::string handler_after_path(const std::string& s, size_t pos, size_t limit) {
    pos = skip_ws(s, pos, limit);
    if (pos >= limit || s[pos] != ',') return "";
    return handler_arg_at(s, pos + 1, limit);
}

// --- Crow ----------------------------------------------------------------------

void scan_crow(const std::string& src, const std::string& file, std::vector<ScanRoute>& out) {
    static const std::regex re(R"re(CROW_ROUTE\s*\(\s*[^,()]+,\s*"(/[^"]+)"\s*\))re");
    for (std::sregex_iterator it = std::sregex_iterator(src.begin(), src.end(), re);
         it != std::sregex_iterator(); ++it) {
        ScanRoute r;
        r.method = "GET";
        r.path = (*it)[1].str();
        r.source_file = file;
        // Walk the trailing chain: .name(...) / .methods(...) / (handler).
        size_t pos = static_cast<size_t>(it->position(0)) + it->length(0);
        const size_t limit = std::min(src.size(), pos + kChainLimit);
        for (;;) {
            pos = skip_ws(src, pos, limit);
            if (pos >= limit || src[pos] == ';') break;
            if (src[pos] == '.') {
                ++pos;
                const std::string chain = read_ident(src, pos, limit);
                size_t b = 0, e = 0, after = 0;
                if (!match_parens(src, pos, limit, b, e, after)) break;
                if (chain == "methods" && r.method == "GET") {
                    const std::string m = crow_method_in(src.substr(b, e - b));
                    if (!m.empty()) r.method = m;
                }
                pos = after;
                continue;
            }
            if (src[pos] == '(') {
                size_t b = 0, e = 0, after = 0;
                if (match_parens(src, pos, limit, b, e, after)) {
                    r.handler = handler_arg_at(src, b, e);
                }
                break;
            }
            break;
        }
        out.push_back(std::move(r));
    }
}

// --- cpp-httplib -----------------------------------------------------------------
// Capitalized verbs (svr.Get("/path", handler)) — Post/Put/Delete/Head/
// Options/Patch likewise.

void scan_httplib(const std::string& src, const std::string& file, std::vector<ScanRoute>& out) {
    static const std::regex re(R"re(\.\s*(Get|Post|Put|Delete|Head|Options|Patch)\s*\(\s*"(/[^"]*)")re");
    for (std::sregex_iterator it = std::sregex_iterator(src.begin(), src.end(), re);
         it != std::sregex_iterator(); ++it) {
        ScanRoute r;
        r.method = upper_ascii((*it)[1].str());
        r.path = (*it)[2].str();
        r.source_file = file;
        r.handler = handler_after_path(src, static_cast<size_t>(it->position(0)) + it->length(0),
                                       src.size());
        out.push_back(std::move(r));
    }
}

// --- Pistache ---------------------------------------------------------------------
// Lowercase verbs on routes() or a *router receiver: routes().get("/path",
// handler), router.post(...), router.del(...) -> DELETE.

void scan_pistache(const std::string& src, const std::string& file, std::vector<ScanRoute>& out) {
    static const std::regex re(
        R"re((?:routes\s*\(\s*\)|\w*[Rr][Oo]uter\w*)\s*\.\s*(get|post|put|del|delete|head|options|patch)\s*\(\s*"(/[^"]*)")re");
    for (std::sregex_iterator it = std::sregex_iterator(src.begin(), src.end(), re);
         it != std::sregex_iterator(); ++it) {
        ScanRoute r;
        const std::string verb = (*it)[1].str();
        r.method = upper_ascii(verb == "del" ? "delete" : verb);
        r.path = (*it)[2].str();
        r.source_file = file;
        r.handler = handler_after_path(src, static_cast<size_t>(it->position(0)) + it->length(0),
                                       src.size());
        out.push_back(std::move(r));
    }
}

// --- Drogon (best effort) -----------------------------------------------------------
// METHOD_ADD(User::login, "/token", Get) and ADD_METHOD_TO(User::detail,
// "/users/{1}", {Get, Post}) — first method token wins, GET when absent.

void scan_drogon(const std::string& src, const std::string& file, std::vector<ScanRoute>& out) {
    static const std::regex re(
        R"re(\b(METHOD_ADD|ADD_METHOD_TO)\s*\(\s*([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)\s*,\s*"(/[^"]*)")re");
    static const std::regex verb(R"re(\b(Get|Post|Put|Delete|Head|Options|Patch)\b)re");
    for (std::sregex_iterator it = std::sregex_iterator(src.begin(), src.end(), re);
         it != std::sregex_iterator(); ++it) {
        ScanRoute r;
        // The handler macro argument may carry spaces around '::'.
        std::string handler;
        for (const char c : (*it)[2].str()) {
            if (!is_space(c)) handler += c;
        }
        r.handler = handler;
        r.path = (*it)[3].str();
        r.source_file = file;
        r.method = "GET";
        size_t pos = static_cast<size_t>(it->position(0)) + it->length(0);
        const size_t limit = std::min(src.size(), pos + 200);
        size_t end = pos;
        while (end < limit && src[end] != ')' && src[end] != ';') ++end;
        for (std::sregex_iterator v = std::sregex_iterator(src.begin() + static_cast<std::ptrdiff_t>(pos),
                                                           src.begin() + static_cast<std::ptrdiff_t>(end), verb);
             v != std::sregex_iterator(); ++v) {
            r.method = upper_ascii(v->str());
            break;
        }
        out.push_back(std::move(r));
    }
}

// --- files ----------------------------------------------------------------------------

bool has_scan_ext(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = upper_ascii(name.substr(dot + 1));
    return ext == "CPP" || ext == "CC" || ext == "CXX" || ext == "HPP" || ext == "H";
}

bool is_skipped_dir(const std::string& name) { return name == "build" || name == ".git"; }

// Collects (absolute, scan-root-relative) paths of C++ sources, '/'-joined.
void walk_tree(const std::string& abs_dir, const std::string& rel_dir,
               std::vector<std::pair<std::string, std::string>>& files) {
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    const std::string pattern = abs_dir + "/*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        const std::string child_abs = abs_dir + "/" + name;
        const std::string child_rel = rel_dir.empty() ? name : rel_dir + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!is_skipped_dir(name)) walk_tree(child_abs, child_rel, files);
        } else if (has_scan_ext(name)) {
            files.emplace_back(child_abs, child_rel);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(abs_dir.c_str());
    if (!d) return;
    struct dirent* de = nullptr;
    while ((de = readdir(d)) != nullptr) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        const std::string child_abs = abs_dir + "/" + name;
        const std::string child_rel = rel_dir.empty() ? name : rel_dir + "/" + name;
        struct stat st {};
        if (::stat(child_abs.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!is_skipped_dir(name)) walk_tree(child_abs, child_rel, files);
        } else if (has_scan_ext(name)) {
            files.emplace_back(child_abs, child_rel);
        }
    }
    closedir(d);
#endif
}

bool is_directory(const std::string& path) {
#ifdef _WIN32
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool read_whole_file(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

// --- CLI helpers -----------------------------------------------------------------------

std::string env_str(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

std::string trim_slashes(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

// --url > $DATAFLOW_HTTP_URL > URL-form $DATAFLOW_ENDPOINT. A bare host:port
// DATAFLOW_ENDPOINT (no scheme) is skipped with a stderr note, mirroring the
// SDK manifest's base-URL rule.
std::string resolve_base_url(const std::string& explicit_url) {
    if (!explicit_url.empty()) return trim_slashes(explicit_url);
    std::string base = trim_slashes(env_str("DATAFLOW_HTTP_URL"));
    if (!base.empty()) return base;
    const std::string endpoint = env_str("DATAFLOW_ENDPOINT");
    if (endpoint.empty()) return "";
    if (endpoint.rfind("http://", 0) == 0 || endpoint.rfind("https://", 0) == 0) {
        return trim_slashes(endpoint);
    }
    std::fprintf(stderr,
                 "dataflow scan: DATAFLOW_ENDPOINT '%s' is not URL-form (http(s)://...); "
                 "pass --url or DATAFLOW_HTTP_URL\n",
                 endpoint.c_str());
    return "";
}

std::string working_dir_name() {
    char buf[4096];
#ifdef _WIN32
    const DWORD n = GetCurrentDirectoryA(sizeof(buf), buf);
    if (n == 0 || n >= sizeof(buf)) return "unknown-service";
#else
    if (!getcwd(buf, sizeof(buf))) return "unknown-service";
#endif
    std::string cwd(buf);
    const size_t cut = cwd.find_last_of("/\\");
    std::string base = (cut == std::string::npos) ? cwd : cwd.substr(cut + 1);
    return base.empty() ? "unknown-service" : base;
}

std::string dir_basename(const std::string& dir) {
    std::string s = dir;
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    const size_t cut = s.find_last_of("/\\");
    std::string base = (cut == std::string::npos) ? s : s.substr(cut + 1);
    if (base.empty() || base == "." || base == "..") return working_dir_name();
    return base;
}

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "dataflow scan (cpp-sdk %s) — publish a static HTTP route catalog\n"
                 "\n"
                 "usage: dataflow-scan [--dir DIR] [--service NAME] [--url URL]\n"
                 "                     [--api-key KEY] [--print]\n"
                 "\n"
                 "  --dir DIR        source root to scan (default \".\")\n"
                 "  --service NAME   service name (default $DATAFLOW_SERVICE_NAME or DIR basename)\n"
                 "  --url URL        server base URL (default $DATAFLOW_HTTP_URL, then\n"
                 "                   URL-form $DATAFLOW_ENDPOINT)\n"
                 "  --api-key KEY    API key (default $DATAFLOW_API_KEY)\n"
                 "  --print          print the catalog JSON to stdout instead of posting\n"
                 "\n"
                 "Scans *.cpp *.cc *.cxx *.hpp *.h under DIR (skipping build/ and .git/),\n"
                 "extracts Crow / cpp-httplib / Pistache / Drogon routes and POSTs at most\n"
                 "%d of them to {base}/api/v1/catalog.\n"
                 "\n"
                 "exit status: 0 ok (incl. no routes found), 1 post/base-URL failure, 2 usage error\n",
                 kVersion, static_cast<int>(kMaxRoutes));
}

} // namespace

// --- public API --------------------------------------------------------------------------

std::vector<ScanRoute> extract_routes(const std::string& source, const std::string& source_file) {
    std::vector<ScanRoute> routes;
    routes.reserve(32);
    scan_crow(source, source_file, routes);
    scan_httplib(source, source_file, routes);
    scan_pistache(source, source_file, routes);
    scan_drogon(source, source_file, routes);
    std::sort(routes.begin(), routes.end(), [](const ScanRoute& a, const ScanRoute& b) {
        if (a.source_file != b.source_file) return a.source_file < b.source_file;
        if (a.path != b.path) return a.path < b.path;
        if (a.method != b.method) return a.method < b.method;
        return a.handler < b.handler;
    });
    routes.erase(std::unique(routes.begin(), routes.end(),
                             [](const ScanRoute& a, const ScanRoute& b) {
                                 return a.method == b.method && a.path == b.path &&
                                        a.handler == b.handler && a.source_file == b.source_file;
                             }),
                 routes.end());
    return routes;
}

std::string build_catalog_json(const std::string& service_name, const std::vector<ScanRoute>& routes,
                               std::size_t max_routes) {
    std::string json = "{\"service_name\":\"" + detail::json_escape(service_name) + "\",\"routes\":[";
    std::size_t count = 0;
    for (const ScanRoute& r : routes) {
        if (count >= max_routes) break;
        if (count > 0) json += ",";
        json += "{\"method\":\"" + detail::json_escape(r.method) + "\",";
        json += "\"path\":\"" + detail::json_escape(r.path) + "\",";
        json += "\"handler\":\"" + detail::json_escape(r.handler) + "\",";
        json += "\"source_file\":\"" + detail::json_escape(r.source_file) + "\"}";
        ++count;
    }
    json += "]}";
    return json;
}

int scan_main(int argc, char** argv) {
    std::string dir = ".";
    std::string service;
    std::string url;
    std::string api_key;
    bool print_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--print") {
            print_only = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(stdout);
            return 0;
        } else if (arg == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (arg == "--service" && i + 1 < argc) {
            service = argv[++i];
        } else if (arg == "--url" && i + 1 < argc) {
            url = argv[++i];
        } else if (arg == "--api-key" && i + 1 < argc) {
            api_key = argv[++i];
        } else if (arg == "--dir" || arg == "--service" || arg == "--url" || arg == "--api-key") {
            std::fprintf(stderr, "dataflow scan: %s requires a value\n", arg.c_str());
            return 2;
        } else {
            std::fprintf(stderr, "dataflow scan: unknown argument '%s' (try --help)\n", arg.c_str());
            return 2;
        }
    }

    if (!is_directory(dir)) {
        std::fprintf(stderr, "dataflow scan: not a directory: %s\n", dir.c_str());
        return 2;
    }
    if (service.empty()) service = env_str("DATAFLOW_SERVICE_NAME");
    if (service.empty()) service = dir_basename(dir);

    std::vector<std::pair<std::string, std::string>> files; // (abs, scan-root relative)
    walk_tree(dir, "", files);
    std::sort(files.begin(), files.end(),
              [](const std::pair<std::string, std::string>& a,
                 const std::pair<std::string, std::string>& b) { return a.second < b.second; });

    std::vector<ScanRoute> routes;
    std::size_t files_scanned = 0;
    for (const auto& f : files) {
        std::string text;
        if (!read_whole_file(f.first, text)) {
            std::fprintf(stderr, "dataflow scan: cannot read %s\n", f.first.c_str());
            continue;
        }
        ++files_scanned;
        const std::vector<ScanRoute> found = extract_routes(text, f.second);
        routes.insert(routes.end(), found.begin(), found.end());
    }
    if (routes.size() > kMaxRoutes) {
        std::fprintf(stderr, "dataflow scan: %d routes found; posting the first %d only\n",
                     static_cast<int>(routes.size()), static_cast<int>(kMaxRoutes));
    }

    if (print_only) {
        std::printf("%s\n", build_catalog_json(service, routes, kMaxRoutes).c_str());
        return 0;
    }

    if (routes.empty()) {
        std::fprintf(stderr, "dataflow scan: no routes found under '%s'; nothing to publish\n",
                     dir.c_str());
        return 0;
    }

    const std::string base = resolve_base_url(url);
    if (base.empty()) {
        std::fprintf(stderr, "dataflow scan: no base URL; pass --url or set DATAFLOW_HTTP_URL\n");
        return 1;
    }
    if (api_key.empty()) api_key = env_str("DATAFLOW_API_KEY");
    if (api_key.empty()) {
        std::fprintf(stderr, "dataflow scan: warning: no API key (--api-key / DATAFLOW_API_KEY)\n");
    }

    const std::string body = build_catalog_json(service, routes, kMaxRoutes);
    long status = 0;
    std::string response, error;
    detail::http_post_json(detail::parse_url(base), "/api/v1/catalog", api_key, {}, body, status,
                           response, error, 15);
    if (!error.empty() || status < 200 || status >= 300) {
        std::fprintf(stderr, "dataflow scan: catalog post failed (HTTP %ld)%s%s\n", status,
                     error.empty() ? "" : ": ", error.c_str());
        return 1;
    }
    std::printf("dataflow scan: %d route(s) from %d file(s) posted for '%s' to %s/api/v1/catalog "
                "(HTTP %ld)\n",
                static_cast<int>(std::min<std::size_t>(routes.size(), kMaxRoutes)),
                static_cast<int>(files_scanned), service.c_str(), base.c_str(), status);
    return 0;
}

} // namespace scan
} // namespace dataflow
