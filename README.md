# dataflow-cpp — HuginnLabs Dataflow SDK for C++

C++17 tracing SDK for the HuginnLabs Dataflow platform: RAII spans, a
thread-local trace context and a background sender that ships events to the
REST ingestion endpoint (`POST /api/v1/ingest`) — wire-compatible with the
Go/Python SDKs.

- **Windows:** WinHTTP + BCrypt, zero third-party dependencies.
- **Linux:** cpp-httplib (vendored header) + OpenSSL.

## Build

Add the header include dir and the translation units to your project:

```text
include/dataflow.hpp      — public API
src/dataflow.cpp          — implementation
include/dataflow_crash.hpp — crash capture API (optional, see below)
src/crash.cpp             — crash capture implementation (optional)
include/dataflow_logs.hpp — application log shipping API (optional, see below)
src/logs.cpp              — application log shipping implementation (optional)
```

MSVC 2019+ and MinGW-w64 both build it. MinGW example:

```sh
g++ -std=c++17 -Iinclude your_app.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o your_app
```

## Quick start

```cpp
#include "dataflow.hpp"

int main() {
    dataflow::configure();                    // reads DATAFLOW_* env
    dataflow::Trace trace("scheduler.Run");   // RAII span, auto-end
    trace.span().set_data("job", "42");
    // ...
}                                             // nested Trace() joins the trace
```

Environment: `DATAFLOW_API_KEY`, `DATAFLOW_ENDPOINT` (http(s)://base),
`DATAFLOW_SERVICE_NAME`, `DATAFLOW_ENCRYPTION_KEY`, `DATAFLOW_SAMPLE_RATIO`,
`DATAFLOW_BUFFER_SIZE`, `DATAFLOW_DISABLED`, `DATAFLOW_APP_VERSION`,
`DATAFLOW_HTTP_URL` (base URL for the startup service manifest).
Without an API key/endpoint the SDK stays passive — every span call becomes
a no-op, so shipping instrumentation in production builds is safe.

`http_get` / `http_post` are convenience wrappers that issue the request and
record an `HTTP_CLIENT` span in one call. When you use your own HTTP or
database client, use the transport spans below.

## Transport spans

`dataflow::HttpSpan` and `dataflow::DbSpan` are RAII helpers for outgoing
HTTP calls and database queries. They are transport-agnostic: they issue no
I/O themselves — you keep libcurl, libpq, your ORM, whatever — and just
bracket the call. Both join the current thread's trace via the usual
`start_span` path, are passive when the SDK is disabled or the span is
unsampled, and their destructors never throw. The span ends with its
duration at scope exit; `set_status` / `record_error` refine it while the
request runs (last call wins).

### Outgoing HTTP — `HttpSpan`

An `HTTP_CLIENT` span named `"<METHOD> host/path"` with `callee_package` =
host and `http.method` / `http.url` metadata. Correlate with the receiving
service by sending the trace header with your request:

```cpp
#include "dataflow.hpp"
#include <curl/curl.h>

dataflow::configure();                                    // once at startup

dataflow::HttpSpan span("GET", "https://api.example.com/v1/users");

struct curl_slist* hdrs = nullptr;
std::string trace_hdr = "X-Dataflow-Trace-Id: " + span.trace_id();
hdrs = curl_slist_append(hdrs, trace_hdr.c_str());
curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
curl_easy_setopt(curl, CURLOPT_URL, "https://api.example.com/v1/users");
curl_easy_perform(curl);

long code = 0;
curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
span.set_status(static_cast<int>(code));      // or record_error on failure
// span ends (with duration) at scope exit
```

### Database queries — `DbSpan`

A `DB_QUERY` span derived from the SQL text: the name is `"<VERB> <table>"`
(e.g. `SELECT orders`, `INSERT users`, `CREATE audit_log` — first table
after `FROM|INTO|UPDATE|TABLE|JOIN`, skipping `IF [NOT] EXISTS`, with
schema qualifiers like `public.items` reduced to the bare table), the
callee package is the db system, and metadata carries `db.system` plus the
statement — whitespace-collapsed and clipped to 200 characters — in
`db.statement`.

**Bind values are never captured.** Pass only the SQL text to the SDK;
parameters stay in your driver call.

```cpp
#include "dataflow.hpp"
#include <libpq-fe.h>

dataflow::configure();                                    // once at startup

const char* sql = "SELECT id, total FROM public.orders WHERE customer_id = $1";
dataflow::DbSpan span("postgres", sql);       // name: "SELECT orders"

const char* params[1] = {customer_id};        // values stay in your code
PGresult* res = PQexecParams(conn, sql, 1, nullptr, params, nullptr, nullptr, 0);
if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    span.set_status(200);
} else {
    span.record_error(PQerrorMessage(conn));
    span.set_status(500);
}
// span ends (with duration) at scope exit
```

The two pure helpers behind the span name are public and unit-tested:

```cpp
std::string dataflow::stmt_summary("SELECT id FROM public.orders"); // "SELECT orders"
std::string dataflow::clip_statement("SELECT  *\n FROM  t");        // "SELECT * FROM t"
```

They mirror dataflow-go's `StmtSummary`/`ClipStatement`, so both SDKs group
identical statement shapes on the dashboard.

## Crash capture

C++ has no panics — the SDK targets uncaught exceptions and `std::terminate`.
`include/dataflow_crash.hpp` + `src/crash.cpp` (SDK 0.5.0+) add:

- `dataflow::Capture(fn)` — runs `fn()` and returns `true` on a clean run.
  When `fn` throws, the backtrace is captured **inside the catch** (so the
  failing call path is still on the stack), the crash is recorded and `false`
  is returned — the exception itself is consumed.
- `dataflow::CaptureException(e)` — record an exception you caught yourself;
  because the throw site's stack is gone by then, the caller's current stack
  is recorded (call it as close to the catch as possible).
- `dataflow::CaptureTerminate()` — installs a `std::terminate` handler that
  records a synthetic `terminate` span (with the active exception's `what()`
  when the runtime still hands it over, else `"terminate"`), synchronously
  flushes the submission pipeline, then chains to the previous terminate
  handler. Installing twice is a no-op.

Wire convention (both paths): the record lands on the currently-active span —
or a synthetic `exception` span when none is open — with `status_code = 500`,
`error_message` = the exception message clipped to 500 bytes, and metadata
`error.stack` = the backtrace text clipped to 8192 bytes
(`<backtrace unavailable>` when the platform refuses to cooperate).

```cpp
#include "dataflow.hpp"
#include "dataflow_crash.hpp"

int main() {
    dataflow::configure();                 // reads DATAFLOW_* env
    dataflow::CaptureTerminate();          // once, first thing in main()

    if (!dataflow::Capture([] {            // false when the body threw
        dataflow::Trace trace("scheduler.Run");
        run_scheduler();
    })) {
        // crash recorded; decide your own recovery/exit policy
    }
}
```

Stack capture is platform best-effort and adds no dependencies — Windows:
`RtlCaptureStackBackTrace` + `SymFromAddr` (dbghelp loaded at runtime with
`GetProcAddress`, never linked, every failure guarded down to raw module
addresses); POSIX: `backtrace()`/`backtrace_symbols()` from `<execinfo.h>`.
The whole crash path is wrapped so it never throws, and with the SDK
disabled everything is a passthrough (`fn` runs bare, the terminate handler
chains immediately).

## Log capture

`include/dataflow_logs.hpp` + `src/logs.cpp` (SDK 0.6.0+) ship application
logs with trace correlation: every line is stamped with the current span's
trace/span ids (when one is active on the calling thread), so logs line up
with traces in the dashboard's Logs tab.

- `dataflow::LogDebug / LogInfo / LogWarn / LogError(msg, fields)` — fixed
  levels; `fields` is a `std::map<std::string, std::string>`.
- `dataflow::Log(level, msg, fields)` — explicit level (`debug` | `info` |
  `warn` | `error`, case-insensitive; `warning` maps to `warn`; anything
  unknown degrades to `info`).
- `dataflow::FlushLogs()` — synchronously ship the buffered lines (explicit
  shutdown paths call it before exit; the background flusher otherwise
  handles shipping).

Lines are buffered in-process (1024 lines, drop-oldest on overflow) and a
background flusher POSTs them in batches to `{base}/api/v1/logs` (`X-Api-Key`
header, at most 1000 lines per batch, one retry per batch then the batch is
dropped) every 500ms or once 50 lines are buffered. Best-effort by contract:
logging never throws, never blocks the caller on I/O, and with the SDK
disabled every call is a no-op. The base URL follows the manifest's
resolution rule (`DATAFLOW_HTTP_URL` wins; otherwise the configured endpoint;
bare `host:port` endpoints get a derived `http://` scheme, matching the
startup manifest report).

Wire caps mirrored client-side, same as the Go SDK: messages clip to 8192
bytes, at most 50 fields per line with values clipped to 512 bytes.

```cpp
#include "dataflow.hpp"
#include "dataflow_logs.hpp"

int main() {
    dataflow::configure();                  // reads DATAFLOW_* env

    dataflow::Trace trace("scheduler.Run");
    dataflow::LogInfo("job started", {{"job", "42"}});
    // ... ids correlate with the active span ...
    dataflow::FlushLogs();                  // optional: ship before exit
}
```

## Route scanning

`dataflow scan` (SDK 0.4.0+) is a static route scanner: it extracts the HTTP
endpoints declared in a C++ codebase and publishes them to the server
catalog (`POST {base}/api/v1/catalog`, `X-Api-Key` header):

```json
{
  "service_name": "shop",
  "routes": [
    {"method": "GET", "path": "/api/orders/:id", "handler": "OrderController::show",
     "source_file": "src/controllers/orders.cpp"}
  ]
}
```

Extraction is line/regex based — routes must be declared with plain string
literal paths, which are kept as written (`:id` / `{id}` params included).
Supported shapes:

- **Crow** — `CROW_ROUTE(app, "/path")`, method taken from a
  `.methods(crow::HTTPMethod::POST)` / `CROW_HTTP_METHOD::POST` /
  `"POST"_method` chain, `GET` when the chain is absent;
- **cpp-httplib** — `svr.Get("/path", handler)` and
  `Post` / `Put` / `Delete` / `Head` / `Options` / `Patch`;
- **Pistache** — `routes().get("/path", handler)` and `router.post(...)`
  (lowercase verbs; `router.del` maps to `DELETE`);
- **Drogon** — `METHOD_ADD` / `ADD_METHOD_TO` macros (best effort; the first
  listed verb wins).

The handler is the macro/function name where trivially visible — anonymous
lambdas report `""`. Files scanned: `*.cpp *.cc *.cxx *.hpp *.h` under the
root, skipping `build/` and `.git/`; at most 1000 routes are posted.

### Build

```sh
# Windows (MinGW)
g++ -std=c++17 -Iinclude tools/scan_main.cpp src/scan.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o dataflow-scan
# Linux
g++ -std=c++17 -Iinclude tools/scan_main.cpp src/scan.cpp src/dataflow.cpp -o dataflow-scan
```

### Usage

```sh
dataflow-scan --dir src --service shop --url http://localhost:25080 --api-key KEY
dataflow-scan --dir . --print     # inspect the catalog JSON without posting
```

| flag | meaning | default |
|------|---------|---------|
| `--dir DIR` | source root to scan | `.` |
| `--service NAME` | service name | `$DATAFLOW_SERVICE_NAME`, else DIR basename |
| `--url URL` | server base URL | `$DATAFLOW_HTTP_URL`, else URL-form `$DATAFLOW_ENDPOINT` |
| `--api-key KEY` | API key | `$DATAFLOW_API_KEY` |
| `--print` | print the JSON to stdout instead of posting | off |

A bare `host:port` `DATAFLOW_ENDPOINT` (no scheme) is skipped with a message
on stderr, mirroring the SDK manifest rule. Exit status: `0` success
(including "no routes found" — nothing is posted then), `1` posting or
base-URL failure, `2` usage error.

## Tests

`tests/test_stmt_summary.cpp` (statement helpers), `tests/test_scan.cpp`
(route extraction + catalog JSON), `tests/test_crash.cpp` (crash capture) and
`tests/test_logs.cpp` (application log shipping) are standalone assert-based
tests:

```sh
# Windows (MinGW)
g++ -std=c++17 -Iinclude tests/test_stmt_summary.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o test_stmt_summary
g++ -std=c++17 -Iinclude tests/test_scan.cpp src/scan.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o test_scan
g++ -std=c++17 -Iinclude tests/test_crash.cpp src/crash.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o test_crash
g++ -std=c++17 -Iinclude tests/test_logs.cpp src/logs.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o test_logs
# Linux
g++ -std=c++17 -Iinclude tests/test_stmt_summary.cpp src/dataflow.cpp -o test_stmt_summary
g++ -std=c++17 -Iinclude tests/test_scan.cpp src/scan.cpp src/dataflow.cpp -o test_scan
g++ -std=c++17 -Iinclude tests/test_crash.cpp src/crash.cpp src/dataflow.cpp -o test_crash
g++ -std=c++17 -Iinclude tests/test_logs.cpp src/logs.cpp src/dataflow.cpp -o test_logs

./test_stmt_summary && ./test_scan && ./test_crash && ./test_logs
```

## Performance

The runtime overhead of every Dataflow SDK is measured with a uniform
benchmark: the same ~1 ms CPU-bound HTTP endpoint in three configs (no
instrumentation / Dataflow SDK / OpenTelemetry), one shared load driver,
spans exported live. Methodology, current numbers and reproduction steps:
Numbers are published in each SDK README as they are measured; the full harness lives in the Dataflow monorepo `bench/`.

Measured for this SDK (dockerized Debian/g++, cpp-httplib server, one
span per request, REST export live): baseline 1921 rps, **1982 rps
instrumented** - within the run-to-run noise floor; the export pipeline
runs on a background thread. No official OTEL C++ SDK exists, so the OTEL
comparison column is not applicable here.
