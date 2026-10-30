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

TEST_CASE("A DATE compares with text that has a time", "[gaps][date]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE d (id INT, day DATE, at DATETIME);");
    run(e, "DAALO MEIN d MAAN (1, '2024-02-29', '2024-02-29 10:00:00'), (2, '2024-03-01', '2024-02-29 23:00:00');");
    REQUIRE(q(e, "DIKHAO id SE d JAHAN day < '2024-02-29 12:00:00' KRAM id;") == "1");
    REQUIRE(q(e, "DIKHAO id SE d JAHAN at > day KRAM id;") == "1");
    REQUIRE(q(e, "DIKHAO GINO(*) SE d JAHAN day < NOW();") == "2");
    REQUIRE(q(e, "DIKHAO id SE d JAHAN day = '2024-03-01 00:00:00';") == "2");
    REQUIRE_THROWS_WITH(e.execute("DIKHAO id SE d JAHAN day < '2024-02-29 xx';"), ContainsSubstring("valid DATETIME"));
}

TEST_CASE("MEIN over a subquery matches a DATE with its text", "[gaps][date]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE d (id INT, day DATE, s TEXT);");
    run(e, "DAALO MEIN d MAAN (1, '2024-02-29', '2024-02-29'), (2, '2024-03-01', 'abc');");
    REQUIRE(q(e, "DIKHAO id SE d JAHAN day MEIN ('2024-02-29');") == "1");
    REQUIRE(q(e, "DIKHAO id SE d JAHAN day MEIN (DIKHAO '2024-02-29');") == "1");
    REQUIRE(q(e, "DIKHAO id SE d JAHAN s MEIN (DIKHAO day SE d) KRAM id;") == "1");
    REQUIRE(q(e, "DIKHAO id SE d JAHAN day NAHI MEIN (DIKHAO '2024-02-29');") == "2");
}

TEST_CASE("SHOW CREATE TABLE keeps every digit of a FLOAT default", "[gaps][show]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE f (a FLOAT WARNA 0.12345678901234, b FLOAT WARNA -2.5);");
    const std::string text = formatValue(run(e, "SHOW CREATE TABLE f;").rows[0][1]);
    REQUIRE_THAT(text, ContainsSubstring("WARNA 0.12345678901234"));
    REQUIRE_THAT(text, ContainsSubstring("WARNA -2.5"));
}

TEST_CASE("Numbers can be written with an exponent", "[gaps][literal]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE(q(e, "DIKHAO 1e5, 2.5E-1, 1E+2, -1e2;") == "100000.0|0.25|100.0|-100.0");
    REQUIRE(run(e, "DIKHAO 1e5;").columns[0] == "100000.0");
    run(e, "BANAO TABLE t (e5 INT, x FLOAT);");
    run(e, "DAALO MEIN t MAAN (7, 1e2);");
    REQUIRE(q(e, "DIKHAO e5, x SE t;") == "7|100.0");
    REQUIRE(q(e, "DIKHAO 2 * x, 1e2 = x SE t;") == "200.0|SACH");
    // `1e` is not an exponent: the number 1 and then a name, as before
    REQUIRE_THROWS_WITH(e.execute("DIKHAO 1e;"), ContainsSubstring("SE"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO 1e999;"), ContainsSubstring("FLOAT"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO id SE t SIRF 1e2;"), ContainsSubstring("whole number"));
}

TEST_CASE("The smallest INT can be written as a literal", "[gaps][literal]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE(q(e, "DIKHAO -9223372036854775808, - 9223372036854775808;") == "-9223372036854775808|-9223372036854775808");
    REQUIRE(q(e, "DIKHAO -9223372036854775808 + 1;") == "-9223372036854775807");
    REQUIRE_THROWS_WITH(e.execute("DIKHAO 9223372036854775808;"), ContainsSubstring("8-byte"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO 1 - 9223372036854775808;"), ContainsSubstring("8-byte"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO -(-9223372036854775808);"), ContainsSubstring("8-byte"));
    REQUIRE_THROWS_WITH(e.execute("DIKHAO - -9223372036854775808;"), ContainsSubstring("8-byte"));
    run(e, "BANAO TABLE t (a INT WARNA -9223372036854775808, b INT);");
    run(e, "DAALO MEIN t (b) MAAN (-9223372036854775808);");
    REQUIRE(q(e, "DIKHAO a = b SE t;") == "SACH");
    // a prepared statement sends this value back through the parser
    run(e, "PREPARE p FROM 'DIKHAO ?';");
    REQUIRE(q(e, "EXECUTE p USING -9223372036854775808;") == "-9223372036854775808");
}
