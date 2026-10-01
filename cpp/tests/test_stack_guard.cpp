// cpp/tests/test_stack_guard.cpp -- deeply nested input must give a clean error, never a stack overflow.
//
// Every deep shape is parsed AND executed on a thread with a deliberately
// small stack, built at the default (-O0) flags. The pairs below are
// (thread stack, stack budget): the real budget on the smallest stack we
// support (1 MB, the MSVC default), and a quarter-size pair that makes the
// guard trip on far shallower input. A crash would take the whole test
// process down, so "the test ran to the end" is itself the assertion.
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/pyjson.h"
#include "meradb/stack_guard.h"
#include "small_stack.h"
#include "test_util.h"
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace meradb;

namespace {

std::string repeated(const std::string& unit, int times, const std::string& separator = " ") {
    std::string out;
    for (int i = 0; i < times; ++i) out += (i ? separator : std::string()) + unit;
    return out;
}

std::string parens(int depth, const std::string& core) {
    return std::string(static_cast<size_t>(depth), '(') + core + std::string(static_cast<size_t>(depth), ')');
}

std::string scalarSubqueries(int depth) {
    std::string s = "1";
    for (int i = 0; i < depth; ++i) s = "(DIKHAO " + s + " SE t SIRF 1)";
    return s;
}

struct Shape {
    const char* name;
    std::function<std::string(int)> make;
};

const std::vector<Shape>& shapes() {
    static const std::vector<Shape> all = {
        {"nested parentheses", [](int n) { return "DIKHAO * SE t JAHAN " + parens(n, "x = 1"); }},
        {"scalar subqueries in the select list", [](int n) { return "DIKHAO " + scalarSubqueries(n) + " SE t"; }},
        {"scalar subqueries in WHERE", [](int n) { return "DIKHAO * SE t JAHAN x = " + scalarSubqueries(n); }},
        {"IN subqueries",
         [](int n) {
             std::string s = "DIKHAO x SE t";
             for (int i = 0; i < n; ++i) s = "DIKHAO x SE t JAHAN x MEIN (" + s + ")";
             return s;
         }},
        {"scalar subqueries inside an IN subquery",
         [](int n) { return "DIKHAO * SE t JAHAN x MEIN (DIKHAO " + scalarSubqueries(n) + " SE t)"; }},
        {"scalar subqueries over a column",
         [](int n) {
             std::string s = "x";
             for (int i = 0; i < n; ++i) s = "(DIKHAO " + s + " SE t SIRF 1)";
             return "DIKHAO " + s + " SE t";
         }},
        {"CASE nested in ELSE",
         [](int n) { return "DIKHAO * SE t JAHAN x = " + repeated("AGAR 1 = 1 TAB 1 WARNA", n) + " 0 " + repeated("KHATAM", n); }},
        {"CASE nested in THEN",
         [](int n) { return "DIKHAO * SE t JAHAN x = " + repeated("AGAR 1 = 1 TAB", n) + " 1 " + repeated("WARNA 0 KHATAM", n); }},
        {"NAHI chain", [](int n) { return "DIKHAO * SE t JAHAN " + repeated("NAHI", n) + " x = 1"; }},
        {"unary minus chain", [](int n) { return "DIKHAO * SE t JAHAN x = " + repeated("-", n) + " 1"; }},
        {"AUR chain", [](int n) { return "DIKHAO * SE t JAHAN " + repeated("x > 0", n, " AUR "); }},
        {"YA chain", [](int n) { return "DIKHAO * SE t JAHAN " + repeated("x > 0", n, " YA "); }},
        {"+ chain", [](int n) { return "DIKHAO * SE t JAHAN x = " + repeated("1", n, " + "); }},
        {"* chain", [](int n) { return "DIKHAO * SE t JAHAN x = " + repeated("1", n, " * "); }},
        {"IN list", [](int n) { return "DIKHAO * SE t JAHAN x MEIN (" + repeated("1", n, ", ") + ")"; }},
        {"nested PEHLA",
         [](int n) {
             std::string s = "1";
             for (int i = 0; i < n; ++i) s = "PEHLA(" + s + ")";
             return "DIKHAO " + s + " SE t";
         }},
        {"set operation chain", [](int n) { return "DIKHAO * SE t " + repeated("SANYUKT DIKHAO * SE t", n); }},
        {"SAMJHAO over parentheses", [](int n) { return "SAMJHAO DIKHAO * SE t JAHAN " + parens(n, "x = 1"); }},
        {"SAMJHAO over a set operation chain",
         [](int n) { return "SAMJHAO DIKHAO * SE t " + repeated("SANYUKT DIKHAO * SE t", n); }},
        {"INSERT ... SELECT with nested subqueries",
         [](int n) { return "DAALO MEIN t DIKHAO " + scalarSubqueries(n) + " SE t"; }},
        {"UPDATE with nested subqueries", [](int n) { return "BADLO t RAKHO x = " + scalarSubqueries(n); }},
    };
    return all;
}

bool cleanOutcome(const std::string& error) {
    return error.empty() || error.find("bahut gehri") != std::string::npos;
}

struct BudgetGuard {  // restores the process-wide budget even when an assertion fails
    std::size_t saved = stackBudget();
    ~BudgetGuard() { setStackBudget(saved); }
};

// Runs every shape at every depth on a `stackBytes` thread under `budget`; returns the first
// complaint ("" when all fine).
std::string sweep(std::size_t stackBytes, std::size_t budget, const std::vector<int>& depths, bool alsoParseOnly) {
    BudgetGuard restore;
    setStackBudget(budget);
    std::string complaint;
    bool started = meradb_test::runOnStack(stackBytes, [&] {
        try {
            meradb_test::TempDir dir;
            Engine e(dir.str());
            e.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1), (2)");
            for (const auto& shape : shapes()) {
                for (int depth : depths) {
                    std::string sql = shape.make(depth);
                    auto where = [&] { return std::string(shape.name) + " depth " + std::to_string(depth) + ": "; };
                    if (alsoParseOnly) {
                        try {
                            parseScript(sql);
                        } catch (const MeraDBError& err) {
                            if (!cleanOutcome(err.message()) && complaint.empty())
                                complaint = where() + "parse: " + err.message().substr(0, 120);
                        }
                    }
                    auto results = e.runScript(sql);
                    if (!cleanOutcome(results[0].error) && complaint.empty())
                        complaint = where() + "run: " + results[0].error.substr(0, 120);
                    // the engine (and the stack guard's base) is fine afterwards
                    Result after = e.runScript("DIKHAO COUNT(*) SE t")[0];
                    if (!after.error.empty() && complaint.empty()) complaint = where() + "engine broken afterwards";
                }
            }
        } catch (const std::exception& ex) {
            complaint = std::string("stray exception: ") + ex.what();
        }
    });
    if (!started) return "could not start the small-stack thread";
    return complaint;
}

}  // namespace

TEST_CASE("stack_guard every deep shape is a clean error on a 1 MB stack", "[stack_guard]") {
    // 399 = the operator budget's last legal level, 5000 and 100000 = far beyond it.
    std::string complaint = sweep(1024 * 1024, kDefaultStackBudget, {399, 5000}, true);
    CHECK(complaint == "");
}

TEST_CASE("stack_guard the same shapes on a quarter-size stack and budget", "[stack_guard]") {
    std::string complaint = sweep(256 * 1024, 128 * 1024, {20, 150, 399}, true);
    CHECK(complaint == "");
}

TEST_CASE("stack_guard 390 nested scalar subqueries on a default thread give an error, not a crash", "[stack_guard]") {
    std::string error;
    std::thread worker([&] {
        meradb_test::TempDir dir;
        Engine e(dir.str());
        e.execute("BANAO TABLE t (x INT)");
        error = e.runScript("DIKHAO " + scalarSubqueries(390) + " SE t")[0].error;
    });
    worker.join();
    CHECK(error.find("Query bahut gehri (nested) hai (") != std::string::npos);
}

TEST_CASE("stack_guard the error names the stack limit and the guard resets between statements", "[stack_guard]") {
    meradb_test::TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1)");
    std::string deep = "DIKHAO * SE t JAHAN " + parens(300, "x = 1");  // ~1 MB of -O0 frames: over the budget
    Result r = e.runScript(deep)[0];
    CHECK(r.error.rfind("[Parser Galti] Query bahut gehri (nested) hai (stack limit 512 KB), par ", 0) == 0);
    CHECK(e.runScript("DIKHAO * SE t JAHAN " + parens(5, "x = 1"))[0].rows.size() == 1);
    CHECK(stackUsedBytes() == 0);  // no StackBase outlives its entry point
}

TEST_CASE("stack_guard nested JSON frames are refused before they can exhaust the stack", "[stack_guard]") {
    // Within the nesting cap the decode must work on a 1 MB stack ...
    std::string okDepth = std::string(400, '[') + std::string(400, ']');
    std::string tooDeep = std::string(100000, '[') + std::string(100000, ']');
    bool parsedOk = false;
    bool refused = false;
    REQUIRE(meradb_test::runOnStack(1024 * 1024, [&] {
        try {
            parsedOk = pyjson::parse(okDepth).is_array();
        } catch (const std::exception&) {
        }
        try {
            pyjson::parse(tooDeep);
        } catch (const pyjson::ParseFailure&) {
            refused = true;
        }
    }));
    CHECK(parsedOk);
    CHECK(refused);

    // ... and the byte guard itself stops the recursive pass: a tiny budget refuses even 400 levels.
    BudgetGuard restore;
    setStackBudget(8 * 1024);
    bool refusedByBytes = false;
    REQUIRE(meradb_test::runOnStack(256 * 1024, [&] {
        try {
            pyjson::parse(okDepth);
        } catch (const pyjson::ParseFailure&) {
            refusedByBytes = true;
        }
    }));
    CHECK(refusedByBytes);
}
