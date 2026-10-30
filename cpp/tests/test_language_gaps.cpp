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
