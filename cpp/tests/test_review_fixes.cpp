// cpp/tests/test_review_fixes.cpp -- one test per flaw found in the whole-codebase review: each one failed before its fix.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include "golden_runner.h"
#include "meradb/engine.h"
#include "test_util.h"

using namespace meradb;
using Catch::Matchers::ContainsSubstring;
using meradb_test::TempDir;

namespace {

Result run(Engine& e, const std::string& sql) { return meradb_test::runLast(e, sql); }

// "a|b|c" per row, rows joined by ";"
std::string q(Engine& e, const std::string& sql) {
    Result r = run(e, sql);
    INFO(sql);
    REQUIRE(r.error.empty());
    std::string out;
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) out += ";";
        for (size_t j = 0; j < r.rows[i].size(); ++j) out += (j ? "|" : "") + formatValue(r.rows[i][j]);
    }
    return out;
}

std::string errorOf(Engine& e, const std::string& sql) { return run(e, sql).error; }

void ok(Engine& e, const std::string& sql) {
    Result r = run(e, sql);
    INFO(sql);
    INFO(r.error);
    REQUIRE(r.error.empty());
}

}  // namespace

TEST_CASE("SHOW VARIABLES LIKE with many % answers at once", "[review][variables]") {
    TempDir dir;
    Engine e(dir.str());
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(run(e, "SHOW VARIABLES LIKE '%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%x';").rows.empty());
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));  // it used to take minutes
    // and the matching itself is unchanged
    REQUIRE(run(e, "SHOW VARIABLES LIKE 'auto%';").rows.size() == 1);
    REQUIRE(run(e, "SHOW VARIABLES LIKE '%_ISOLATION';").rows.size() == 1);
    REQUIRE(run(e, "SHOW VARIABLES LIKE '%o%o%';").rows.size() == 2);
    REQUIRE(run(e, "SHOW VARIABLES LIKE 'autocommi_';").rows.size() == 1);
    REQUIRE(run(e, "SHOW VARIABLES LIKE 'autocommit_';").rows.empty());
    REQUIRE(run(e, "SHOW VARIABLES LIKE '';").rows.empty());
    REQUIRE(run(e, "SHOW VARIABLES LIKE '%';").rows.size() == 2);
}

TEST_CASE("TAKRAAV PAR BADLO respects the child rows of the row it changes", "[review][upsert][fk]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE p (id INT MUKHYA KUNJI, v TEXT);");
    ok(e, "BANAO TABLE c (id INT MUKHYA KUNJI, pid INT SANDARBH p(id));");
    ok(e, "DAALO MEIN p MAAN (1, 'a'), (2, 'b');");
    ok(e, "DAALO MEIN c MAAN (10, 1);");
    // RESTRICT (the default): the key a child row points at cannot be moved away
    REQUIRE_FALSE(errorOf(e, "DAALO MEIN p MAAN (1, 'x') TAKRAAV PAR BADLO id = id + 100;").empty());
    REQUIRE(q(e, "DIKHAO id, v SE p KRAM id;") == "1|a;2|b");
    REQUIRE(q(e, "DIKHAO pid SE c;") == "1");
    // a row nobody points at, and a change that leaves the key alone, still go through
    ok(e, "DAALO MEIN p MAAN (2, 'x') TAKRAAV PAR BADLO id = id + 100;");
    ok(e, "DAALO MEIN p MAAN (1, 'z') TAKRAAV PAR BADLO v = v;");
    REQUIRE(q(e, "DIKHAO id, v SE p KRAM id;") == "1|z;102|b");

    // ON UPDATE CASCADE: the child follows
    ok(e, "BANAO TABLE k (id INT MUKHYA KUNJI, e TEXT ANOKHA);");
    ok(e, "BANAO TABLE kc (id INT MUKHYA KUNJI, ke TEXT SANDARBH k(e) ON UPDATE CASCADE);");
    ok(e, "DAALO MEIN k MAAN (1, 'a');");
    ok(e, "DAALO MEIN kc MAAN (1, 'a');");
    ok(e, "DAALO MEIN k MAAN (1, 'ignored') TAKRAAV PAR BADLO e = 'changed';");
    REQUIRE(q(e, "DIKHAO e SE k;") == "changed");
    REQUIRE(q(e, "DIKHAO ke SE kc;") == "changed");
}

TEST_CASE("ADD COLUMN keeps the table's named indexes", "[review][alter][index]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (a INT, b INT, c INT);");
    ok(e, "BANAO ANOKHA INDEX ux PAR t (a, b);");
    ok(e, "BANAO INDEX px PAR t (c);");
    ok(e, "SUDHARO TABLE t JODO COLUMN d INT;");
    const std::string shown = q(e, "SHOW INDEX FROM t;");
    REQUIRE_THAT(shown, ContainsSubstring("|ux|"));
    REQUIRE_THAT(shown, ContainsSubstring("|px|"));
    ok(e, "DAALO MEIN t (a, b) MAAN (1, 1);");
    REQUIRE_FALSE(errorOf(e, "DAALO MEIN t (a, b) MAAN (1, 1);").empty());
    ok(e, "HATAO INDEX ux PAR t;");  // the unique rule can still be lifted by its name
    ok(e, "DAALO MEIN t (a, b) MAAN (1, 1);");
}

TEST_CASE("ADD COLUMN refuses a second MUKHYA KUNJI beside a composite one", "[review][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (a INT, b INT, MUKHYA KUNJI (a, b));");
    REQUIRE(errorOf(e, "SUDHARO TABLE t JODO COLUMN k INT MUKHYA KUNJI;") ==
            "[Execution Galti] Table mein pehle se MUKHYA KUNJI hai");
    ok(e, "SUDHARO TABLE t JODO COLUMN k INT;");
}

TEST_CASE("DROP COLUMN keeps the constraints that do not name the column", "[review][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (a INT, b INT, c INT, d INT, MUKHYA KUNJI (a, b), ANOKHA (c, d));");
    ok(e, "BANAO INDEX ia PAR t (a);");
    ok(e, "BANAO ANOKHA INDEX ub PAR t (b, c);");
    ok(e, "DAALO MEIN t MAAN (1, 1, 1, 1);");
    ok(e, "SUDHARO TABLE t HATAO COLUMN d;");  // takes ANOKHA (c, d) with it, nothing else
    REQUIRE_FALSE(errorOf(e, "DAALO MEIN t MAAN (1, 1, 2);").empty());  // MUKHYA KUNJI (a, b) still holds
    REQUIRE_FALSE(errorOf(e, "DAALO MEIN t MAAN (2, 1, 1);").empty());  // so does the unique index (b, c)
    ok(e, "DAALO MEIN t MAAN (2, 2, 1);");                              // c alone was never unique
    const std::string shown = q(e, "SHOW INDEX FROM t;");
    REQUIRE_THAT(shown, ContainsSubstring("|ia|"));
    REQUIRE_THAT(shown, ContainsSubstring("|ub|"));
    // a constraint that names the dropped column goes with it
    ok(e, "SUDHARO TABLE t HATAO COLUMN c;");
    REQUIRE_THAT(q(e, "SHOW INDEX FROM t;"), !ContainsSubstring("|ub|"));
    ok(e, "SUDHARO TABLE t HATAO COLUMN b;");
    ok(e, "DAALO MEIN t MAAN (1);");  // (a, b) is gone with b
    REQUIRE(q(e, "DIKHAO COUNT(*) SE t JAHAN a = 1;") == "2");
}

TEST_CASE("USING and NATURAL joins need DIKHAO on the columns they compare", "[review][users]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin{inst};
    Engine ravi{inst};
    ravi.user = "ravi";
    admin.execute("BANAO USER ravi GUPT 'pw';"
                  "BANAO TABLE staff (id INT MUKHYA KUNJI, salary INT);"
                  "BANAO TABLE probe (salary INT);"
                  "BANAO TABLE ids (id INT);"
                  "DAALO MEIN staff MAAN (1, 500), (2, 900);"
                  "DAALO MEIN probe MAAN (900);"
                  "DAALO MEIN ids MAAN (2);"
                  "ADHIKAR DO DIKHAO (id) PAR staff KO ravi;"
                  "ADHIKAR DO DIKHAO PAR probe KO ravi;"
                  "ADHIKAR DO DIKHAO PAR ids KO ravi;");
    const std::string denied = "[Execution Galti] 'ravi' ko table 'staff' ke column 'salary' par DIKHAO ka adhikar nahi hai";
    REQUIRE(errorOf(ravi, "DIKHAO staff.id SE probe MILAO staff USING (salary);") == denied);
    REQUIRE(errorOf(ravi, "DIKHAO staff.id SE staff MILAO probe USING (salary);") == denied);
    REQUIRE(errorOf(ravi, "DIKHAO staff.id SE probe SAMAAN MILAO staff;") == denied);
    REQUIRE(errorOf(ravi, "DIKHAO staff.id SE staff SAMAAN MILAO probe;") == denied);
    // the columns ravi may read still join
    REQUIRE(q(ravi, "DIKHAO staff.id SE ids MILAO staff USING (id);") == "2");
    REQUIRE(q(ravi, "DIKHAO staff.id SE ids SAMAAN MILAO staff;") == "2");
    REQUIRE(q(admin, "DIKHAO staff.id SE probe MILAO staff USING (salary);") == "2");
}

TEST_CASE("POWER with a negative base leaves INT for FLOAT when it no longer fits", "[review][functions]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE(q(e, "DIKHAO POWER(-3, 3), POWER(-3, 4), POWER(-2, 62), POWER(2, 62);") ==
            "-27|81|4611686018427387904|4611686018427387904");
    REQUIRE(q(e, "DIKHAO POWER(-2, 63);") == "-9223372036854775808");  // the smallest INT, exactly
    Result big = run(e, "DIKHAO POWER(-3, 40), POWER(-3, 41), POWER(2, 63), POWER(-2, 64);");
    REQUIRE(big.error.empty());
    for (const auto& v : big.rows.at(0)) REQUIRE(std::holds_alternative<double>(v.data));
    REQUIRE(std::get<double>(big.rows[0][0].data) > 1.2e19);
    REQUIRE(std::get<double>(big.rows[0][1].data) < -3.6e19);
}

TEST_CASE("a table cannot be renamed onto a view's name", "[review][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (a INT);");
    ok(e, "BANAO VIEW v KAHO DIKHAO a SE t;");
    REQUIRE(errorOf(e, "SUDHARO TABLE t NAYA_NAAM v;") == "[Execution Galti] 'v' naam ka VIEW pehle se hai");
    ok(e, "SUDHARO TABLE t NAYA_NAAM u;");
}
