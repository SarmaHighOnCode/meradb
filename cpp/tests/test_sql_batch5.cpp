// cpp/tests/test_sql_batch5.cpp -- BANAO / HATAO INDEX and SHOW INDEX, IF [NOT] EXISTS, SET TRANSACTION ISOLATION LEVEL,
// FOR UPDATE / LOCK IN SHARE MODE, SET DEFAULT ROLE.
//
// MeraDB-C++ only, so every expectation is written out here.
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

void ok(Engine& e, const std::string& sql) {
    Result r = run(e, sql);
    INFO(sql);
    INFO(r.error);
    REQUIRE(r.error.empty());
}

std::string q(Engine& e, const std::string& sql) {
    Result r = run(e, sql);
    INFO(sql);
    INFO(r.error);
    REQUIRE(r.error.empty());
    return flat(r);
}

std::string errorOf(Engine& e, const std::string& sql) {
    std::string error = run(e, sql).error;
    size_t close = error.find("Galti] ");
    return close == std::string::npos ? error : error.substr(close + 7);
}

}  // namespace

TEST_CASE("BANAO INDEX: plain, composite and unique indexes are listed by SHOW INDEX", "[sql5][index]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE a (id INT MUKHYA KUNJI, n INT, s TEXT, mail TEXT ANOKHA);");
    ok(e, "DAALO MEIN a MAAN (1, 10, 'x', 'p'), (2, 10, 'y', 'q');");
    ok(e, "BANAO INDEX ix_n PAR a (n);");
    ok(e, "BANAO INDEX ix_ns PAR a (n, s);");
    CHECK(q(e, "SHOW INDEX FROM a;") ==
          "a|0|PRIMARY|1|id;a|0|mail|1|mail;a|1|ix_n|1|n;a|1|ix_ns|1|n;a|1|ix_ns|2|s");
    CHECK_THAT(errorOf(e, "BANAO INDEX ix_n PAR a (s);"), ContainsSubstring("pehle se hai"));
    ok(e, "BANAO INDEX IF NOT EXISTS ix_n PAR a (s);");
    CHECK_THAT(errorOf(e, "BANAO INDEX bad PAR a (nope);"), ContainsSubstring("nope"));
    CHECK_THAT(errorOf(e, "BANAO INDEX bad PAR a (n, n);"), ContainsSubstring("do baar"));
    CHECK_THAT(errorOf(e, "BANAO INDEX bad PAR missing (n);"), ContainsSubstring("exist nahi"));

    SECTION("an index follows a column rename, and goes with the column it names") {
        ok(e, "SUDHARO TABLE a COLUMN s NAYA_NAAM s2;");
        CHECK(q(e, "SHOW INDEX FROM a;").find("ix_ns|2|s2") != std::string::npos);
        ok(e, "SUDHARO TABLE a HATAO COLUMN s2;");
        CHECK(q(e, "SHOW INDEX FROM a;") == "a|0|PRIMARY|1|id;a|0|mail|1|mail;a|1|ix_n|1|n");
    }
    SECTION("HATAO INDEX") {
        ok(e, "HATAO INDEX ix_n PAR a;");
        CHECK(q(e, "SHOW INDEX FROM a;").find("|ix_n|") == std::string::npos);
        CHECK_THAT(errorOf(e, "HATAO INDEX ix_n PAR a;"), ContainsSubstring("nahi hai"));
        ok(e, "HATAO INDEX IF EXISTS ix_n PAR a;");
    }
    SECTION("indexes survive a reopen") {
        Engine again(dir.str());
        CHECK(q(again, "SHOW INDEX FROM a;").find("ix_ns|2|s") != std::string::npos);
    }
}

TEST_CASE("BANAO ANOKHA INDEX enforces uniqueness until it is dropped", "[sql5][index]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE a (id INT MUKHYA KUNJI, n INT, s TEXT);");
    ok(e, "DAALO MEIN a MAAN (1, 10, 'x'), (2, 10, 'y');");
    CHECK_THAT(errorOf(e, "BANAO ANOKHA INDEX ux_n PAR a (n);"), ContainsSubstring("Duplicate"));
    CHECK(q(e, "SHOW INDEX FROM a;") == "a|0|PRIMARY|1|id");   // the failed attempt left nothing behind
    ok(e, "BANAO ANOKHA INDEX ux_ns PAR a (n, s);");
    CHECK_THAT(errorOf(e, "DAALO MEIN a MAAN (3, 10, 'x');"), ContainsSubstring("Duplicate"));
    ok(e, "DAALO MEIN a MAAN (3, 10, 'z');");
    ok(e, "HATAO INDEX ux_ns PAR a;");
    ok(e, "DAALO MEIN a MAAN (4, 10, 'x');");
}

TEST_CASE("index statements are superuser-only", "[sql5][index]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin(inst);
    ok(admin, "BANAO TABLE a (id INT MUKHYA KUNJI, n INT);");
    ok(admin, "BANAO USER u GUPT 'p';");
    ok(admin, "ADHIKAR DO DIKHAO PAR a KO u;");
    Engine u(inst);
    u.user = "u";
    CHECK_THAT(errorOf(u, "BANAO INDEX i PAR a (n);"), ContainsSubstring("superuser"));
    CHECK_THAT(errorOf(u, "SHOW INDEX FROM a;"), ContainsSubstring("superuser"));
}

TEST_CASE("IF [NOT] EXISTS on databases, tables, views, users and roles", "[sql5][ifexists]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO DATABASE IF NOT EXISTS d1;");
    ok(e, "BANAO DATABASE IF NOT EXISTS d1;");
    CHECK_THAT(errorOf(e, "BANAO DATABASE d1;"), ContainsSubstring("pehle se hai"));
    ok(e, "ISTEMAL d1;");
    ok(e, "BANAO TABLE IF NOT EXISTS t (id INT);");
    ok(e, "DAALO MEIN t MAAN (1);");
    ok(e, "BANAO TABLE IF NOT EXISTS t (id INT, other INT);");   // left alone: still one column, one row
    CHECK(q(e, "DIKHAO * SE t;") == "1");
    CHECK_THAT(errorOf(e, "BANAO TABLE t (id INT);"), ContainsSubstring("pehle se hai"));
    ok(e, "HATAO TABLE IF EXISTS nope;");
    CHECK_THAT(errorOf(e, "HATAO TABLE nope;"), ContainsSubstring("exist nahi"));
    ok(e, "HATAO VIEW IF EXISTS nope;");
    ok(e, "HATAO USER IF EXISTS nobody;");
    CHECK_THAT(errorOf(e, "HATAO USER nobody;"), ContainsSubstring("nobody"));
    ok(e, "BANAO USER IF NOT EXISTS u GUPT 'p';");
    ok(e, "BANAO USER IF NOT EXISTS u GUPT 'p';");
    ok(e, "BANAO ROLE IF NOT EXISTS r;");
    ok(e, "BANAO ROLE IF NOT EXISTS r;");
    CHECK_THAT(errorOf(e, "BANAO ROLE r;"), ContainsSubstring("pehle se hai"));
    ok(e, "HATAO ROLE IF EXISTS r;");
    ok(e, "HATAO ROLE IF EXISTS r;");
    ok(e, "ISTEMAL main;");
    ok(e, "HATAO DATABASE IF EXISTS d1;");
    ok(e, "HATAO DATABASE IF EXISTS d1;");
    CHECK_THAT(errorOf(e, "HATAO DATABASE d1;"), ContainsSubstring("exist nahi"));
}

TEST_CASE("SET TRANSACTION ISOLATION LEVEL and FOR UPDATE", "[sql5][txn]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE a (id INT MUKHYA KUNJI, n INT);");
    ok(e, "DAALO MEIN a MAAN (1, 10), (2, 20);");
    CHECK(q(e, "SHOW VARIABLES LIKE 'transaction_isolation';") == "transaction_isolation|REPEATABLE READ");
    for (const char* level : {"READ UNCOMMITTED", "READ COMMITTED", "REPEATABLE READ", "SERIALIZABLE"}) {
        ok(e, std::string("SET TRANSACTION ISOLATION LEVEL ") + level + ";");
        CHECK(q(e, "SHOW VARIABLES LIKE 'transaction_isolation';") == std::string("transaction_isolation|") + level);
    }
    ok(e, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;");
    ok(e, "SET GLOBAL TRANSACTION ISOLATION LEVEL SERIALIZABLE;");
    CHECK_THAT(errorOf(e, "SET TRANSACTION ISOLATION LEVEL SOMETIMES;"), ContainsSubstring("READ UNCOMMITTED"));

    ok(e, "SHURU;");
    CHECK(q(e, "DIKHAO * SE a JAHAN id = 1 FOR UPDATE;") == "1|10");
    CHECK(q(e, "DIKHAO * SE a KRAM id LOCK IN SHARE MODE;") == "1|10;2|20");
    CHECK(q(e, "DIKHAO * SE a JAHAN id = 2 FOR SHARE;") == "2|20");
    CHECK(q(e, "DIKHAO * SE a JAHAN id = 2 FOR UPDATE NOWAIT;") == "2|20");
    CHECK(q(e, "DIKHAO * SE a JAHAN id = 2 FOR UPDATE SKIP LOCKED;") == "2|20");
    ok(e, "BADLO a RAKHO n = 11 JAHAN id = 1;");
    ok(e, "PAKKA;");
    CHECK(q(e, "DIKHAO n SE a JAHAN id = 1;") == "11");
}

TEST_CASE("SET DEFAULT ROLE chooses the roles a session starts with", "[sql5][roles]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin(inst);
    ok(admin, "BANAO TABLE a (id INT);");
    ok(admin, "BANAO TABLE b (id INT);");
    ok(admin, "DAALO MEIN a MAAN (1);");
    ok(admin, "DAALO MEIN b MAAN (2);");
    ok(admin, "BANAO USER u GUPT 'p';");
    ok(admin, "BANAO ROLE ra; BANAO ROLE rb;");
    ok(admin, "ADHIKAR DO DIKHAO PAR a KO ra;");
    ok(admin, "ADHIKAR DO DIKHAO PAR b KO rb;");
    ok(admin, "ADHIKAR DO ROLE ra, rb KO u;");

    auto session = [&]() {
        auto s = std::make_unique<Engine>(inst);
        s->user = "u";
        return s;
    };
    {   // no default chosen: every granted role is active
        auto s = session();
        CHECK(q(*s, "DIKHAO * SE a;") == "1");
        CHECK(q(*s, "DIKHAO * SE b;") == "2");
    }
    ok(admin, "SET DEFAULT ROLE ra TO u;");
    {
        auto s = session();
        CHECK(q(*s, "DIKHAO * SE a;") == "1");
        CHECK_THAT(errorOf(*s, "DIKHAO * SE b;"), ContainsSubstring("adhikar"));
        ok(*s, "SET ROLE SAB;");   // the session can still switch
        CHECK(q(*s, "DIKHAO * SE b;") == "2");
    }
    ok(admin, "SET DEFAULT ROLE NONE TO u;");
    {
        auto s = session();
        CHECK_THAT(errorOf(*s, "DIKHAO * SE a;"), ContainsSubstring("adhikar"));
    }
    ok(admin, "SET DEFAULT ROLE ALL TO u;");
    {
        auto s = session();
        CHECK(q(*s, "DIKHAO * SE b;") == "2");
    }
    CHECK_THAT(errorOf(admin, "SET DEFAULT ROLE zz TO u;"), ContainsSubstring("diya hi nahi"));
    CHECK_THAT(errorOf(admin, "SET DEFAULT ROLE ra TO nobody;"), ContainsSubstring("nobody"));
}
