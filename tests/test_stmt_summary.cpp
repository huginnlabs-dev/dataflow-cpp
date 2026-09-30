// test_stmt_summary.cpp — standalone unit tests for the DB_QUERY statement
// helpers (dataflow::stmt_summary / dataflow::clip_statement). The helpers
// are pure, but they live in the SDK translation unit, so the test links it:
//
//   Windows (MinGW):
//     g++ -std=c++17 -Iinclude tests/test_stmt_summary.cpp src/dataflow.cpp -lwinhttp -lbcrypt -o test_stmt_summary
//   Linux:
//     g++ -std=c++17 -Iinclude tests/test_stmt_summary.cpp src/dataflow.cpp -o test_stmt_summary
//
// Expected values mirror dataflow-go's stmtSummary/clipStatement, so both
// SDKs group identical statement shapes on the dashboard.

#include "dataflow.hpp"

#include <cstdio>
#include <string>

static int g_failures = 0;

static void check_eq(const std::string& got, const std::string& want, const char* what) {
    if (got == want) {
        std::printf("ok   %-64s -> %s\n", what, got.c_str());
        return;
    }
    ++g_failures;
    std::printf("FAIL %s\n  got:  \"%s\"\n  want: \"%s\"\n", what, got.c_str(), want.c_str());
}

int main() {
    // --- stmt_summary: verb + table -------------------------------------
    check_eq(dataflow::stmt_summary("SELECT id, name FROM public.orders WHERE id = $1"),
             "SELECT orders", "SELECT .. FROM schema-qualified");
    check_eq(dataflow::stmt_summary("INSERT INTO users (name) VALUES ($1)"),
             "INSERT users", "INSERT INTO");
    check_eq(dataflow::stmt_summary("UPDATE public.items SET stock = stock - 1 WHERE sku = $1"),
             "UPDATE items", "UPDATE (verb doubles as table keyword)");
    check_eq(dataflow::stmt_summary("DELETE FROM sessions WHERE token = $1"),
             "DELETE sessions", "DELETE FROM");
    check_eq(dataflow::stmt_summary("CREATE TABLE IF NOT EXISTS audit_log (id BIGINT)"),
             "CREATE audit_log", "CREATE TABLE IF NOT EXISTS");
    check_eq(dataflow::stmt_summary("DROP TABLE IF EXISTS temp_events"),
             "DROP temp_events", "DROP TABLE IF EXISTS");
    check_eq(dataflow::stmt_summary("TRUNCATE TABLE visits"),
             "TRUNCATE visits", "TRUNCATE TABLE");
    check_eq(dataflow::stmt_summary("UPDATE counter SET n = n + 1"),
             "UPDATE counter", "UPDATE (no schema)");
    check_eq(dataflow::stmt_summary("WITH recent AS (SELECT id FROM events)"),
             "WITH events", "CTE: table found in the body");

    // --- stmt_summary: quoting, casing, dollar-separated schemas ---------
    check_eq(dataflow::stmt_summary("select * from t1"),
             "SELECT t1", "lowercase keywords");
    check_eq(dataflow::stmt_summary("  SELECT   *\n\tFROM\t`orders`  "),
             "SELECT orders", "backtick-quoted table + whitespace collapse");
    check_eq(dataflow::stmt_summary("SELECT * FROM \"orders\""),
             "SELECT orders", "double-quoted table");
    check_eq(dataflow::stmt_summary("SELECT * FROM myschema$.events"),
             "SELECT events", "dollar-separated schema");

    // --- stmt_summary: bare verbs and non-SQL fallback --------------------
    check_eq(dataflow::stmt_summary("COMMIT"), "COMMIT", "bare verb");
    check_eq(dataflow::stmt_summary("SELECT 1"), "SELECT", "verb without table");
    check_eq(dataflow::stmt_summary("(SELECT 1) UNION (SELECT 2)"), "SELECT",
             "parenthesized statement");
    check_eq(dataflow::stmt_summary(""), "QUERY", "empty statement");
    check_eq(dataflow::stmt_summary("hello world, not sql at all"), "HELLO",
             "non-SQL: first word uppercased");
    check_eq(dataflow::stmt_summary("justonecommand"), "QUERY",
             "non-SQL: single word, no delimiter");
    check_eq(dataflow::stmt_summary("SELECT2 tokens"), "SELECT2",
             "verb boundary: SELECT2 is not SELECT");

    // --- clip_statement ----------------------------------------------------
    check_eq(dataflow::clip_statement("SELECT\n *\t  FROM   t"), "SELECT * FROM t",
             "clip: whitespace collapsed");
    check_eq(dataflow::clip_statement("  spaced   out  "), "spaced out",
             "clip: trimmed");
    const std::string long_sql(250, 'x');
    const std::string clipped = dataflow::clip_statement(long_sql);
    if (clipped.size() == 200) {
        std::printf("ok   %-64s -> %d chars\n", "clip: 250 chars clipped to 200",
                   static_cast<int>(clipped.size()));
    } else {
        ++g_failures;
        std::printf("FAIL clip: 250 chars clipped, got %d chars, want 200\n",
                    static_cast<int>(clipped.size()));
    }

    if (g_failures == 0) {
        std::printf("\nall tests passed\n");
        return 0;
    }
    std::printf("\n%d test(s) failed\n", g_failures);
    return 1;
}
