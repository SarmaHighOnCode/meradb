// cpp/tests/test_sql_batch3.cpp -- views (OR REPLACE, ALTER VIEW, WITH CHECK OPTION, DAALO/BADLO/MITAO through a simple
// view), roles and finer grants (SET ROLE, WITH GRANT OPTION, column-level grants), and the SHOW family.
//
// MeraDB-C++ only (the Python engine does not have them), so every expectation is written out here.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "golden_runner.h"
#include "meradb/catalog.h"
#include "meradb/client.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/highlight.h"
#include "meradb/parser.h"
#include "server_fixture.h"
#include "test_util.h"
#include <fstream>
#include <iterator>

using namespace meradb;
using Catch::Matchers::ContainsSubstring;
using meradb_test::RunningServer;
using meradb_test::TempDir;

namespace {

Result run(Engine& e, const std::string& sql) { return meradb_test::runLast(e, sql); }

// "a|b|c" per row, rows joined by ";" -- so one REQUIRE shows a whole result.
std::string flat(const Result& r) {
    std::string out;
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) out += ";";
        for (size_t j = 0; j < r.rows[i].size(); ++j) out += (j ? "|" : "") + formatValue(r.rows[i][j]);
    }
    return out;
}

std::string q(Engine& e, const std::string& sql) {
    Result r = run(e, sql);
    INFO(sql);
    REQUIRE(r.error.empty());
    return flat(r);
}

// The error message of a failing statement, without the "[... Galti] " stage prefix ("" if it ran fine).
std::string errorOf(Engine& e, const std::string& sql) {
    std::string error = run(e, sql).error;
    size_t close = error.find("Galti] ");
    return close == std::string::npos ? error : error.substr(close + 7);
}

void ok(Engine& e, const std::string& sql) {
    Result r = run(e, sql);
    INFO(sql);
    INFO(r.error);
    REQUIRE(r.error.empty());
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const char* const kNotUpdatable = "'v' ek VIEW hai, table nahi -- isme DAALO/BADLO/MITAO nahi kar sakte";

// one instance, a superuser session and a restricted session for ravi
struct Users {
    TempDir dir;
    std::shared_ptr<Instance> inst = std::make_shared<Instance>(dir.str());
    Engine admin{inst};
    Engine ravi{inst};
    Users() {
        ravi.user = "ravi";
        ok(admin, "BANAO USER ravi GUPT 'pw'; BANAO USER asha GUPT 'pw2';");
        ok(admin, "BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, email TEXT, umar INT);");
        ok(admin, "DAALO MEIN students MAAN (1, 'Ravi', 'r@x.in', 20), (2, 'Priya', 'p@x.in', 19);");
        ok(admin, "BANAO TABLE other (id INT MUKHYA KUNJI, note TEXT);");
        ok(admin, "DAALO MEIN other MAAN (1, 'secret');");
    }
};

}  // namespace

// ------------------------------------------------------------ views: OR REPLACE / ALTER / CHECK OPTION ----

TEST_CASE("BANAO OR REPLACE VIEW, BANAO BADAL VIEW and SUDHARO VIEW", "[sql3][view]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, naam TEXT, dept TEXT);");
    ok(e, "DAALO MEIN t MAAN (1, 'a', 'cs'), (2, 'b', 'ee');");
    CHECK(run(e, "BANAO VIEW v KAHO DIKHAO id SE t;").message == "View 'v' ban gaya");
    CHECK_THAT(errorOf(e, "BANAO VIEW v KAHO DIKHAO naam SE t;"), ContainsSubstring("View 'v' pehle se hai"));
    CHECK(run(e, "BANAO OR REPLACE VIEW v KAHO DIKHAO naam SE t;").message == "View 'v' badal diya");
    CHECK(q(e, "DIKHAO * SE v KRAM naam;") == "a;b");
    CHECK(run(e, "banao or replace view v2 kaho dikhao id se t;").message == "View 'v2' ban gaya");  // creates when missing
    ok(e, "BANAO BADAL VIEW v KAHO DIKHAO id, naam SE t;");
    CHECK(q(e, "DIKHAO * SE v KRAM id;") == "1|a;2|b");
    CHECK(run(e, "SUDHARO VIEW v KAHO DIKHAO dept SE t JAHAN id = 2;").message == "View 'v' badal diya");
    CHECK(q(e, "DIKHAO * SE v;") == "ee");
    CHECK_THAT(errorOf(e, "SUDHARO VIEW nope KAHO DIKHAO id SE t;"), ContainsSubstring("View 'nope' exist nahi karta"));
    CHECK_THAT(errorOf(e, "BANAO OR REPLACE VIEW t KAHO DIKHAO id SE t;"), ContainsSubstring("Table 't' pehle se hai"));
    // a replacement that does not run leaves the old view alone
    CHECK_THAT(errorOf(e, "BANAO OR REPLACE VIEW v KAHO DIKHAO nope SE t;"), ContainsSubstring("nope"));
    CHECK(q(e, "DIKHAO * SE v;") == "ee");
    CHECK_THAT(errorOf(e, "BANAO OR REPLACE VIEW v KAHO DIKHAO id SE v;"), ContainsSubstring("apne aap ko refer nahi kar sakti"));
}

TEST_CASE("WITH CHECK OPTION is stored, shown, replaced and refused on a view that cannot be written", "[sql3][view]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, dept TEXT);");
        ok(e, "BANAO VIEW a KAHO DIKHAO id, dept SE t JAHAN dept = 'cs' WITH CHECK OPTION;");
        ok(e, "BANAO VIEW b KAHO DIKHAO id, dept SE t JAHAN dept = 'cs' WITH LOCAL CHECK OPTION;");
        ok(e, "BANAO VIEW c KAHO DIKHAO id, dept SE t x JAHAN dept = 'cs' WITH CASCADED CHECK OPTION;");  // alias x, then WITH
        ok(e, "BANAO VIEW plain KAHO DIKHAO id SE t;");
        CHECK(q(e, "SHOW CREATE VIEW a;") == "a|BANAO VIEW a KAHO DIKHAO id, dept SE t JAHAN dept = 'cs' WITH CHECK OPTION;");
        CHECK(q(e, "BATAO a;") == "DIKHAO id, dept SE t JAHAN dept = 'cs'");  // BATAO shows the stored query only
        CHECK_THAT(errorOf(e, "BANAO VIEW d KAHO DIKHAO GINO(*) KAHO n SE t WITH CHECK OPTION;"),
                   ContainsSubstring("WITH CHECK OPTION sirf simple view par lag sakta hai"));
        CHECK_THAT(errorOf(e, "BANAO VIEW d KAHO DIKHAO id SE t WITH CHECK;"), ContainsSubstring("CHECK OPTION expected tha"));
        // OR REPLACE without the clause drops the option
        ok(e, "BANAO OR REPLACE VIEW c KAHO DIKHAO id, dept SE t JAHAN dept = 'cs';");
        CHECK(q(e, "SHOW CREATE VIEW c;") == "c|BANAO VIEW c KAHO DIKHAO id, dept SE t JAHAN dept = 'cs';");
    }
    // catalog.json carries the option; reloaded, it still rejects
    std::string catalog = slurp(dir.file("main/catalog.json"));
    CHECK_THAT(catalog, ContainsSubstring("\"view_check_options\""));
    Engine again(dir.str());
    CHECK(errorOf(again, "DAALO MEIN a MAAN (1, 'ee');").find("CHECK OPTION") != std::string::npos);
    ok(again, "DAALO MEIN a MAAN (1, 'cs');");
    ok(again, "HATAO VIEW a; HATAO VIEW b;");
    ok(again, "BANAO VIEW a KAHO DIKHAO id, dept SE t;");  // a dropped view forgets its option
    ok(again, "DAALO MEIN a MAAN (2, 'ee');");
}

TEST_CASE("a catalog without view_check_options (written by an older build) loads and is not rewritten with it", "[sql3][view]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO TABLE t (id INT); BANAO VIEW v KAHO DIKHAO id SE t;");
    }
    CHECK(slurp(dir.file("main/catalog.json")).find("view_check_options") == std::string::npos);
}

// ------------------------------------------------------------ DML through a simple view ----

TEST_CASE("DAALO / BADLO / MITAO through a simple view hit the base table", "[sql3][viewdml]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE emp (id INT AUTO_INCREMENT MUKHYA KUNJI, naam TEXT, dept TEXT WARNA 'none', sal INT);");
    ok(e, "BANAO VIEW cs KAHO DIKHAO id, naam KAHO who, sal SE emp JAHAN dept = 'cs';");
    // columns the view does not show get defaults / auto numbers / KHALI; aliases map back to the real column
    CHECK(run(e, "DAALO MEIN cs (who, sal) MAAN ('a', 10);").message == "1 row(s) daal di");
    CHECK(q(e, "DIKHAO id, naam, dept, sal SE emp;") == "1|a|none|10");
    CHECK(q(e, "DIKHAO * SE cs;") == "");  // dept 'none': the new row is not visible through the view (no CHECK OPTION)
    ok(e, "DAALO MEIN emp (naam, dept, sal) MAAN ('b', 'cs', 20), ('c', 'cs', 30), ('d', 'ee', 40);");
    CHECK(q(e, "DIKHAO * SE cs KRAM id;") == "2|b|20;3|c|30");
    // without a column list all of the view's columns, in the view's order
    ok(e, "DAALO MEIN cs MAAN (KHALI, 'x', 5);");
    CHECK(q(e, "DIKHAO id, naam, dept SE emp JAHAN naam = 'x';") == "5|x|none");
    // UPDATE: the view's JAHAN is ANDed in; names are the view's
    CHECK(run(e, "BADLO cs RAKHO sal = sal + 1 JAHAN who = 'c';").message == "1 row(s) badal di");
    CHECK(q(e, "DIKHAO naam, sal SE emp KRAM id;") == "a|10;b|20;c|31;d|40;x|5");
    CHECK(run(e, "BADLO cs RAKHO sal = 0;").message == "2 row(s) badal di");  // only b and c (dept = 'cs')
    CHECK(q(e, "DIKHAO naam, sal SE emp KRAM id;") == "a|10;b|0;c|0;d|40;x|5");
    CHECK(run(e, "BADLO cs RAKHO who = 'bb' JAHAN id = 2;").message == "1 row(s) badal di");
    CHECK(q(e, "DIKHAO naam SE emp JAHAN id = 2;") == "bb");
    CHECK(run(e, "BADLO cs RAKHO sal = 1 JAHAN id = 4;").message == "0 row(s) badal di");  // d is not in the view
    // DELETE
    CHECK(run(e, "MITAO SE cs JAHAN who = 'c';").message == "1 row(s) mita di");
    CHECK(q(e, "DIKHAO naam SE emp KRAM id;") == "a;bb;d;x");
    CHECK(run(e, "MITAO SE cs;").message == "1 row(s) mita di");  // only bb is left in the view
    CHECK(q(e, "DIKHAO naam SE emp KRAM id;") == "a;d;x");
    // names the view does not have
    CHECK(errorOf(e, "BADLO cs RAKHO dept = 'x';") == "Column 'dept' view 'cs' mein nahi hai");
    CHECK(errorOf(e, "DAALO MEIN cs (naam) MAAN ('z');") == "Column 'naam' view 'cs' mein nahi hai");  // it is called who here
    CHECK(errorOf(e, "MITAO SE cs JAHAN naam = 'a';") == "Column 'naam' view 'cs' mein nahi hai");
    // INSERT ... SELECT and a column count mismatch
    ok(e, "BANAO TABLE src (n TEXT, s INT); DAALO MEIN src MAAN ('p', 7), ('q', 8);");
    CHECK(run(e, "DAALO MEIN cs (who, sal) DIKHAO n, s SE src;").message == "2 row(s) daal di");
    CHECK(errorOf(e, "DAALO MEIN cs (who) MAAN ('z', 1);") == "1 values chahiye thi, 2 mili");
    // a trigger on the base table fires for rows written through the view
    ok(e, "BANAO TABLE log (m TEXT);");
    ok(e, "BANAO TRIGGER t1 BAAD DAALO PAR emp SHURU DAALO MEIN log MAAN (NAYA.naam); KHATAM;");
    ok(e, "DAALO MEIN cs (who) MAAN ('trig');");
    CHECK(q(e, "DIKHAO m SE log;") == "trig");
}

TEST_CASE("a SELECT * view accepts all columns; the view over a view and other shapes are refused", "[sql3][viewdml]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE p (id INT MUKHYA KUNJI, naam TEXT, score INT); BANAO TABLE d (id INT, label TEXT);");
    ok(e, "DAALO MEIN p MAAN (1, 'a', 10), (2, 'b', 20); DAALO MEIN d MAAN (1, 'one');");
    ok(e, "BANAO VIEW v KAHO DIKHAO * SE p;");
    ok(e, "DAALO MEIN v MAAN (3, 'c', 30);");
    CHECK(q(e, "DIKHAO naam SE p KRAM id;") == "a;b;c");
    ok(e, "BANAO VIEW qv KAHO DIKHAO p.id, p.naam SE p;");  // qualified references are plain columns too
    CHECK(run(e, "BADLO qv RAKHO naam = 'z' JAHAN id = 3;").message == "1 row(s) badal di");
    CHECK(q(e, "DIKHAO naam SE p JAHAN id = 3;") == "z");
    // refused shapes keep the classic message
    const std::vector<std::string> bad = {
        "DIKHAO ALAG naam SE p",                                  // DISTINCT
        "DIKHAO naam, GINO(*) KAHO n SE p SAMOOH naam",           // GROUP BY
        "DIKHAO GINO(*) KAHO n SE p",                             // aggregate
        "DIKHAO naam SE p SANYUKT DIKHAO label SE d",             // not even a single query -- BANAO VIEW refuses it
        "DIKHAO p.naam, d.label SE p MILAO d PAR p.id = d.id",    // JOIN
        "DIKHAO t.naam SE (DIKHAO naam SE p) t",                  // derived table
        "DIKHAO score * 2 KAHO s2 SE p",                          // expression
        "DIKHAO UPPER(naam) KAHO u SE p",                         // scalar function
        "DIKHAO naam SE p SIRF 2",                                // LIMIT
        "DIKHAO naam, naam KAHO naam SE p",                       // duplicate output name
        "DIKHAO 1 + 1",                                           // no table
    };
    for (const auto& body : bad) {
        INFO(body);
        if (run(e, "BANAO OR REPLACE VIEW v KAHO " + body + ";").error.empty()) {
            CHECK(errorOf(e, "DAALO MEIN v MAAN (9);") != "");
            CHECK(errorOf(e, "BADLO v RAKHO naam = 'x';") == kNotUpdatable);
            CHECK(errorOf(e, "MITAO SE v;") == kNotUpdatable);
        }
    }
    // a view over a view is not written through
    ok(e, "BANAO OR REPLACE VIEW v KAHO DIKHAO * SE p; BANAO VIEW vv KAHO DIKHAO * SE v;");
    CHECK(errorOf(e, "DAALO MEIN vv MAAN (8, 'q', 1);") == "'vv' ek VIEW hai, table nahi -- isme DAALO/BADLO/MITAO nahi kar sakte");
    // TRUNCATE / DROP TABLE / ALTER still refuse a view by name
    CHECK_THAT(errorOf(e, "SAAF TABLE v;"), ContainsSubstring("ek VIEW hai"));
    CHECK_THAT(errorOf(e, "HATAO TABLE v;"), ContainsSubstring("ek VIEW hai"));
    // BATAO / SHOW COLUMNS still describe a view
    CHECK(q(e, "BATAO v;") == "DIKHAO * SE p");
}

TEST_CASE("WITH CHECK OPTION refuses rows the view would not show", "[sql3][viewdml]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE emp (id INT MUKHYA KUNJI, naam TEXT, dept TEXT, sal INT);");
    ok(e, "DAALO MEIN emp MAAN (1, 'a', 'cs', 10), (2, 'b', 'cs', 20), (3, 'c', 'ee', 30);");
    ok(e, "BANAO VIEW rich KAHO DIKHAO id, naam, dept, sal SE emp JAHAN dept = 'cs' AUR sal >= 10 WITH CHECK OPTION;");
    const std::string refused = "View 'rich' ki WITH CHECK OPTION ke karan row reject: ye row view ki JAHAN shart poori nahi karti";
    CHECK(errorOf(e, "DAALO MEIN rich MAAN (4, 'd', 'ee', 50);") == refused);
    CHECK(errorOf(e, "DAALO MEIN rich MAAN (4, 'd', 'cs', 5);") == refused);
    CHECK(errorOf(e, "DAALO MEIN rich MAAN (4, 'd', 'cs', KHALI);") == refused);  // KHALI is not true
    CHECK(q(e, "DIKHAO GINO(*) SE emp;") == "3");                                  // nothing was written
    CHECK(run(e, "DAALO MEIN rich MAAN (4, 'd', 'cs', 50);").message == "1 row(s) daal di");
    // one bad row spoils the whole INSERT
    CHECK(errorOf(e, "DAALO MEIN rich MAAN (5, 'e', 'cs', 60), (6, 'f', 'ee', 60);") == refused);
    CHECK(q(e, "DIKHAO GINO(*) SE emp;") == "4");
    // UPDATE: the new values must still be visible
    CHECK(errorOf(e, "BADLO rich RAKHO dept = 'ee' JAHAN id = 1;") == refused);
    CHECK(q(e, "DIKHAO dept SE emp JAHAN id = 1;") == "cs");
    CHECK(errorOf(e, "BADLO rich RAKHO sal = sal - 15;") == refused);  // 10 - 15 breaks one row: nothing changes
    CHECK(q(e, "DIKHAO sal SE emp KRAM id;") == "10;20;30;50");
    CHECK(run(e, "BADLO rich RAKHO sal = sal + 1;").message == "3 row(s) badal di");
    // rows already invisible are never touched, and DELETE needs no check
    CHECK(run(e, "BADLO rich RAKHO dept = 'cs' JAHAN id = 3;").message == "0 row(s) badal di");
    CHECK(run(e, "MITAO SE rich JAHAN id = 1;").message == "1 row(s) mita di");
    // REPLACE and an upsert are checked too
    CHECK(errorOf(e, "REPLACE INTO rich MAAN (2, 'b', 'ee', 1);") == refused);
    CHECK(errorOf(e, "DAALO MEIN rich MAAN (2, 'b', 'cs', 21) TAKRAAV PAR BADLO dept = 'ee';") == refused);
    CHECK(run(e, "DAALO MEIN rich MAAN (2, 'b', 'cs', 21) TAKRAAV PAR BADLO sal = 99;").error.empty());
    CHECK(q(e, "DIKHAO sal SE emp JAHAN id = 2;") == "99");
    // a view WITHOUT the option lets the row slip out of sight
    ok(e, "BANAO VIEW loose KAHO DIKHAO id, dept SE emp JAHAN dept = 'cs';");
    CHECK(run(e, "BADLO loose RAKHO dept = 'ee' JAHAN id = 2;").message == "1 row(s) badal di");
    CHECK(q(e, "DIKHAO * SE loose KRAM id;") == "4|cs");
}

TEST_CASE("privileges on a view cover DAALO / BADLO / MITAO through it, and nothing else", "[sql3][viewdml][users]") {
    Users u;
    ok(u.admin, "BANAO VIEW young KAHO DIKHAO id, naam KAHO who SE students JAHAN umar < 21 WITH CHECK OPTION;");
    // ravi has no grant on the view yet
    CHECK(errorOf(u.ravi, "DAALO MEIN young (who) MAAN ('z');") == "'ravi' ko table 'young' par DAALO ka adhikar nahi hai");
    CHECK(errorOf(u.ravi, "BADLO young RAKHO who = 'z';") == "'ravi' ko table 'young' par BADLO ka adhikar nahi hai");
    CHECK(errorOf(u.ravi, "MITAO SE young;") == "'ravi' ko table 'young' par MITAO ka adhikar nahi hai");
    ok(u.admin, "ADHIKAR DO DIKHAO, BADLO, MITAO PAR young KO ravi;");
    // BADLO / MITAO now work through the view although ravi holds nothing on students itself
    CHECK(u.ravi.runScript("BADLO young RAKHO who = 'R2' JAHAN id = 1;")[0].message == "1 row(s) badal di");
    CHECK(q(u.admin, "DIKHAO naam SE students JAHAN id = 1;") == "R2");
    CHECK(errorOf(u.ravi, "DAALO MEIN young (id, who) MAAN (9, 'n');") == "'ravi' ko table 'young' par DAALO ka adhikar nahi hai");
    CHECK(errorOf(u.ravi, "DIKHAO * SE students;") == "'ravi' ko table 'students' par DIKHAO ka adhikar nahi hai");
    CHECK(u.ravi.runScript("MITAO SE young JAHAN id = 2;")[0].message == "1 row(s) mita di");
    CHECK(q(u.admin, "DIKHAO GINO(*) SE students;") == "1");
    ok(u.admin, "ADHIKAR DO DAALO PAR young KO ravi;");
    CHECK(u.ravi.runScript("DAALO MEIN young (id, who) MAAN (9, 'n');")[0].error.find("CHECK OPTION") != std::string::npos);  // umar stays KHALI
}

// ------------------------------------------------------------ roles ----

TEST_CASE("roles: BANAO / HATAO ROLE, grants to a role, ADHIKAR DO ROLE", "[sql3][roles]") {
    Users u;
    CHECK(run(u.admin, "BANAO ROLE clerk;").message == "Role 'clerk' ban gaya");
    CHECK_THAT(errorOf(u.admin, "BANAO ROLE clerk;"), ContainsSubstring("Role 'clerk' pehle se hai"));
    CHECK_THAT(errorOf(u.admin, "BANAO ROLE ravi;"), ContainsSubstring("naam ka user pehle se hai"));
    CHECK_THAT(errorOf(u.admin, "BANAO USER clerk GUPT 'x';"), ContainsSubstring("pehle se hai"));
    CHECK_THAT(errorOf(u.admin, "BANAO ROLE none;"), ContainsSubstring("role ka naam nahi ho sakta"));
    CHECK_THAT(errorOf(u.admin, "HATAO USER clerk;"), ContainsSubstring("User 'clerk' exist nahi karta"));
    CHECK_THAT(errorOf(u.admin, "HATAO ROLE ravi;"), ContainsSubstring("Role 'ravi' exist nahi karta"));
    CHECK(run(u.admin, "ADHIKAR DO SELECT, INSERT PAR students KO clerk;").message ==
          "'clerk' ko 'main.students' par DIKHAO, DAALO ka adhikar mil gaya");
    // ravi has nothing until he is given the role
    CHECK(errorOf(u.ravi, "DIKHAO * SE students;") == "'ravi' ko table 'students' par DIKHAO ka adhikar nahi hai");
    CHECK(run(u.admin, "ADHIKAR DO ROLE clerk KO ravi;").message == "Role clerk ab 'ravi' ko mil gaya");
    CHECK(u.ravi.runScript("DIKHAO naam SE students KRAM id;")[0].rows.size() == 2);
    CHECK(u.ravi.runScript("DAALO MEIN students MAAN (3, 'N', 'n@x', 30);")[0].message == "1 row(s) daal di");
    CHECK(errorOf(u.ravi, "MITAO SE students;") == "'ravi' ko table 'students' par MITAO ka adhikar nahi hai");
    // grants to a role are shown for the role; the user shows the role
    CHECK(q(u.admin, "SHOW GRANTS FOR clerk;") == "GRANT SELECT, INSERT ON main.students TO clerk");
    CHECK(q(u.admin, "SHOW GRANTS FOR ravi;") == "GRANT clerk TO ravi");
    // a role can only go to a user, and a user cannot be handed out like a role
    CHECK_THAT(errorOf(u.admin, "ADHIKAR DO ROLE clerk KO clerk;"), ContainsSubstring("ek role hai, user nahi"));
    CHECK_THAT(errorOf(u.admin, "ADHIKAR DO ROLE ghost KO ravi;"), ContainsSubstring("Role 'ghost' exist nahi karta"));
    CHECK_THAT(errorOf(u.admin, "ADHIKAR DO ROLE clerk KO ghost;"), ContainsSubstring("User 'ghost' exist nahi karta"));
    // revoking a privilege from the role takes it from everybody who has the role
    ok(u.admin, "ADHIKAR WAPAS INSERT PAR students SE clerk;");
    CHECK(errorOf(u.ravi, "DAALO MEIN students MAAN (4, 'M', 'm@x', 31);") == "'ravi' ko table 'students' par DAALO ka adhikar nahi hai");
    // ADHIKAR WAPAS ROLE
    CHECK(run(u.admin, "ADHIKAR WAPAS ROLE clerk SE ravi;").message == "Role clerk 'ravi' se wapas le liya");
    CHECK(errorOf(u.ravi, "DIKHAO * SE students;") == "'ravi' ko table 'students' par DIKHAO ka adhikar nahi hai");
    CHECK_THAT(errorOf(u.admin, "ADHIKAR WAPAS ROLE clerk SE ravi;"), ContainsSubstring("diya hi nahi gaya"));
    // dropping a role removes it from its holders
    ok(u.admin, "ADHIKAR DO ROLE clerk KO ravi;");
    CHECK(u.ravi.runScript("DIKHAO * SE students;")[0].error.empty());
    CHECK(run(u.admin, "HATAO ROLE clerk;").message == "Role 'clerk' hata diya");
    CHECK(errorOf(u.ravi, "DIKHAO * SE students;") == "'ravi' ko table 'students' par DIKHAO ka adhikar nahi hai");
    CHECK(q(u.admin, "SHOW GRANTS FOR ravi;") == "GRANT USAGE ON *.* TO ravi");
    // a restricted user can manage no roles
    CHECK_THAT(errorOf(u.ravi, "BANAO ROLE x;"), ContainsSubstring("superuser nahi hai -- 'CreateRole'"));
    CHECK_THAT(errorOf(u.ravi, "HATAO ROLE x;"), ContainsSubstring("superuser nahi hai -- 'DropRole'"));
    CHECK_THAT(errorOf(u.ravi, "ADHIKAR DO ROLE x KO ravi;"), ContainsSubstring("superuser nahi hai -- 'GrantRole'"));
    CHECK_THAT(errorOf(u.ravi, "ADHIKAR WAPAS DIKHAO PAR students SE ravi;"), ContainsSubstring("superuser nahi hai -- 'Revoke'"));
}

TEST_CASE("SET ROLE chooses which granted roles are in force", "[sql3][roles]") {
    Users u;
    ok(u.admin, "BANAO ROLE reader; BANAO ROLE writer;");
    ok(u.admin, "ADHIKAR DO DIKHAO PAR students KO reader; ADHIKAR DO DAALO PAR students KO writer;");
    ok(u.admin, "ADHIKAR DO DIKHAO PAR other KO ravi;");  // ravi's own grant
    ok(u.admin, "ADHIKAR DO ROLE reader KO ravi; ADHIKAR DO ROLE writer KO ravi;");
    auto can = [&](const std::string& sql) { return u.ravi.runScript(sql)[0].error.empty(); };
    // default: every granted role is active
    CHECK(can("DIKHAO * SE students;"));
    CHECK(can("DAALO MEIN students MAAN (10, 'a', 'a', 1);"));
    CHECK(run(u.ravi, "SET ROLE NONE;").message.find("koi role active nahi") != std::string::npos);
    CHECK_FALSE(can("DIKHAO * SE students;"));
    CHECK_FALSE(can("DAALO MEIN students MAAN (11, 'a', 'a', 1);"));
    CHECK(can("DIKHAO * SE other;"));  // his own grants stay
    ok(u.ravi, "SET ROLE reader;");
    CHECK(can("DIKHAO * SE students;"));
    CHECK_FALSE(can("DAALO MEIN students MAAN (11, 'a', 'a', 1);"));
    ok(u.ravi, "SET ROLE writer;");
    CHECK_FALSE(can("DIKHAO * SE students;"));
    CHECK(can("DAALO MEIN students MAAN (11, 'a', 'a', 1);"));
    ok(u.ravi, "SET ROLE reader, writer;");
    CHECK(can("DIKHAO * SE students;"));
    CHECK(can("DAALO MEIN students MAAN (12, 'a', 'a', 1);"));
    ok(u.ravi, "SET ROLE NONE;");
    ok(u.ravi, "SET ROLE SAB;");
    CHECK(can("DIKHAO * SE students;"));
    ok(u.ravi, "SET ROLE NONE;");
    ok(u.ravi, "SET ROLE ALL;");  // the English word works as well
    CHECK(can("DAALO MEIN students MAAN (13, 'a', 'a', 1);"));
    CHECK(errorOf(u.ravi, "SET ROLE ghost;") == "Role 'ghost' 'ravi' ko diya hi nahi gaya");
    ok(u.admin, "BANAO ROLE other_role;");
    CHECK(errorOf(u.ravi, "SET ROLE other_role;") == "Role 'other_role' 'ravi' ko diya hi nahi gaya");  // exists, but not his
    // the choice belongs to one session
    Engine second(u.inst);
    second.user = "ravi";
    ok(u.ravi, "SET ROLE NONE;");
    CHECK(second.runScript("DIKHAO * SE students;")[0].error.empty());
    // a superuser session: harmless
    CHECK_THAT(run(u.admin, "SET ROLE reader;").message, ContainsSubstring("koi asar nahi"));
}

TEST_CASE("users.json keeps its old shape and roles/grant options/column grants are written only when used", "[sql3][roles][users]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO USER ravi GUPT 'pw'; ADHIKAR DO DIKHAO, DAALO PAR t KO ravi;");
    }
    std::string plain = slurp(dir.file("users.json"));
    CHECK_THAT(plain, ContainsSubstring("\"main.t\""));
    for (const char* key : {"roles", "column_grants", "grant_option", "column_grant_option", "\"role\""})
        CHECK(plain.find(key) == std::string::npos);
    {
        Engine e(dir.str());
        ok(e, "BANAO ROLE r; ADHIKAR DO ROLE r KO ravi;");
        ok(e, "ADHIKAR DO DIKHAO (a, b) PAR t KO ravi;");
        ok(e, "ADHIKAR DO BADLO PAR t KO ravi WITH GRANT OPTION;");
    }
    std::string rich = slurp(dir.file("users.json"));
    CHECK_THAT(rich, ContainsSubstring("\"roles\""));
    CHECK_THAT(rich, ContainsSubstring("\"column_grants\""));
    CHECK_THAT(rich, ContainsSubstring("\"grant_option\""));
    CHECK_THAT(rich, ContainsSubstring("\"role\": true"));
    // reload from disk; the role can never log in; roles are not listed as users
    Engine again(dir.str());
    CHECK(q(again, "SHOW GRANTS FOR ravi;") ==
          "GRANT SELECT, INSERT ON main.t TO ravi;GRANT UPDATE ON main.t TO ravi WITH GRANT OPTION;GRANT SELECT (a, b) ON main.t TO ravi;GRANT r TO ravi");
    CHECK_FALSE(again.instance().users().verify("r", ""));
    CHECK(again.instance().users().names() == std::vector<std::string>{"ravi"});
    CHECK(again.instance().users().roleNames() == std::vector<std::string>{"r"});
    // taking everything back leaves the old shape again
    ok(again, "ADHIKAR WAPAS ROLE r SE ravi; ADHIKAR WAPAS DIKHAO, DAALO, BADLO PAR t SE ravi; HATAO ROLE r;");
    std::string clean = slurp(dir.file("users.json"));
    for (const char* key : {"roles", "column_grants", "grant_option", "column_grant_option", "\"role\""})
        CHECK(clean.find(key) == std::string::npos);
}

// ------------------------------------------------------------ WITH GRANT OPTION ----

TEST_CASE("WITH GRANT OPTION lets a user pass a privilege on, never more than they hold", "[sql3][grant]") {
    Users u;
    Engine asha(u.inst);
    asha.user = "asha";
    ok(u.admin, "ADHIKAR DO DIKHAO, DAALO PAR students KO ravi WITH GRANT OPTION;");
    ok(u.admin, "ADHIKAR DO BADLO PAR students KO ravi;");  // held, but without the option
    CHECK(q(u.admin, "SHOW GRANTS FOR ravi;") ==
          "GRANT UPDATE ON main.students TO ravi;GRANT SELECT, INSERT ON main.students TO ravi WITH GRANT OPTION");
    // ravi passes DIKHAO on
    CHECK(run(u.ravi, "ADHIKAR DO DIKHAO PAR students KO asha;").message == "'asha' ko 'main.students' par DIKHAO ka adhikar mil gaya");
    CHECK(asha.runScript("DIKHAO * SE students;")[0].error.empty());
    CHECK(errorOf(asha, "ADHIKAR DO DIKHAO PAR students KO ravi;") ==  // asha got no option, so she can pass nothing on
          "'asha' superuser nahi hai -- 'Grant' jaisa DDL/admin command sirf superuser (bina username connect kiya session) chala sakta hai");
    // not what he does not hold, not what he holds without the option, not on other tables
    CHECK_THAT(errorOf(u.ravi, "ADHIKAR DO MITAO PAR students KO asha;"), ContainsSubstring("superuser nahi hai -- 'Grant'"));
    CHECK_THAT(errorOf(u.ravi, "ADHIKAR DO DIKHAO, BADLO PAR students KO asha;"),
               ContainsSubstring("ke paas 'students' par BADLO ka WITH GRANT OPTION nahi hai"));
    CHECK_THAT(errorOf(u.ravi, "ADHIKAR DO SAB PAR students KO asha;"), ContainsSubstring("WITH GRANT OPTION nahi hai"));
    CHECK_THAT(errorOf(u.ravi, "ADHIKAR DO DIKHAO PAR other KO asha;"), ContainsSubstring("superuser nahi hai -- 'Grant'"));
    CHECK(errorOf(asha, "DAALO MEIN students MAAN (5, 'x', 'x', 1);") == "'asha' ko table 'students' par DAALO ka adhikar nahi hai");
    // he can pass the option on too, and give to a role
    ok(u.ravi, "ADHIKAR DO DAALO PAR students KO asha WITH GRANT OPTION;");
    CHECK(q(u.admin, "SHOW GRANTS FOR asha;") ==
          "GRANT SELECT ON main.students TO asha;GRANT INSERT ON main.students TO asha WITH GRANT OPTION");
    ok(u.admin, "BANAO ROLE r;");
    ok(asha, "ADHIKAR DO DAALO PAR students KO r;");
    CHECK(q(u.admin, "SHOW GRANTS FOR r;") == "GRANT INSERT ON main.students TO r");
    // the grant option survives a reload from disk
    Engine again(u.dir.str());
    CHECK(again.instance().users().access("asha", std::nullopt, "main", "students", "DAALO", true).full);
    CHECK_FALSE(again.instance().users().access("asha", std::nullopt, "main", "students", "DIKHAO", true).full);
    // a grant option held through a role counts, until the role is switched off
    ok(u.admin, "BANAO ROLE boss; ADHIKAR DO BADLO PAR students KO boss WITH GRANT OPTION; ADHIKAR DO ROLE boss KO asha;");
    CHECK(asha.runScript("ADHIKAR DO BADLO PAR students KO r;")[0].error.empty());
    ok(asha, "SET ROLE NONE;");
    CHECK_FALSE(asha.runScript("ADHIKAR DO BADLO PAR students KO r;")[0].error.empty());
    // revoking the privilege takes the option with it
    ok(u.admin, "ADHIKAR WAPAS DAALO PAR students SE asha; ADHIKAR DO DAALO PAR students KO asha;");
    CHECK(q(u.admin, "SHOW GRANTS FOR asha;") == "GRANT SELECT, INSERT ON main.students TO asha;GRANT boss TO asha");
}

// ------------------------------------------------------------ column-level grants ----

TEST_CASE("column-level SELECT: only the granted columns may be named", "[sql3][columns]") {
    Users u;
    ok(u.admin, "ADHIKAR DO SELECT (id, naam) PAR students KO ravi;");
    CHECK(u.ravi.runScript("DIKHAO id, naam SE students KRAM id;")[0].rows.size() == 2);
    CHECK(u.ravi.runScript("DIKHAO naam KAHO n SE students JAHAN id = 1 KRAM naam;")[0].rows.size() == 1);
    CHECK(u.ravi.runScript("DIKHAO GINO(*) SE students;")[0].error.empty());  // COUNT(*) reads no column
    CHECK(u.ravi.runScript("DIKHAO s.naam SE students s JAHAN s.id > 0;")[0].error.empty());
    const std::string noEmail = "'ravi' ko table 'students' ke column 'email' par DIKHAO ka adhikar nahi hai";
    CHECK(errorOf(u.ravi, "DIKHAO email SE students;") == noEmail);
    CHECK(errorOf(u.ravi, "DIKHAO id SE students JAHAN email = 'x';") == noEmail);
    CHECK(errorOf(u.ravi, "DIKHAO id SE students KRAM email;") == noEmail);
    CHECK(errorOf(u.ravi, "DIKHAO id, GINO(*) SE students SAMOOH id JINKA ADHIKTAM(umar) > 3;") ==
          "'ravi' ko table 'students' ke column 'umar' par DIKHAO ka adhikar nahi hai");
    CHECK(errorOf(u.ravi, "DIKHAO naam, AGAR email HAI KHALI TAB 1 WARNA 2 KHATAM SE students;") == noEmail);
    CHECK(errorOf(u.ravi, "DIKHAO UPPER(email) SE students;") == noEmail);
    CHECK(errorOf(u.ravi, "DIKHAO * SE students;") == noEmail);  // * expands to every column: email is the first not granted
    CHECK(errorOf(u.ravi, "DIKHAO s.* SE students s;") == noEmail);
    // sub-selects and joins cannot be used to get around it
    CHECK(errorOf(u.ravi, "DIKHAO (DIKHAO MAX(email) SE students) SE students;") == noEmail);
    CHECK(errorOf(u.ravi, "DIKHAO id SE students JAHAN id MEIN (DIKHAO umar SE students);") ==
          "'ravi' ko table 'students' ke column 'umar' par DIKHAO ka adhikar nahi hai");
    CHECK(errorOf(u.ravi, "DIKHAO t.email SE (DIKHAO id, email SE students) t;") == noEmail);
    CHECK(errorOf(u.ravi, "DIKHAO id SE students s JAHAN EXISTS (DIKHAO 1 SE students x JAHAN x.id = s.id AUR s.email = 'a');") == noEmail);
    CHECK(errorOf(u.ravi, "DIKHAO s.id SE students s MILAO other o PAR s.id = o.id;") ==
          "'ravi' ko table 'other' par DIKHAO ka adhikar nahi hai");
    // no grant at all still reads like before
    CHECK(errorOf(u.ravi, "DIKHAO id SE other;") == "'ravi' ko table 'other' par DIKHAO ka adhikar nahi hai");
    // the table-wide grant wins, and sub-selects now need their own tables' privileges
    ok(u.admin, "ADHIKAR DO SELECT PAR students KO ravi;");
    CHECK(u.ravi.runScript("DIKHAO * SE students;")[0].error.empty());
    CHECK(errorOf(u.ravi, "DIKHAO id SE students JAHAN id MEIN (DIKHAO id SE other);") ==
          "'ravi' ko table 'other' par DIKHAO ka adhikar nahi hai");
    // revoking the table-wide part leaves the columns; revoking columns takes them away one by one
    ok(u.admin, "ADHIKAR WAPAS SELECT (naam) PAR students SE ravi;");
    CHECK(q(u.admin, "SHOW GRANTS FOR ravi;") == "GRANT SELECT ON main.students TO ravi;GRANT SELECT (id) ON main.students TO ravi");
    ok(u.admin, "ADHIKAR WAPAS SELECT PAR students SE ravi;");
    CHECK(q(u.admin, "SHOW GRANTS FOR ravi;") == "GRANT USAGE ON *.* TO ravi");  // a table-wide revoke clears the columns as well
}

TEST_CASE("column-level SELECT through a view and a join", "[sql3][columns]") {
    Users u;
    ok(u.admin, "ADHIKAR DO DIKHAO (id) PAR students KO ravi; ADHIKAR DO DIKHAO (id, note) PAR other KO ravi;");
    CHECK(u.ravi.runScript("DIKHAO s.id, o.note SE students s MILAO other o PAR s.id = o.id;")[0].rows.size() == 1);
    CHECK(errorOf(u.ravi, "DIKHAO s.id, o.note SE students s MILAO other o PAR s.naam = o.id;") ==
          "'ravi' ko table 'students' ke column 'naam' par DIKHAO ka adhikar nahi hai");
    CHECK(errorOf(u.ravi, "DIKHAO naam SE students s MILAO other o PAR s.id = o.id;") ==
          "'ravi' ko table 'students' ke column 'naam' par DIKHAO ka adhikar nahi hai");
    // a view may be granted by columns too, and the base table stays closed
    ok(u.admin, "BANAO VIEW names KAHO DIKHAO id, naam, email SE students;");
    ok(u.admin, "ADHIKAR DO DIKHAO (id, naam) PAR names KO ravi;");
    CHECK(u.ravi.runScript("DIKHAO naam SE names;")[0].rows.size() == 2);
    CHECK(errorOf(u.ravi, "DIKHAO email SE names;") == "'ravi' ko table 'names' ke column 'email' par DIKHAO ka adhikar nahi hai");
    CHECK(errorOf(u.ravi, "DIKHAO * SE names;") == "'ravi' ko table 'names' ke column '*' par DIKHAO ka adhikar nahi hai");
}

TEST_CASE("column-level INSERT and UPDATE", "[sql3][columns]") {
    Users u;
    ok(u.admin, "ADHIKAR DO INSERT (id, naam) PAR students KO ravi; ADHIKAR DO UPDATE (email) PAR students KO ravi;");
    CHECK(u.ravi.runScript("DAALO MEIN students (id, naam) MAAN (3, 'N');")[0].message == "1 row(s) daal di");
    CHECK(u.ravi.runScript("DAALO MEIN students (naam, id) MAAN ('M', 4);")[0].message == "1 row(s) daal di");
    CHECK(errorOf(u.ravi, "DAALO MEIN students (id, umar) MAAN (5, 1);") ==
          "'ravi' ko table 'students' ke column 'umar' par DAALO ka adhikar nahi hai");
    // no column list = every column
    CHECK(errorOf(u.ravi, "DAALO MEIN students MAAN (5, 'a', 'b', 1);") ==
          "'ravi' ko table 'students' ke column 'email' par DAALO ka adhikar nahi hai");
    CHECK(u.ravi.runScript("BADLO students RAKHO email = 'z@x';")[0].message == "4 row(s) badal di");
    CHECK(errorOf(u.ravi, "BADLO students RAKHO naam = 'z';") ==
          "'ravi' ko table 'students' ke column 'naam' par BADLO ka adhikar nahi hai");
    CHECK(errorOf(u.ravi, "BADLO students RAKHO email = 'q', umar = 1;") ==
          "'ravi' ko table 'students' ke column 'umar' par BADLO ka adhikar nahi hai");
    CHECK(q(u.admin, "DIKHAO email SE students JAHAN id = 3;") == "z@x");
    CHECK(errorOf(u.ravi, "MITAO SE students;") == "'ravi' ko table 'students' par MITAO ka adhikar nahi hai");
    // INSERT / UPDATE grant is not SELECT: reading back is still refused
    CHECK(errorOf(u.ravi, "DIKHAO id SE students;") == "'ravi' ko table 'students' par DIKHAO ka adhikar nahi hai");
    // sub-selects inside the values need their tables' SELECT privilege
    CHECK(errorOf(u.ravi, "DAALO MEIN students (id, naam) MAAN (7, (DIKHAO note SE other));") ==
          "'ravi' ko table 'other' par DIKHAO ka adhikar nahi hai");
    CHECK(errorOf(u.ravi, "BADLO students RAKHO email = (DIKHAO note SE other);") ==
          "'ravi' ko table 'other' par DIKHAO ka adhikar nahi hai");
    // MITAO has no column form
    CHECK_THROWS_AS(parseScript("ADHIKAR DO MITAO (id) PAR students KO ravi;"), ParseError);
    // grants name real columns
    CHECK_THAT(errorOf(u.admin, "ADHIKAR DO SELECT (nope) PAR students KO ravi;"), ContainsSubstring("column 'nope' nahi hai"));
    CHECK_THAT(errorOf(u.admin, "ADHIKAR DO SELECT, SELECT PAR students KO ravi;"), ContainsSubstring("do baar likha hai"));
}

TEST_CASE("column grants can be passed on with WITH GRANT OPTION, within the columns held", "[sql3][columns][grant]") {
    Users u;
    Engine asha(u.inst);
    asha.user = "asha";
    ok(u.admin, "ADHIKAR DO SELECT (id, naam) PAR students KO ravi WITH GRANT OPTION;");
    ok(u.ravi, "ADHIKAR DO SELECT (naam) PAR students KO asha;");
    CHECK(asha.runScript("DIKHAO naam SE students;")[0].error.empty());
    CHECK(errorOf(asha, "DIKHAO id SE students;") == "'asha' ko table 'students' ke column 'id' par DIKHAO ka adhikar nahi hai");
    CHECK_THAT(errorOf(u.ravi, "ADHIKAR DO SELECT (id, email) PAR students KO asha;"), ContainsSubstring("WITH GRANT OPTION nahi hai"));
    CHECK_THAT(errorOf(u.ravi, "ADHIKAR DO SELECT PAR students KO asha;"), ContainsSubstring("WITH GRANT OPTION nahi hai"));
    CHECK(q(u.admin, "SHOW GRANTS FOR ravi;") == "GRANT SELECT (id, naam) ON main.students TO ravi WITH GRANT OPTION");
}

// ------------------------------------------------------------ SHOW family ----

TEST_CASE("SHOW GRANTS", "[sql3][show]") {
    Users u;
    Result none = run(u.admin, "SHOW GRANTS FOR ravi;");
    CHECK(none.columns == std::vector<std::string>{"Grants for ravi"});
    CHECK(flat(none) == "GRANT USAGE ON *.* TO ravi");
    ok(u.admin, "ADHIKAR DO SELECT, INSERT PAR students KO ravi; ADHIKAR DO DIKHAO PAR other KO ravi;");
    CHECK(q(u.admin, "SHOW GRANTS FOR ravi;") == "GRANT SELECT, INSERT ON main.students TO ravi;GRANT SELECT ON main.other TO ravi");
    CHECK(q(u.admin, "DIKHAO GRANTS FOR ravi;") == "GRANT SELECT, INSERT ON main.students TO ravi;GRANT SELECT ON main.other TO ravi");
    // without FOR: the session's own user
    CHECK(run(u.ravi, "SHOW GRANTS;").columns == std::vector<std::string>{"Grants for ravi"});
    CHECK(flat(run(u.ravi, "DIKHAO GRANTS;")) == "GRANT SELECT, INSERT ON main.students TO ravi;GRANT SELECT ON main.other TO ravi");
    CHECK(flat(run(u.ravi, "SHOW GRANTS FOR ravi;")) == "GRANT SELECT, INSERT ON main.students TO ravi;GRANT SELECT ON main.other TO ravi");
    CHECK_THAT(errorOf(u.ravi, "SHOW GRANTS FOR asha;"), ContainsSubstring("superuser nahi hai -- 'ShowGrants'"));
    CHECK_THAT(errorOf(u.admin, "SHOW GRANTS FOR ghost;"), ContainsSubstring("User 'ghost' exist nahi karta"));
    CHECK(run(u.admin, "SHOW GRANTS;").columns == std::vector<std::string>{"Grants for superuser"});
    // grants are per database
    ok(u.admin, "BANAO DATABASE school; ISTEMAL school; ADHIKAR DO MITAO PAR t KO ravi;");
    CHECK(q(u.admin, "SHOW GRANTS FOR ravi;") ==
          "GRANT SELECT, INSERT ON main.students TO ravi;GRANT SELECT ON main.other TO ravi;GRANT DELETE ON school.t TO ravi");
}

TEST_CASE("SHOW TABLES / VIEWS / DATABASES / TRIGGERS / PROCEDURE STATUS and their DIKHAO spellings", "[sql3][show]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE b (x INT); BANAO TABLE a (x INT); BANAO VIEW v KAHO DIKHAO x SE a;");
    CHECK(q(e, "SHOW TABLES;") == "a;b");
    CHECK(q(e, "show tables") == "a;b");
    CHECK(q(e, "SHOW VIEWS;") == "v");
    CHECK(run(e, "SHOW TABLES;").message == "2 table(s) in 'main'");
    CHECK(run(e, "SHOW TABLES;").columns == run(e, "DIKHAO TABLES;").columns);
    ok(e, "BANAO DATABASE zeta; BANAO DATABASE college;");
    CHECK(q(e, "SHOW DATABASES;") == "college;main;zeta");
    CHECK(q(e, "DIKHAO DATABASES;") == "college;main;zeta");
    // triggers
    ok(e, "BANAO TABLE log (m TEXT);");
    ok(e, "BANAO TRIGGER tr1 BAAD DAALO PAR a SHURU DAALO MEIN log MAAN (NAYA.x); KHATAM;");
    Result trig = run(e, "SHOW TRIGGERS;");
    CHECK(trig.columns == std::vector<std::string>{"trigger", "timing", "event", "table", "body"});
    CHECK(flat(trig) == "tr1|BAAD|DAALO|a|DAALO MEIN log MAAN (NAYA.x);");
    CHECK(q(e, "DIKHAO TRIGGERS;") == "tr1|BAAD|DAALO|a|DAALO MEIN log MAAN (NAYA.x);");
    CHECK(trig.message == "1 trigger(s) in 'main'");
    // procedures: name, database, created -- across all databases
    ok(e, "BANAO PROCEDURE bump (p_id INT) SHURU BADLO a RAKHO x = x + p_id; KHATAM;");
    ok(e, "ISTEMAL college; BANAO TABLE c (x INT); BANAO PROCEDURE other_p () SHURU MITAO SE c; KHATAM; ISTEMAL main;");
    Result procs = run(e, "SHOW PROCEDURE STATUS;");
    CHECK(procs.columns == std::vector<std::string>{"name", "database", "created"});
    REQUIRE(procs.rows.size() == 2);
    CHECK(formatValue(procs.rows[0][0]) + "|" + formatValue(procs.rows[0][1]) == "other_p|college");
    CHECK(formatValue(procs.rows[1][0]) + "|" + formatValue(procs.rows[1][1]) == "bump|main");
    for (const auto& row : procs.rows) {
        std::string created = formatValue(row[2]);
        INFO(created);
        REQUIRE(created.size() == 19);  // 'YYYY-MM-DD HH:MM:SS'
        CHECK(created[4] == '-');
        CHECK(created[10] == ' ');
        CHECK(created[13] == ':');
    }
    CHECK(run(e, "DIKHAO PROCEDURES;").rows.size() == 2);
    CHECK(run(e, "SHOW PROCEDURES;").rows.size() == 2);
    // a procedure without a recorded time (made by an older build) shows KHALI
    e.catalog().procedures["old"] = nlohmann::ordered_json::parse(R"({"params": [], "body_text": "MITAO SE a;"})");
    e.catalog().save();
    Result withOld = run(e, "SHOW PROCEDURE STATUS;");
    bool sawOld = false;
    for (const auto& row : withOld.rows)
        if (formatValue(row[0]) == "old") {
            sawOld = true;
            CHECK(row[2].isNull());
        }
    CHECK(sawOld);
    // restricted users do not get to list the schema
    e.instance().users().create("ravi", "pw");
    Engine ravi(e.instance().dataDir());
    ravi.user = "ravi";
    CHECK_THAT(errorOf(ravi, "SHOW TRIGGERS;"), ContainsSubstring("superuser nahi hai"));
}

TEST_CASE("SHOW COLUMNS FROM is BATAO; SHOW CREATE TABLE gives back a statement that rebuilds the table", "[sql3][show]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE depts (id INT MUKHYA KUNJI, naam TEXT ANOKHA);");
    ok(e, "BANAO TABLE emp ("
          "id INT AUTO_INCREMENT MUKHYA KUNJI, naam VARCHAR(30) ZAROORI, email TEXT ANOKHA, "
          "umar INT WARNA 18 SHART (umar >= 0 AUR umar < 150), kind ENUM('a', 'it''s') WARNA 'a', "
          "joined DATETIME, ratio FLOAT WARNA 1.5, ok BOOL WARNA SACH, born DATE WARNA '2000-01-02', "
          "did INT SANDARBH depts(id) ON DELETE SET NULL ON UPDATE CASCADE, "
          "boss INT, grp INT, other INT, "
          "ANOKHA (grp, other), "
          "CONSTRAINT fk_boss FOREIGN KEY (boss) SANDARBH depts(id) ON DELETE CASCADE);");
    ok(e, "BANAO TABLE pairs (a INT, b INT, MUKHYA KUNJI (a, b));");
    for (const std::string name : {"depts", "emp", "pairs"}) {
        INFO(name);
        std::string beforeDescribe = q(e, "BATAO " + name + ";");
        Result created = run(e, "SHOW CREATE TABLE " + name + ";");
        REQUIRE(created.error.empty());
        CHECK(created.columns == std::vector<std::string>{"table", "create_table"});
        std::string text = std::get<std::string>(created.rows[0][1].data);
        INFO(text);
        // run it in an empty database: BATAO must come out the same
        TempDir other;
        Engine copy(other.str());
        if (name != "depts") ok(copy, "BANAO TABLE depts (id INT MUKHYA KUNJI, naam TEXT ANOKHA);");
        Result made = run(copy, text);
        INFO(made.error);
        REQUIRE(made.error.empty());
        CHECK(flat(run(copy, "BATAO " + name + ";")) == beforeDescribe);
    }
    std::string emp = std::get<std::string>(run(e, "SHOW CREATE TABLE emp;").rows[0][1].data);
    CHECK_THAT(emp, ContainsSubstring("id INT AUTO_INCREMENT MUKHYA KUNJI"));
    CHECK_THAT(emp, ContainsSubstring("naam TEXT(30) ZAROORI"));
    CHECK_THAT(emp, ContainsSubstring("email TEXT ANOKHA"));
    CHECK_THAT(emp, ContainsSubstring("umar INT WARNA 18 SHART (umar >= 0 AUR umar < 150)"));
    CHECK_THAT(emp, ContainsSubstring("kind ENUM('a', 'it''s') WARNA 'a'"));
    CHECK_THAT(emp, ContainsSubstring("ok BOOL WARNA SACH"));
    CHECK_THAT(emp, ContainsSubstring("born DATE WARNA '2000-01-02'"));
    CHECK_THAT(emp, ContainsSubstring("did INT SANDARBH depts(id) ON DELETE SET NULL ON UPDATE CASCADE"));
    CHECK_THAT(emp, ContainsSubstring("ANOKHA (grp, other)"));
    CHECK_THAT(emp, ContainsSubstring("CONSTRAINT fk_boss FOREIGN KEY (boss) SANDARBH depts(id) ON DELETE CASCADE"));
    CHECK_THAT(std::get<std::string>(run(e, "SHOW CREATE TABLE pairs;").rows[0][1].data), ContainsSubstring("MUKHYA KUNJI (a, b)"));
    // SHOW COLUMNS FROM
    CHECK(flat(run(e, "SHOW COLUMNS FROM emp;")) == flat(run(e, "BATAO emp;")));
    CHECK(flat(run(e, "SHOW COLUMNS SE emp;")) == flat(run(e, "BATAO emp;")));
    CHECK(flat(run(e, "show fields from emp")) == flat(run(e, "BATAO emp;")));
    CHECK_THAT(errorOf(e, "SHOW CREATE TABLE nope;"), ContainsSubstring("Table 'nope' exist nahi karta"));
    ok(e, "BANAO VIEW v KAHO DIKHAO id SE depts;");
    CHECK_THAT(errorOf(e, "SHOW CREATE TABLE v;"), ContainsSubstring("ek VIEW hai -- SHOW CREATE VIEW v use karo"));
    CHECK_THAT(errorOf(e, "SHOW CREATE VIEW depts;"), ContainsSubstring("ek table hai, VIEW nahi"));
    CHECK_THAT(errorOf(e, "SHOW CREATE VIEW nope;"), ContainsSubstring("View 'nope' exist nahi karta"));
    CHECK(q(e, "SHOW COLUMNS FROM v;") == "DIKHAO id SE depts");
}

TEST_CASE("SHOW and SET are statement words only; show, grants, role stay usable as names", "[sql3][show]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (show INT, grants INT, role TEXT, status INT, databases INT);");
    ok(e, "DAALO MEIN t MAAN (1, 2, 'r', 3, 4);");
    CHECK(q(e, "DIKHAO show, grants, role SE t;") == "1|2|r");
    CHECK(q(e, "DIKHAO grants SE t;") == "2");
    CHECK(q(e, "DIKHAO databases SE t;") == "4");
    CHECK(q(e, "DIKHAO status SE t;") == "3");
    CHECK(run(e, "BADLO t RAKHO show = 10, role = 'q';").message == "1 row(s) badal di");
    CHECK(q(e, "DIKHAO show SE t JAHAN role = 'q';") == "10");
    ok(e, "BANAO TABLE role (x INT);");  // BANAO TABLE role, not BANAO ROLE
    CHECK_THROWS_AS(parseScript("SHOW;"), ParseError);
    CHECK_THROWS_AS(parseScript("SHOW NOTHING;"), ParseError);
    CHECK_THROWS_AS(parseScript("SHOW CREATE;"), ParseError);
    CHECK_THROWS_AS(parseScript("SHOW PROCEDURE;"), ParseError);
    CHECK_THROWS_AS(parseScript("SHOW COLUMNS emp;"), ParseError);
    CHECK_THROWS_AS(parseScript("ADHIKAR DO r KO ravi;"), ParseError);  // needs the word ROLE
    CHECK(parseScript("ADHIKAR DO ROLE r KO ravi;").size() == 1);
}

// ------------------------------------------------------------ over the server protocol ----

TEST_CASE("roles, column grants and SHOW work for a -U user over the wire", "[sql3][server]") {
    RunningServer s;
    {
        ConnectOptions admin;
        admin.port = s.port();
        Connection a(admin);
        a.execute("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, email TEXT)");
        a.execute("DAALO MEIN students MAAN (1, 'Ravi', 'r@x'), (2, 'Priya', 'p@x')");
        a.execute("BANAO DATABASE school");
        a.execute("BANAO VIEW pub KAHO DIKHAO id, naam SE students WITH CHECK OPTION");
        a.execute("BANAO USER ravi GUPT 'pw'; BANAO ROLE viewer; BANAO ROLE editor");
        a.execute("ADHIKAR DO SELECT (id, naam) PAR students KO viewer");
        a.execute("ADHIKAR DO BADLO, MITAO PAR pub KO editor");
        a.execute("ADHIKAR DO ROLE viewer KO ravi; ADHIKAR DO ROLE editor KO ravi");
    }
    ConnectOptions options;
    options.port = s.port();
    options.user = "ravi";
    options.password = "pw";
    options.database = "main";  // ISTEMAL at hello time works for a restricted user now
    Connection ravi(options);
    CHECK(ravi.currentDb() == "main");
    auto names = ravi.execute("DIKHAO naam SE students KRAM id")[0];
    REQUIRE(names.rows.size() == 2);
    CHECK(std::get<std::string>(names.rows[0][0].data) == "Ravi");
    CHECK(ravi.runScript("DIKHAO email SE students")[0].error.find("ke column 'email' par DIKHAO ka adhikar nahi hai") != std::string::npos);
    CHECK(ravi.runScript("BADLO pub RAKHO naam = 'R2' JAHAN id = 1")[0].message == "1 row(s) badal di");
    CHECK(ravi.runScript("DAALO MEIN pub (id, naam) MAAN (3, 'z')")[0].error.find("adhikar nahi hai") != std::string::npos);
    auto grants = ravi.execute("SHOW GRANTS")[0];
    CHECK(grants.columns == std::vector<std::string>{"Grants for ravi"});
    CHECK(grants.rows.size() == 2);  // GRANT viewer TO ravi, GRANT editor TO ravi
    // SET ROLE is per connection
    CHECK(ravi.runScript("SET ROLE viewer")[0].error.empty());
    CHECK(ravi.runScript("BADLO pub RAKHO naam = 'R3' JAHAN id = 1")[0].error.find("adhikar nahi hai") != std::string::npos);
    CHECK(ravi.runScript("DIKHAO naam SE students")[0].error.empty());
    CHECK(ravi.runScript("SET ROLE editor")[0].error.empty());
    CHECK(ravi.runScript("DIKHAO naam SE students")[0].error.find("adhikar nahi hai") != std::string::npos);
    CHECK(ravi.runScript("BADLO pub RAKHO naam = 'R3' JAHAN id = 1")[0].error.empty());
    CHECK(ravi.runScript("SET ROLE NONE")[0].error.empty());
    CHECK(ravi.runScript("MITAO SE pub")[0].error.find("adhikar nahi hai") != std::string::npos);
    Connection other(options);  // a new connection starts with every role active
    CHECK(other.runScript("MITAO SE pub JAHAN id = 2")[0].message == "1 row(s) mita di");
    CHECK(ravi.runScript("BANAO ROLE x")[0].error.find("superuser nahi hai") != std::string::npos);
    CHECK(ravi.runScript("ISTEMAL school")[0].error.empty());
    CHECK(ravi.currentDb() == "school");
    CHECK(ravi.runScript("DIKHAO naam SE students")[0].error.find("adhikar nahi hai") != std::string::npos);  // grants are per database
}

// ------------------------------------------------------------ highlighter ----

TEST_CASE("the highlighter knows the batch 3 words", "[sql3][highlight]") {
    auto kinds = [](const std::string& line) {
        std::string out;
        for (const auto& span : highlight::spans(line)) {
            if (!out.empty()) out += ";";
            out += line.substr(span.start, span.end - span.start) + "=" + highlight::kindName(span.kind);
        }
        return out;
    };
    CHECK(kinds("SHOW GRANTS FOR ravi") == "SHOW=keyword;GRANTS=keyword;FOR=keyword");
    CHECK(kinds("SET ROLE NONE") == "SET=keyword;ROLE=keyword;NONE=keyword");
    CHECK(kinds("WITH CHECK OPTION") == "WITH=keyword;CHECK=keyword;OPTION=keyword");
    CHECK(kinds("OR REPLACE VIEW") == "OR=keyword;REPLACE=keyword;VIEW=keyword");
    CHECK(kinds("SHOW CREATE TABLE") == "SHOW=keyword;CREATE=keyword;TABLE=keyword");
    CHECK(kinds("SHOW PROCEDURE STATUS") == "SHOW=keyword;PROCEDURE=keyword;STATUS=keyword");
}
