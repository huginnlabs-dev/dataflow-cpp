# dataflow-cpp — HuginnLabs Dataflow SDK for C++

C++17 tracing SDK for the HuginnLabs Dataflow platform: RAII spans, a
thread-local trace context and a background sender that ships events to the
REST ingestion endpoint (`POST /api/v1/ingest`) — wire-compatible with the
Go/Python SDKs.

- **Windows:** WinHTTP + BCrypt, zero third-party dependencies.
- **Linux:** cpp-httplib (vendored header) + OpenSSL.

## Build

Add the header include dir and the single translation unit to your project:

```text
include/dataflow.hpp   — public API
src/dataflow.cpp       — implementation
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

## Tests

`tests/test_stmt_summary.cpp` is a standalone assert-based test for the
statement helpers:

```sh
# Windows (MinGW)
g++ -std=c++17 -Iinclude tests/test_stmt_summary.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o test_stmt_summary
# Linux
g++ -std=c++17 -Iinclude tests/test_stmt_summary.cpp src/dataflow.cpp -o test_stmt_summary

./test_stmt_summary
```
