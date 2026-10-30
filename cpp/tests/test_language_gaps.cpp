// cpp/tests/test_language_gaps.cpp -- bugs and small language gaps found reviewing the procedural code, type conversion,
// dates and the expression grammar. Each case was a wrong answer, a silent no-op or a parse error before it was fixed.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "golden_runner.h"
#include "test_util.h"

using namespace meradb;
using Catch::Matchers::ContainsSubstring;
using meradb_test::TempDir;

namespace {

Result run(Engine& e, const std::string& sql) { return meradb_test::runLast(e, sql); }

std::string flat(const Result& r) {
    std::string out;
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) out += ";";
        for (size_t j = 0; j < r.rows[i].size(); ++j) out += (j ? "|" : "") + formatValue(r.rows[i][j]);
    }
    return out;
}

std::string q(Engine& e, const std::string& sql) { return flat(run(e, sql)); }

}  // namespace

TEST_CASE("ROUND to tens does not wrap an INT around", "[gaps][scalar]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE_THROWS_WITH(e.execute("DIKHAO ROUND(9223372036854775807, -1);"), ContainsSubstring("ROUND"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO ROUND(-9223372036854775807, -1);"), ContainsSubstring("ROUND"));
    REQUIRE(q(e, "DIKHAO ROUND(9223372036854775804, -1), ROUND(15, -1), ROUND(-15, -1), ROUND(14, -1);") ==
            "9223372036854775800|20|-20|10");
}

TEST_CASE("Date functions read the whole text, not its first ten characters", "[gaps][date]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE_THROWS_WITH(e.execute("DIKHAO YEAR('2024-01-01 garbage');"), ContainsSubstring("valid DATETIME"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO CAST('2024-01-01 garbage' AS DATE);"), ContainsSubstring("valid DATETIME"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO DATE_FORMAT('2024-01-01 99:99:99', '%H');"), ContainsSubstring("valid DATETIME"));
    REQUIRE(q(e, "DIKHAO DATE_FORMAT('2024-01-01 10:20', '%H:%i');") == "10:20");
    REQUIRE(q(e, "DIKHAO YEAR('2024-01-01T10:20:30'), DATEDIFF('2024-01-15 23:00:00', '2024-01-14 01:00:00');") == "2024|1");
}

TEST_CASE("DATE_FORMAT knows the MySQL time specifiers", "[gaps][date]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE(q(e, "DIKHAO DATE_FORMAT('2024-03-05 13:02:09', '%S %T %r %h %I %l %k %c %p %q');") ==
            "09 13:02:09 01:02:09 PM 01 01 1 13 3 PM q");
    REQUIRE(q(e, "DIKHAO DATE_FORMAT('2024-03-05 00:30:00', '%h %l %p');") == "12 12 AM");
}

TEST_CASE("CAST and stored functions take decimal text only", "[gaps][cast]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE_THROWS_WITH(e.execute("DIKHAO CAST('0x10' AS FLOAT);"), ContainsSubstring("cast"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO CAST('0x10' AS INT);"), ContainsSubstring("cast"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO CAST('inf' AS FLOAT);"), ContainsSubstring("cast"));
    REQUIRE(q(e, "DIKHAO CAST('1e3' AS FLOAT), CAST('-2.5' AS FLOAT);") == "1000.0|-2.5");
    run(e, "BANAO FUNCTION same(a INT) RETURNS INT SHURU RETURN a; KHATAM;");
    run(e, "BANAO FUNCTION samef(a FLOAT) RETURNS FLOAT SHURU RETURN a; KHATAM;");
    // strtoll used to clamp this to the largest INT
    REQUIRE_THROWS(e.execute("DIKHAO same('99999999999999999999');"));
    REQUIRE_THROWS(e.execute("DIKHAO samef('0x10');"));
    REQUIRE(q(e, "DIKHAO same('12'), samef('1.5'), samef(2);") == "12|1.5|2.0");
}
