// cpp/tests/test_hardening.cpp -- hostile input must produce an error, never a crash.
//
// A network client can send any query it likes. Python answers absurdly deep
// input with a RecursionError; a C++ server would overflow its thread stack
// and die. So the parser spends one unit of a per-statement budget on every
// operator and nesting level (limit 400) and every recursive function checks
// the stack bytes used (stack_guard.h, budget 512 KB; test_stack_guard.cpp
// runs the deep shapes on a small stack), statements nest at most 32 deep and
// views over views at most 32 deep. The work runs on a std::thread, because
// that is where the server runs statements (a smaller stack than main's);
// Catch2 assertions stay on the test's own thread.
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "test_util.h"
#include <future>
#include <string>
#include <vector>

using namespace meradb;

namespace {

template <typename F>
auto onThread(F body) {
    return std::async(std::launch::async, body).get();
}

std::string parens(int depth, const std::string& core) {
    return std::string(static_cast<size_t>(depth), '(') + core + std::string(static_cast<size_t>(depth), ')');
}

std::string repeated(const std::string& unit, int times, const std::string& separator) {
    std::string out;
    for (int i = 0; i < times; ++i) out += (i ? separator : std::string()) + unit;
    return out;
}

// The parse error's message without its ", par ... mila (line, col)" tail; "" when it parses.
std::string parseErrorOf(const std::string& sql) {
    try {
        parseScript(sql);
    } catch (const ParseError& e) {
        return e.message().substr(0, e.message().find(", par "));
    }
    return "";
}

// Either bound may trip first, depending on the shape: the operator budget
// ("limit 400") or the stack budget ("stack limit 512 KB").
const std::string kTooDeepPrefix = "Query bahut gehri (nested) hai (";
bool tooDeep(const std::string& message) { return message.rfind(kTooDeepPrefix, 0) == 0; }

}  // namespace

TEST_CASE("hardening the parser accepts nesting up to the budget and refuses more", "[hardening]") {
    auto r = onThread([] {
        return std::vector<std::string>{parseErrorOf("DIKHAO * SE t JAHAN " + parens(100, "1 = 1")),
                                        parseErrorOf("DIKHAO * SE t JAHAN " + parens(401, "1 = 1")),
                                        parseErrorOf("DIKHAO * SE t JAHAN " + parens(100000, "1 = 1"))};
    });
    CHECK(r[0].empty());
    CHECK(tooDeep(r[1]));
    CHECK(tooDeep(r[2]));
}

TEST_CASE("hardening long operator chains are bounded too", "[hardening]") {
    auto r = onThread([] {
        return std::vector<std::string>{
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("1", 300, " + ")),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("1", 500, " + ")),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("1", 100000, " + ")),
            parseErrorOf("DIKHAO * SE t JAHAN " + repeated("1 = 1", 500, " AUR ")),
            parseErrorOf("DIKHAO * SE t JAHAN " + repeated("1 = 1", 500, " YA ")),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("2", 500, " * ")),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("-", 500, " ") + " 1"),
            parseErrorOf("DIKHAO * SE t JAHAN " + repeated("NAHI", 500, " ") + " x = 1"),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("AGAR 1 = 1 TAB 1 WARNA", 500, " ") + " 0 " +
                         repeated("KHATAM", 500, " ")),
            parseErrorOf("DIKHAO * SE t JAHAN x MEIN (" + repeated("1", 200, ", ") + ")"),
            parseErrorOf("DIKHAO * SE t JAHAN x MEIN (" + repeated("1", 5000, ", ") + ")")};
    });
    CHECK(r[0].empty());
    for (size_t i = 1; i <= 8; ++i) CHECK(tooDeep(r[i]));
    CHECK(r[9].empty());       // an IN list becomes an OR chain, one level per item ...
    CHECK(tooDeep(r[10]));  // ... so it is bounded like any other chain (Python fails near 450)
}

TEST_CASE("hardening set-operation chains and nested subqueries are bounded", "[hardening]") {
    auto r = onThread([] {
        std::string nested = "DIKHAO * SE t JAHAN x MEIN (DIKHAO x SE t)";
        for (int i = 0; i < 500; ++i) nested = "DIKHAO * SE t JAHAN x MEIN (" + nested + ")";
        return std::vector<std::string>{parseErrorOf("DIKHAO * SE t " + repeated("SANYUKT DIKHAO * SE t", 500, " ")),
                                        parseErrorOf("DIKHAO * SE t " + repeated("SANYUKT DIKHAO * SE t", 50, " ")),
                                        parseErrorOf(nested)};
    });
    CHECK(tooDeep(r[0]));
    CHECK(r[1].empty());
    CHECK(tooDeep(r[2]));
}

TEST_CASE("hardening each statement has its own budget", "[hardening]") {
    auto r = onThread([] {
        const std::string one = "DIKHAO * SE t JAHAN " + repeated("1 = 1", 200, " AUR ") + ";";
        return parseErrorOf(repeated(one, 100, "\n"));  // 100 statements x 199 operators
    });
    CHECK(r.empty());
}

TEST_CASE("hardening SAMJHAO cannot be stacked without limit", "[hardening]") {
    auto r = onThread([] {
        return std::vector<std::string>{parseErrorOf(repeated("SAMJHAO", 20, " ") + " DIKHAO * SE t"),
                                        parseErrorOf(repeated("SAMJHAO", 100, " ") + " DIKHAO * SE t")};
    });
    CHECK(r[0].empty());
    CHECK(r[1] == "Statements bahut gehre nested hain (limit 32)");
}

TEST_CASE("hardening a million open parentheses fail fast", "[hardening]") {
    auto r = onThread([] { return parseErrorOf("DIKHAO * SE t JAHAN " + std::string(1000000, '(')); });
    CHECK(tooDeep(r));
}

TEST_CASE("hardening deep-but-legal expressions evaluate without exhausting the stack", "[hardening]") {
    meradb_test::TempDir dir;
    auto r = onThread([&] {
        Engine e(dir.str());
        e.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1), (2)");
        std::vector<Result> out;
        out.push_back(e.runScript("DIKHAO * SE t JAHAN " + parens(80, "x = 1"))[0]);
        out.push_back(e.runScript("DIKHAO * SE t JAHAN x = " + repeated("1", 200, " * "))[0]);
        out.push_back(e.runScript("DIKHAO * SE t JAHAN " + repeated("x > 0", 200, " AUR "))[0]);
        out.push_back(e.runScript("SAMJHAO DIKHAO * SE t JAHAN " + parens(80, "x = 1"))[0]);
        out.push_back(e.runScript("DIKHAO * SE t JAHAN x MEIN (DIKHAO x SE t JAHAN " + parens(80, "x = 2") + ")")[0]);
        out.push_back(e.runScript("DIKHAO * SE t JAHAN " + parens(500, "x = 1"))[0]);
        return out;
    });
    for (size_t i = 0; i < 5; ++i) CHECK(r[i].error.empty());
    CHECK(r[0].rows.size() == 1);
    CHECK(r[1].rows.size() == 1);
    CHECK(r[2].rows.size() == 2);
    CHECK(r[4].rows.size() == 1);
    CHECK(r[5].error.rfind("[Parser Galti] " + kTooDeepPrefix, 0) == 0);
}

TEST_CASE("hardening views over views are capped", "[hardening]") {
    meradb_test::TempDir dir;
    auto r = onThread([&] {
        Engine e(dir.str());
        e.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (7); BANAO VIEW v1 KAHO DIKHAO * SE t");
        for (int i = 2; i <= 33; ++i)  // creating a view runs its query once to check it
            e.execute("BANAO VIEW v" + std::to_string(i) + " KAHO DIKHAO * SE v" + std::to_string(i - 1));
        std::vector<Result> out;
        out.push_back(e.runScript("DIKHAO * SE v32")[0]);                     // 32 levels of views: allowed
        out.push_back(e.runScript("DIKHAO * SE v33")[0]);                     // 33 levels: refused
        out.push_back(e.runScript("BANAO VIEW v34 KAHO DIKHAO * SE v33")[0]);  // so v34 cannot even be created
        out.push_back(e.runScript("DIKHAO * SE v1")[0]);                      // the depth counter unwound
        return out;
    });
    CHECK(r[0].error.empty());
    CHECK(r[0].rows.size() == 1);
    const std::string refusal = "[Execution Galti] View 'v1' bahut gehri nested hai (limit 32 views ek ke andar ek)";
    CHECK(r[1].error == refusal);
    CHECK(r[2].error == refusal);
    CHECK(r[3].error.empty());
}

namespace {
// Creates tables a and b with `n` shared INT columns through the engine API (no SQL text to parse).
void createWideTables(Engine& e, int n) {
    for (const char* name : {"a", "b"}) {
        ast::CreateTable ct;
        ct.name = name;
        for (int i = 0; i < n; ++i) {
            ast::ColumnDef c;
            c.name = "c" + std::to_string(i);
            c.typeName = "INT";
            ct.columns.push_back(std::move(c));
        }
        e.executeStatement(ct);
    }
}

std::string firstError(const std::vector<Result>& rs) { return rs[0].error.empty() ? "ok" : rs[0].error; }
}  // namespace

// The AND chain behind a natural join is as wide as the schema, which a client
// controls: it is built balanced, so a 20,000-column join works (Python itself
// stops with a RecursionError near 500 columns; we chose to work correctly).
// Creating the two tables is the slow part (~15 s at -O0), so they are made
// once and joined from the test's own thread and from a std::thread.
TEST_CASE("hardening natural join of very wide tables does not overflow the stack", "[hardening]") {
    meradb_test::TempDir dir;
    Engine e(dir.str());
    createWideTables(e, 20000);
    const std::string count = "DIKHAO GINO(*) SE a SAMAAN MILAO b";
    Result plain = e.runScript(count)[0];
    REQUIRE(plain.error.empty());
    CHECK(std::get<int64_t>(plain.rows[0][0].data) == 0);
    CHECK(onThread([&] { return firstError(e.runScript(count)); }) == "ok");
    CHECK(onThread([&] { return firstError(e.runScript("SAMJHAO " + count)); }) == "ok");
}

// Same data as the Python run that produced the expected count (1).
TEST_CASE("hardening natural join conjunction keeps its column order and NULL logic", "[hardening]") {
    meradb_test::TempDir dir;
    auto r = onThread([&] {
        Engine e(dir.str());
        createWideTables(e, 50);
        std::string vals1, vals2, vals3;
        for (int i = 0; i < 50; ++i) {
            vals1 += (i ? "," : "") + std::string("1");
            vals2 += (i ? "," : "") + std::string(i == 37 ? "KHALI" : "1");
            vals3 += (i ? "," : "") + std::string(i == 12 ? "2" : "1");
        }
        e.execute("DAALO MEIN a MAAN (" + vals1 + "), (" + vals2 + "), (" + vals3 + ")");
        e.execute("DAALO MEIN b MAAN (" + vals1 + "), (" + vals2 + ")");
        return e.runScript("DIKHAO GINO(*) SE a SAMAAN MILAO b")[0];
    });
    REQUIRE(r.error.empty());
    CHECK(std::get<int64_t>(r.rows[0][0].data) == 1);  // only the all-ones rows match; NULL and 2 never do
}
