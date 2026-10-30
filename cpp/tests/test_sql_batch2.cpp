// cpp/tests/test_sql_batch2.cpp -- more MySQL-style SQL: AUTO_INCREMENT, ENUM, DATETIME, CREATE TABLE ... AS SELECT /
// LIKE, foreign key actions (ON DELETE / ON UPDATE CASCADE / SET NULL), ALTER TABLE CHANGE / MODIFY / AFTER / FIRST,
// named foreign keys, REPLACE INTO, DIKHAO without SE, LIMIT, `/` and DIV.
//
// MeraDB-C++ only (the Python engine does not have them), so every expectation is written out here.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "golden_runner.h"
#include "meradb/catalog.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/storage.h"
#include "test_util.h"

using namespace meradb;
using Catch::Matchers::ContainsSubstring;
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
    REQUIRE(r.error.empty());
}

}  // namespace

// ------------------------------------------------------------ AUTO_INCREMENT ----

TEST_CASE("AUTO_INCREMENT numbers rows when the column is left out or KHALI", "[sql2][auto]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT AUTO_INCREMENT MUKHYA KUNJI, naam TEXT);");
    ok(e, "DAALO MEIN t (naam) MAAN ('a'), ('b');");
    ok(e, "DAALO MEIN t (id, naam) MAAN (KHALI, 'c');");
    REQUIRE(q(e, "DIKHAO id, naam SE t KRAM id;") == "1|a;2|b;3|c");
    ok(e, "DAALO MEIN t (id, naam) MAAN (10, 'x'), (KHALI, 'y');");  // an explicit value moves the counter on
    REQUIRE(q(e, "DIKHAO id, naam SE t KRAM id;") == "1|a;2|b;3|c;10|x;11|y");
    ok(e, "DAALO MEIN t MAAN (KHALI, 'z');");
    REQUIRE(q(e, "DIKHAO MAX(id) SE t;") == "12");
    ok(e, "DAALO MEIN t (naam) DIKHAO naam SE t JAHAN id < 3;");  // INSERT ... SELECT numbers too
    REQUIRE(q(e, "DIKHAO MAX(id) SE t;") == "14");
    REQUIRE(errorOf(e, "DAALO MEIN t (id, naam) MAAN (2, 'dup');").find("Duplicate") != std::string::npos);
}

TEST_CASE("AUTO_ANK alias, plain (non-key) column and the DESCRIBE flag", "[sql2][auto]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (n INT AUTO_ANK, naam TEXT);");
    ok(e, "DAALO MEIN t (naam) MAAN ('a'), ('b');");
    ok(e, "DAALO MEIN t (n, naam) MAAN (7, 'c');");
    ok(e, "DAALO MEIN t (naam) MAAN ('d');");
    REQUIRE(q(e, "DIKHAO n SE t;") == "1;2;7;8");
    REQUIRE(q(e, "BATAO t;") == "n|INT|ZAROORI AUTO_INCREMENT;naam|TEXT|");
}

TEST_CASE("AUTO_INCREMENT rules", "[sql2][auto]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE(errorOf(e, "BANAO TABLE t (a TEXT AUTO_INCREMENT);") == "Column 'a': AUTO_INCREMENT sirf INT column par ho sakta hai");
    REQUIRE(errorOf(e, "BANAO TABLE t (a INT AUTO_INCREMENT, b INT AUTO_INCREMENT);") ==
            "Ek table mein sirf ek AUTO_INCREMENT column ho sakta hai");
    REQUIRE(errorOf(e, "BANAO TABLE t (a INT AUTO_INCREMENT WARNA 3);") ==
            "Column 'a': AUTO_INCREMENT ke saath WARNA nahi chalta");
    ok(e, "BANAO TABLE t (naam TEXT);");
    ok(e, "DAALO MEIN t MAAN ('a'), ('b'), ('c');");
    ok(e, "SUDHARO TABLE t JODO id INT AUTO_INCREMENT MUKHYA KUNJI FIRST;");  // existing rows are numbered
    REQUIRE(q(e, "DIKHAO id, naam SE t KRAM id;") == "1|a;2|b;3|c");
    ok(e, "DAALO MEIN t (naam) MAAN ('d');");
    REQUIRE(q(e, "DIKHAO id SE t JAHAN naam = 'd';") == "4");
    REQUIRE(errorOf(e, "SUDHARO TABLE t JODO id2 INT AUTO_INCREMENT;") == "Ek table mein sirf ek AUTO_INCREMENT column ho sakta hai");
}

TEST_CASE("AUTO_INCREMENT survives a reload and the catalog keeps the flag", "[sql2][auto]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO TABLE t (id INT AUTO_INCREMENT MUKHYA KUNJI, naam TEXT);");
        ok(e, "DAALO MEIN t (naam) MAAN ('a'), ('b');");
    }
    Engine again(dir.str());
    ok(again, "DAALO MEIN t (naam) MAAN ('c');");
    REQUIRE(q(again, "DIKHAO id SE t KRAM id;") == "1;2;3");
    REQUIRE(again.catalog().get("t").columns[0].autoIncrement);
}

// ---------------------------------------------------------------------- ENUM ----

TEST_CASE("ENUM accepts only its members", "[sql2][enum]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT, s ENUM('open', 'closed') WARNA 'open', z ENUM('x') ZAROORI WARNA 'x');");
    ok(e, "DAALO MEIN t (id) MAAN (1);");
    ok(e, "DAALO MEIN t (id, s) MAAN (2, 'closed'), (3, KHALI);");  // KHALI is allowed unless ZAROORI
    REQUIRE(q(e, "DIKHAO id, s SE t KRAM id;") == "1|open;2|closed;3|KHALI");
    REQUIRE(errorOf(e, "DAALO MEIN t (id, s) MAAN (4, 'stuck');") ==
            "Column 's' ENUM('open', 'closed') hai, par value 'stuck' mili -- sirf inhi mein se ek chalegi");
    REQUIRE(errorOf(e, "DAALO MEIN t (id, z) MAAN (4, KHALI);") == "Column 'z' ZAROORI hai, KHALI nahi ho sakta");
    REQUIRE(errorOf(e, "BADLO t RAKHO s = 'nope' JAHAN id = 1;").find("ENUM") != std::string::npos);
    ok(e, "BADLO t RAKHO s = 'closed' JAHAN id = 1;");
    REQUIRE(q(e, "DIKHAO id SE t JAHAN s = 'closed' KRAM id;") == "1;2");  // compared as text
    REQUIRE(q(e, "BATAO t;") == "id|INT|;s|ENUM('open','closed')|WARNA 'open';z|ENUM('x')|ZAROORI WARNA 'x'");
    REQUIRE(errorOf(e, "BANAO TABLE bad (s ENUM('a') WARNA 'b');").find("ENUM") != std::string::npos);
    REQUIRE_THROWS_AS(parseScript("BANAO TABLE bad (s ENUM('a', 'a'));"), ParseError);
    REQUIRE_THROWS_AS(parseScript("BANAO TABLE bad (s ENUM());"), ParseError);
    REQUIRE_THROWS_AS(parseScript("BANAO TABLE bad (s ENUM(1, 2));"), ParseError);
}

TEST_CASE("ENUM persists through save and reload", "[sql2][enum]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO TABLE t (s ENUM('a', 'b c'));");
        ok(e, "DAALO MEIN t MAAN ('b c');");
    }
    Engine again(dir.str());
    REQUIRE(again.catalog().get("t").columns[0].enumValues == std::vector<std::string>{"a", "b c"});
    REQUIRE(q(again, "DIKHAO * SE t;") == "b c");
    REQUIRE(errorOf(again, "DAALO MEIN t MAAN ('c');").find("ENUM") != std::string::npos);
}

// ------------------------------------------------------------------ DATETIME ----

TEST_CASE("DATETIME validates and normalises", "[sql2][datetime]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT, at DATETIME, ts TIMESTAMP);");
    ok(e, "DAALO MEIN t (id, at) MAAN (1, '2024-03-05 09:30:00'), (2, '2024-03-05'), (3, '2024-03-05 09:30'), "
          "(4, '2024-03-05T01:02:03');");
    REQUIRE(q(e, "DIKHAO id, at SE t KRAM id;") ==
            "1|2024-03-05 09:30:00;2|2024-03-05 00:00:00;3|2024-03-05 09:30:00;4|2024-03-05 01:02:03");
    for (const char* bad : {"'2024-13-01 00:00:00'", "'2024-02-30'", "'2024-03-05 24:00:00'", "'2024-03-05 10:61:00'",
                            "'2024-03-05 10:00:00 '", "'hello'", "'2024-03-05 1:00:00'", "12"}) {
        INFO(bad);
        REQUIRE_THAT(errorOf(e, std::string("DAALO MEIN t (id, at) MAAN (9, ") + bad + ");"), ContainsSubstring("DATETIME"));
    }
    REQUIRE(q(e, "BATAO t;") == "id|INT|;at|DATETIME|;ts|DATETIME|");
}

TEST_CASE("DATETIME takes NOW(), compares, sorts, groups and feeds the date functions", "[sql2][datetime]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT, at DATETIME);");
    ok(e, "DAALO MEIN t (id, at) MAAN (1, NOW());");
    Result r = run(e, "DIKHAO at SE t;");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(formatValue(r.rows[0][0]).size() == 19);
    ok(e, "BADLO t RAKHO at = '2024-01-31 23:59:59' JAHAN id = 1;");
    ok(e, "DAALO MEIN t (id, at) MAAN (2, '2024-02-01 08:00:00'), (3, '2023-12-31 12:00:00'), (4, '2024-02-01 08:00:00');");
    REQUIRE(q(e, "DIKHAO id SE t KRAM at, id;") == "3;1;2;4");
    REQUIRE(q(e, "DIKHAO id SE t KRAM at ULTA, id;") == "2;4;1;3");
    REQUIRE(q(e, "DIKHAO NYUNTAM(at), ADHIKTAM(at) SE t;") == "2023-12-31 12:00:00|2024-02-01 08:00:00");
    REQUIRE(q(e, "DIKHAO at, GINO(*) SE t SAMOOH at KRAM at;") ==
            "2023-12-31 12:00:00|1;2024-01-31 23:59:59|1;2024-02-01 08:00:00|2");
    REQUIRE(q(e, "DIKHAO id SE t JAHAN at > '2024-01-31 23:59:59' KRAM id;") == "2;4");
    REQUIRE(q(e, "DIKHAO id SE t JAHAN at >= '2024-01-31' AUR at < '2024-02-01' KRAM id;") == "1");  // date-only = midnight
    REQUIRE(q(e, "DIKHAO id SE t JAHAN at BEECH '2024-01-01' AUR '2024-01-31 23:59:59';") == "1");
    REQUIRE(q(e, "DIKHAO YEAR(at), MONTH(at), DAY(at) SE t JAHAN id = 1;") == "2024|1|31");
    REQUIRE(q(e, "DIKHAO DATEDIFF(at, '2024-01-01') SE t JAHAN id = 2;") == "31");
    REQUIRE(q(e, "DIKHAO DATE_FORMAT(at, '%d/%m/%Y %H:%i') SE t JAHAN id = 1;") == "31/01/2024 23:59");
    REQUIRE(q(e, "DIKHAO CAST('2024-05-06' KAHO DATETIME) SE t JAHAN id = 1;") == "2024-05-06 00:00:00");
    REQUIRE(q(e, "DIKHAO CAST(at KAHO DATE) SE t JAHAN id = 1;") == "2024-01-31");
}

TEST_CASE("DATETIME as ANOKHA / MUKHYA KUNJI, hash join key and WARNA", "[sql2][datetime]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (at DATETIME MUKHYA KUNJI, tag TEXT, made DATETIME WARNA '2020-01-01');");
    ok(e, "DAALO MEIN t (at, tag) MAAN ('2024-01-01', 'a'), ('2024-01-01 00:00:01', 'b');");
    REQUIRE(errorOf(e, "DAALO MEIN t (at, tag) MAAN ('2024-01-01 00:00:00', 'dup');").find("Duplicate") != std::string::npos);
    REQUIRE(q(e, "DIKHAO tag, made SE t KRAM at;") == "a|2020-01-01 00:00:00;b|2020-01-01 00:00:00");
    REQUIRE(q(e, "DIKHAO a.tag, b.tag SE t a MILAO t b PAR a.at = b.at KRAM a.tag;") == "a|a;b|b");
    REQUIRE(q(e, "DIKHAO tag SE t JAHAN at = '2024-01-01 00:00:01';") == "b");
}

TEST_CASE("DATETIME round-trips through the .tbl file and the catalog", "[sql2][datetime]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO TABLE t (id INT, at DATETIME WARNA '2001-02-03 04:05:06');");
        ok(e, "DAALO MEIN t (id, at) MAAN (1, '2024-03-05 09:30:00'), (2, KHALI);");
        ok(e, "DAALO MEIN t (id) MAAN (3);");
    }
    Engine again(dir.str());
    REQUIRE(q(again, "DIKHAO id, at SE t KRAM id;") == "1|2024-03-05 09:30:00;2|KHALI;3|2001-02-03 04:05:06");
    REQUIRE(again.catalog().get("t").columns[1].typeName == "DATETIME");
    // the row codec on its own: DATETIME is stored like text, so an old file of other types is untouched
    auto payload = encodeRow({Value(std::string("2024-03-05 09:30:00"))}, {"DATETIME"});
    REQUIRE(decodeRow(payload, {"DATETIME"})[0].data == Value(std::string("2024-03-05 09:30:00")).data);
}

// ----------------------------------------------------------------- CTAS / LIKE ----

TEST_CASE("CREATE TABLE ... AS SELECT builds the table from the query", "[sql2][ctas]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE s (id INT MUKHYA KUNJI, naam VARCHAR(10) ZAROORI, cgpa FLOAT, joined DATETIME, ok BOOL, d DATE);");
    ok(e, "DAALO MEIN s MAAN (1, 'Ravi', 8.5, '2024-01-01 10:00:00', SACH, '2024-01-01'), "
          "(2, 'Priya', 9.5, KHALI, JHOOTH, KHALI);");
    ok(e, "BANAO TABLE c1 KAHO DIKHAO id, naam, cgpa, joined, ok, d, cgpa * 2 KAHO dbl, NOW() KAHO t0, id + 1 KAHO nx SE s;");
    REQUIRE(q(e, "BATAO c1;") == "id|INT|;naam|TEXT|;cgpa|FLOAT|;joined|DATETIME|;ok|BOOL|;d|DATE|;dbl|FLOAT|;t0|DATETIME|;nx|INT|");
}

TEST_CASE("CTAS copies rows, not constraints, and accepts AS, joins, groups and set operations", "[sql2][ctas]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE s (id INT MUKHYA KUNJI, naam TEXT ZAROORI, city TEXT);");
    ok(e, "DAALO MEIN s MAAN (1, 'Ravi', 'Delhi'), (2, 'Priya', 'Pune'), (3, 'Aman', 'Delhi');");
    ok(e, "BANAO TABLE c1 KAHO DIKHAO * SE s JAHAN id > 1;");
    REQUIRE(q(e, "DIKHAO * SE c1 KRAM id;") == "2|Priya|Pune;3|Aman|Delhi");
    REQUIRE(q(e, "BATAO c1;") == "id|INT|;naam|TEXT|;city|TEXT|");  // no MUKHYA KUNJI / ZAROORI
    ok(e, "DAALO MEIN c1 MAAN (2, KHALI, KHALI);");                  // so a duplicate id and a KHALI naam are fine
    ok(e, "BANAO TABLE c2 AS DIKHAO city, GINO(*) KAHO n SE s SAMOOH city;");
    REQUIRE(q(e, "DIKHAO * SE c2 KRAM city;") == "Delhi|2;Pune|1");
    ok(e, "BANAO TABLE c3 KAHO DIKHAO a.naam KAHO a_naam, b.naam KAHO b_naam SE s a MILAO s b PAR a.city = b.city JAHAN a.id < b.id;");
    REQUIRE(q(e, "DIKHAO * SE c3;") == "Ravi|Aman");
    ok(e, "BANAO TABLE c4 KAHO DIKHAO s.naam SE s;");  // s.naam -> naam
    REQUIRE(q(e, "BATAO c4;") == "naam|TEXT|");
    ok(e, "BANAO TABLE c5 KAHO DIKHAO naam SE s JAHAN id = 1 SANYUKT DIKHAO naam SE s JAHAN id = 2;");
    REQUIRE(q(e, "DIKHAO * SE c5 KRAM naam;") == "Priya;Ravi");
    ok(e, "BANAO TABLE c6 KAHO DIKHAO id SE s JAHAN id > 100;");  // an empty result still makes the table
    REQUIRE(q(e, "DIKHAO GINO(*) SE c6;") == "0");
    ok(e, "BANAO TABLE c7 KAHO DIKHAO 1 + 1 KAHO two, 'x' KAHO ch;");  // no SE
    REQUIRE(q(e, "DIKHAO * SE c7;") == "2|x");
    ok(e, "BANAO TABLE c8 KAHO DIKHAO 7 / 2 KAHO half, 6 / 2 KAHO whole;");
    REQUIRE(q(e, "BATAO c8;") == "half|FLOAT|;whole|INT|");
}

TEST_CASE("CTAS refuses unusable column names and an existing table", "[sql2][ctas]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE s (id INT, naam TEXT);");
    ok(e, "DAALO MEIN s MAAN (1, 'a');");
    REQUIRE_THAT(errorOf(e, "BANAO TABLE c KAHO DIKHAO GINO(*) SE s;"), ContainsSubstring("KAHO se alias do"));
    REQUIRE_THAT(errorOf(e, "BANAO TABLE c KAHO DIKHAO id + 1 SE s;"), ContainsSubstring("KAHO se alias do"));
    REQUIRE(errorOf(e, "BANAO TABLE c KAHO DIKHAO id, id SE s;") == "Query mein column 'id' do baar hai -- KAHO se alag naam do");
    REQUIRE(errorOf(e, "BANAO TABLE c KAHO DIKHAO a.id, b.id SE s a MILAO s b PAR a.id = b.id;") ==
            "Query mein column 'id' do baar hai -- KAHO se alag naam do");
    REQUIRE(errorOf(e, "BANAO TABLE s KAHO DIKHAO id SE s;") == "Table 's' pehle se hai");
    REQUIRE(errorOf(e, "BANAO TABLE c KAHO DIKHAO nope SE s;").find("nope") != std::string::npos);
    REQUIRE(e.catalog().find("c") == nullptr);  // nothing was created by the failures
    REQUIRE(errorOf(e, "BANAO TABLE c KAHO DIKHAO TABLES;").find("DIKHAO") != std::string::npos);
    REQUIRE(e.catalog().find("c") == nullptr);
}

TEST_CASE("CTAS table persists", "[sql2][ctas]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO TABLE s (id INT, at DATETIME);");
        ok(e, "DAALO MEIN s MAAN (1, '2024-01-01 01:01:01');");
        ok(e, "BANAO TABLE c KAHO DIKHAO * SE s;");
    }
    Engine again(dir.str());
    REQUIRE(q(again, "DIKHAO * SE c;") == "1|2024-01-01 01:01:01");
}

TEST_CASE("CREATE TABLE ... LIKE copies definitions but not rows", "[sql2][like]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE p (id INT MUKHYA KUNJI);");
    ok(e, "BANAO TABLE t (id INT AUTO_INCREMENT MUKHYA KUNJI, naam VARCHAR(5) ZAROORI ANOKHA, k ENUM('a','b') WARNA 'a', "
          "n INT SHART (n > 0), pid INT SANDARBH p(id) ON DELETE SET NULL, at DATETIME, ANOKHA (n, k));");
    ok(e, "DAALO MEIN p MAAN (1);");
    ok(e, "DAALO MEIN t (naam, n, pid) MAAN ('abc', 5, 1);");
    ok(e, "BANAO TABLE t2 LIKE t;");
    REQUIRE(q(e, "DIKHAO GINO(*) SE t2;") == "0");
    REQUIRE(q(e, "BATAO t2;") ==
            "id|INT|MUKHYA KUNJI ZAROORI AUTO_INCREMENT;naam|TEXT(5)|ZAROORI ANOKHA;k|ENUM('a','b')|WARNA 'a';"
            "n|INT|SHART (n > 0);pid|INT|SANDARBH p(id) ON DELETE SET NULL;at|DATETIME|;(n, k)||ANOKHA");
    // the constraints are live on the copy
    ok(e, "DAALO MEIN t2 (naam, n) MAAN ('x', 1);");
    REQUIRE(q(e, "DIKHAO id, k SE t2;") == "1|a");
    REQUIRE_THAT(errorOf(e, "DAALO MEIN t2 (naam, n) MAAN ('x', 2);"), ContainsSubstring("Duplicate"));
    REQUIRE_THAT(errorOf(e, "DAALO MEIN t2 (naam, n) MAAN ('y', 0);"), ContainsSubstring("SHART"));
    REQUIRE_THAT(errorOf(e, "DAALO MEIN t2 (naam, n, k) MAAN ('y', 3, 'z');"), ContainsSubstring("ENUM"));
    REQUIRE_THAT(errorOf(e, "DAALO MEIN t2 (naam, n, pid) MAAN ('y', 3, 99);"), ContainsSubstring("SANDARBH"));
    REQUIRE(errorOf(e, "BANAO TABLE t2 LIKE t;") == "Table 't2' pehle se hai");
    REQUIRE(errorOf(e, "BANAO TABLE t3 LIKE nope;") == "Table 'nope' exist nahi karta");
    ok(e, "BANAO VIEW v KAHO DIKHAO * SE t;");
    REQUIRE_THAT(errorOf(e, "BANAO TABLE t4 LIKE v;"), ContainsSubstring("VIEW"));
}

// ---------------------------------------------------------- foreign key actions ----

namespace {
void seedFk(Engine& e, const std::string& onDelete, const std::string& onUpdate) {
    ok(e, "BANAO TABLE d (id INT MUKHYA KUNJI, naam TEXT);");
    ok(e, "BANAO TABLE e (id INT MUKHYA KUNJI, did INT SANDARBH d(id) " + onDelete + " " + onUpdate + ", naam TEXT);");
    ok(e, "DAALO MEIN d MAAN (1, 'cs'), (2, 'ee'), (3, 'me');");
    ok(e, "DAALO MEIN e MAAN (10, 1, 'a'), (11, 1, 'b'), (12, 2, 'c'), (13, KHALI, 'd');");
}
}  // namespace

TEST_CASE("ON DELETE CASCADE deletes the children, RESTRICT is the default", "[sql2][fk]") {
    TempDir dir;
    Engine e(dir.str());
    seedFk(e, "ON DELETE CASCADE", "");
    REQUIRE(q(e, "DIKHAO id SE e KRAM id;") == "10;11;12;13");
    REQUIRE(run(e, "MITAO SE d JAHAN id = 1;").message == "1 row(s) mita di");
    REQUIRE(q(e, "DIKHAO id SE e KRAM id;") == "12;13");
    REQUIRE(q(e, "DIKHAO id SE d KRAM id;") == "2;3");
    // ON UPDATE was not given: RESTRICT
    REQUIRE_THAT(errorOf(e, "BADLO d RAKHO id = 20 JAHAN id = 2;"), ContainsSubstring("abhi bhi inhe use karta hai"));
    REQUIRE(q(e, "DIKHAO id SE d KRAM id;") == "2;3");
}

TEST_CASE("plain SANDARBH still refuses (RESTRICT / NO ACTION)", "[sql2][fk]") {
    TempDir dir;
    Engine e(dir.str());
    seedFk(e, "ON DELETE NO ACTION", "ON UPDATE RESTRICT");
    REQUIRE_THAT(errorOf(e, "MITAO SE d JAHAN id = 1;"), ContainsSubstring("abhi bhi inhe use karta hai"));
    REQUIRE_THAT(errorOf(e, "BADLO d RAKHO id = 9 JAHAN id = 1;"), ContainsSubstring("abhi bhi inhe use karta hai"));
    ok(e, "MITAO SE d JAHAN id = 3;");  // nobody points at 3
    REQUIRE(q(e, "DIKHAO id SE d KRAM id;") == "1;2");
}

TEST_CASE("ON UPDATE CASCADE follows the new parent value", "[sql2][fk]") {
    TempDir dir;
    Engine e(dir.str());
    seedFk(e, "", "ON UPDATE CASCADE");
    ok(e, "BADLO d RAKHO id = id + 100;");
    REQUIRE(q(e, "DIKHAO did SE e KRAM id;") == "101;101;102;KHALI");
    REQUIRE(q(e, "DIKHAO id SE d KRAM id;") == "101;102;103");
    REQUIRE_THAT(errorOf(e, "MITAO SE d JAHAN id = 101;"), ContainsSubstring("abhi bhi inhe use karta hai"));  // RESTRICT on delete
}

TEST_CASE("SET NULL on delete and on update", "[sql2][fk]") {
    TempDir dir;
    Engine e(dir.str());
    seedFk(e, "ON DELETE SET NULL", "ON UPDATE SET NULL");
    ok(e, "MITAO SE d JAHAN id = 1;");
    REQUIRE(q(e, "DIKHAO id, did SE e KRAM id;") == "10|KHALI;11|KHALI;12|2;13|KHALI");
    ok(e, "BADLO d RAKHO id = 50 JAHAN id = 2;");
    REQUIRE(q(e, "DIKHAO id, did SE e KRAM id;") == "10|KHALI;11|KHALI;12|KHALI;13|KHALI");
}

TEST_CASE("SET NULL needs a nullable child column", "[sql2][fk]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE d (id INT MUKHYA KUNJI);");
    REQUIRE_THAT(errorOf(e, "BANAO TABLE e (did INT ZAROORI SANDARBH d(id) ON DELETE SET NULL);"), ContainsSubstring("SET NULL"));
    REQUIRE_THAT(errorOf(e, "BANAO TABLE e (did INT MUKHYA KUNJI SANDARBH d(id) ON UPDATE SET NULL);"), ContainsSubstring("SET NULL"));
    REQUIRE(e.catalog().find("e") == nullptr);
    ok(e, "BANAO TABLE e (did INT SANDARBH d(id) ON DELETE SET KHALI);");  // KHALI spelling
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE e MODIFY did INT ZAROORI;"), ContainsSubstring("SET NULL"));
}

TEST_CASE("cascades go through several tables and a self-reference", "[sql2][fk]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE a (id INT MUKHYA KUNJI);");
    ok(e, "BANAO TABLE b (id INT MUKHYA KUNJI, aid INT SANDARBH a(id) ON DELETE CASCADE ON UPDATE CASCADE);");
    ok(e, "BANAO TABLE c (id INT MUKHYA KUNJI, bid INT SANDARBH b(id) ON DELETE CASCADE ON UPDATE CASCADE);");
    ok(e, "DAALO MEIN a MAAN (1), (2);");
    ok(e, "DAALO MEIN b MAAN (10, 1), (11, 2);");
    ok(e, "DAALO MEIN c MAAN (100, 10), (101, 10), (102, 11);");
    ok(e, "BADLO b RAKHO id = 20 JAHAN id = 10;");  // c follows
    REQUIRE(q(e, "DIKHAO id, bid SE c KRAM id;") == "100|20;101|20;102|11");
    ok(e, "MITAO SE a JAHAN id = 1;");  // b (10 -> now 20) goes, then c
    REQUIRE(q(e, "DIKHAO id SE b;") == "11");
    REQUIRE(q(e, "DIKHAO id SE c;") == "102");

    ok(e, "BANAO TABLE n (id INT MUKHYA KUNJI, up INT SANDARBH n(id) ON DELETE CASCADE);");
    ok(e, "DAALO MEIN n MAAN (1, KHALI), (2, 1), (3, 2), (4, 3), (5, KHALI), (6, 5);");
    ok(e, "MITAO SE n JAHAN id = 1;");
    REQUIRE(q(e, "DIKHAO id SE n KRAM id;") == "5;6");
}

TEST_CASE("a failing cascade changes nothing", "[sql2][fk]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE d (id INT MUKHYA KUNJI);");
    ok(e, "BANAO TABLE e (id INT MUKHYA KUNJI, did INT SANDARBH d(id) ON DELETE CASCADE);");
    ok(e, "BANAO TABLE g (id INT MUKHYA KUNJI, eid INT SANDARBH e(id));");  // RESTRICT below the cascade
    ok(e, "DAALO MEIN d MAAN (1), (2);");
    ok(e, "DAALO MEIN e MAAN (10, 1), (11, 2);");
    ok(e, "DAALO MEIN g MAAN (100, 11);");
    REQUIRE_THAT(errorOf(e, "MITAO SE d;"), ContainsSubstring("abhi bhi inhe use karta hai"));
    REQUIRE(q(e, "DIKHAO id SE d KRAM id;") == "1;2");  // the d row 1 (whose e row is free) is still there
    REQUIRE(q(e, "DIKHAO id SE e KRAM id;") == "10;11");
    // SET NULL that breaks the child's own SHART is refused as a whole
    ok(e, "BANAO TABLE d2 (id INT MUKHYA KUNJI);");
    ok(e, "BANAO TABLE e2 (id INT, did INT SANDARBH d2(id) ON DELETE SET NULL SHART (did > 0 AUR did HAI NAHI KHALI AUR did < 100 YA did HAI KHALI));");
    ok(e, "DAALO MEIN d2 MAAN (1);");
    ok(e, "DAALO MEIN e2 MAAN (1, 1);");
    ok(e, "MITAO SE d2;");  // the CHECK is KHALI-tolerant, so this works
    REQUIRE(q(e, "DIKHAO did SE e2;") == "KHALI");
}

TEST_CASE("named foreign keys: ADD, DROP, auto names", "[sql2][fk]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE d (id INT MUKHYA KUNJI);");
    ok(e, "BANAO TABLE e (id INT, did INT SANDARBH d(id), did2 INT SANDARBH d(id));");
    REQUIRE(e.catalog().get("e").columns[1].fkName == "e_ibfk_1");
    REQUIRE(e.catalog().get("e").columns[2].fkName == "e_ibfk_2");
    ok(e, "SUDHARO TABLE e HATAO FOREIGN KEY e_ibfk_1;");
    REQUIRE(!e.catalog().get("e").columns[1].refTable.has_value());
    REQUIRE(e.catalog().get("e").columns[2].fkName == "e_ibfk_2");  // names are not renumbered
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE e HATAO FOREIGN KEY e_ibfk_1;"), ContainsSubstring("nahi hai"));
    ok(e, "DAALO MEIN d MAAN (1);");
    ok(e, "DAALO MEIN e (id, did) MAAN (1, 999);");  // no longer checked
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE e JODO CONSTRAINT fk_d FOREIGN KEY (did) SANDARBH d(id);"),
                 ContainsSubstring("mein nahi mila"));  // existing data is checked
    ok(e, "BADLO e RAKHO did = 1;");
    ok(e, "SUDHARO TABLE e JODO CONSTRAINT fk_d FOREIGN KEY (did) SANDARBH d(id) ON DELETE CASCADE ON UPDATE SET NULL;");
    REQUIRE(e.catalog().get("e").columns[1].fkName == "fk_d");
    REQUIRE(e.catalog().get("e").columns[1].onDelete == "CASCADE");
    REQUIRE(e.catalog().get("e").columns[1].onUpdate == "SET NULL");
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE e JODO FOREIGN KEY (did) SANDARBH d(id);"), ContainsSubstring("pehle se SANDARBH"));
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE e JODO CONSTRAINT fk_d FOREIGN KEY (id) SANDARBH d(id);"), ContainsSubstring("fk_d"));
    ok(e, "SUDHARO TABLE e JODO FOREIGN KEY (id) SANDARBH d(id);");  // unnamed: gets e_ibfk_<n>
    REQUIRE(e.catalog().get("e").columns[0].fkName == "e_ibfk_1");
    ok(e, "SUDHARO TABLE e HATAO CONSTRAINT fk_d;");
    REQUIRE(!e.catalog().get("e").columns[1].refTable.has_value());
    REQUIRE(errorOf(e, "SUDHARO TABLE e JODO FOREIGN KEY (nope) SANDARBH d(id);").find("nope") != std::string::npos);
    REQUIRE(errorOf(e, "SUDHARO TABLE e JODO FOREIGN KEY (did) SANDARBH nope(id);").find("nope") != std::string::npos);
}

TEST_CASE("table-level FOREIGN KEY in CREATE TABLE, names unique per database", "[sql2][fk]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE d (id INT MUKHYA KUNJI);");
    ok(e, "BANAO TABLE e (id INT, did INT, CONSTRAINT my_fk FOREIGN KEY (did) REFERENCES d(id) ON DELETE CASCADE);");
    REQUIRE(e.catalog().get("e").columns[1].fkName == "my_fk");
    REQUIRE(e.catalog().get("e").columns[1].onDelete == "CASCADE");
    ok(e, "BANAO TABLE f (id INT, did INT, FOREIGN KEY (did) SANDARBH d(id));");
    REQUIRE(e.catalog().get("f").columns[1].fkName == "f_ibfk_1");
    REQUIRE_THAT(errorOf(e, "BANAO TABLE g (did INT, CONSTRAINT my_fk FOREIGN KEY (did) SANDARBH d(id));"),
                 ContainsSubstring("my_fk"));
    REQUIRE(e.catalog().find("g") == nullptr);
    REQUIRE(q(e, "BATAO e;") == "id|INT|;did|INT|SANDARBH d(id) [my_fk] ON DELETE CASCADE");
}

TEST_CASE("foreign key actions and names survive a reload", "[sql2][fk]") {
    TempDir dir;
    {
        Engine e(dir.str());
        seedFk(e, "ON DELETE CASCADE", "ON UPDATE SET NULL");
    }
    Engine again(dir.str());
    const Column& col = again.catalog().get("e").columns[1];
    REQUIRE(col.onDelete == "CASCADE");
    REQUIRE(col.onUpdate == "SET NULL");
    REQUIRE(col.fkName == "e_ibfk_1");
    ok(again, "MITAO SE d JAHAN id = 1;");
    REQUIRE(q(again, "DIKHAO id SE e KRAM id;") == "12;13");
}

TEST_CASE("a catalog from before foreign key names gets auto names, and saves back unchanged", "[sql2][fk]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO TABLE d (id INT MUKHYA KUNJI);");
        ok(e, "BANAO TABLE e (a INT SANDARBH d(id), b INT SANDARBH d(id));");
    }
    std::string path = (std::filesystem::path(dir.str()) / "main" / "catalog.json").string();
    std::string before = meradb_test::readText(path);
    REQUIRE(before.find("fk_name") == std::string::npos);  // auto names are not written
    Engine again(dir.str());
    REQUIRE(again.catalog().get("e").columns[0].fkName == "e_ibfk_1");
    REQUIRE(again.catalog().get("e").columns[1].fkName == "e_ibfk_2");
    again.catalog().save();
    REQUIRE(meradb_test::readText(path) == before);
    // dropping the first leaves the second named e_ibfk_2: that one IS written, so the name survives a reload
    ok(again, "SUDHARO TABLE e HATAO FOREIGN KEY e_ibfk_1;");
    Engine third(dir.str());
    REQUIRE(third.catalog().get("e").columns[1].fkName == "e_ibfk_2");
}

// ------------------------------------------------------------------- ALTER ----

TEST_CASE("ALTER ... JODO ... AFTER / FIRST puts the column where asked", "[sql2][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (a INT, b INT, c INT);");
    ok(e, "DAALO MEIN t MAAN (1, 2, 3);");
    ok(e, "SUDHARO TABLE t JODO x INT WARNA 9 AFTER a;");
    REQUIRE(q(e, "DIKHAO * SE t;") == "1|9|2|3");
    ok(e, "SUDHARO TABLE t JODO f INT FIRST;");
    REQUIRE(q(e, "DIKHAO * SE t;") == "KHALI|1|9|2|3");
    ok(e, "SUDHARO TABLE t JODO z TEXT AFTER c;");
    REQUIRE(q(e, "DIKHAO * SE t;") == "KHALI|1|9|2|3|KHALI");
    REQUIRE(q(e, "BATAO t;") == "f|INT|;a|INT|;x|INT|WARNA 9;b|INT|;c|INT|;z|TEXT|");
    ok(e, "DAALO MEIN t MAAN (0, 5, 5, 5, 5, 'q');");  // positional insert uses the new order
    REQUIRE(q(e, "DIKHAO z SE t JAHAN f = 0;") == "q");
    REQUIRE(errorOf(e, "SUDHARO TABLE t JODO y INT AFTER nope;").find("nope") != std::string::npos);
    REQUIRE(e.catalog().get("t").columns.size() == 6);
    ok(e, "SUDHARO TABLE t JODO COLUMN w INT AFTER f;");  // optional COLUMN keyword
    REQUIRE(e.catalog().get("t").columns[1].name == "w");
}

TEST_CASE("ADD COLUMN keeps composite constraints", "[sql2][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (a INT, b INT, ANOKHA (a, b));");
    ok(e, "SUDHARO TABLE t JODO c INT FIRST;");
    ok(e, "DAALO MEIN t (a, b) MAAN (1, 1);");
    REQUIRE_THAT(errorOf(e, "DAALO MEIN t (a, b) MAAN (1, 1);"), ContainsSubstring("Duplicate"));
}

TEST_CASE("MODIFY retypes a column and converts the data", "[sql2][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, n TEXT, f FLOAT, d TEXT);");
    ok(e, "DAALO MEIN t MAAN (1, '12', 1.5, '2024-03-05'), (2, '7', 2.0, KHALI);");
    ok(e, "SUDHARO TABLE t MODIFY n INT;");
    REQUIRE(q(e, "DIKHAO n + 1 SE t KRAM id;") == "13;8");
    ok(e, "SUDHARO TABLE t MODIFY d DATE;");
    REQUIRE(q(e, "DIKHAO YEAR(d) SE t KRAM id;") == "2024;KHALI");
    ok(e, "SUDHARO TABLE t MODIFY d DATETIME;");
    REQUIRE(q(e, "DIKHAO d SE t KRAM id;") == "2024-03-05 00:00:00;KHALI");
    ok(e, "SUDHARO TABLE t MODIFY id TEXT;");  // the key stays a key
    REQUIRE(q(e, "DIKHAO id SE t KRAM id;") == "1;2");
    REQUIRE_THAT(errorOf(e, "DAALO MEIN t (id) MAAN ('1');"), ContainsSubstring("Duplicate"));
    ok(e, "SUDHARO TABLE t MODIFY n TEXT;");
    REQUIRE(q(e, "DIKHAO n SE t KRAM id;") == "12;7");
    ok(e, "SUDHARO TABLE t MODIFY COLUMN f INT;");  // 1.5 rounds, like CAST
    REQUIRE(q(e, "DIKHAO f SE t KRAM id;") == "2;2");
    REQUIRE(e.catalog().get("t").columns[2].typeName == "INT");
}

TEST_CASE("MODIFY / CHANGE refuse data that does not fit, changing nothing", "[sql2][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT, n TEXT, k TEXT);");
    ok(e, "DAALO MEIN t MAAN (1, '12', 'a'), (2, 'abc', 'a'), (3, KHALI, KHALI);");
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE t MODIFY n INT;"), ContainsSubstring("'abc'"));
    REQUIRE(e.catalog().get("t").columns[1].typeName == "TEXT");
    REQUIRE(q(e, "DIKHAO n SE t KRAM id;") == "12;abc;KHALI");
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE t MODIFY k TEXT ZAROORI;"), ContainsSubstring("ZAROORI"));
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE t MODIFY k TEXT ANOKHA;"), ContainsSubstring("Duplicate"));
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE t MODIFY k VARCHAR(0);"), ContainsSubstring("zyada se zyada"));
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE t MODIFY k ENUM('b');"), ContainsSubstring("ENUM"));
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE t MODIFY k DATE;"), ContainsSubstring("DATE"));
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE t MODIFY nope INT;"), ContainsSubstring("nope"));
    REQUIRE(q(e, "DIKHAO k SE t KRAM id;") == "a;a;KHALI");
    REQUIRE(q(e, "BATAO t;") == "id|INT|;n|TEXT|;k|TEXT|");
}

TEST_CASE("CHANGE renames and retypes; constraints and references follow", "[sql2][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE p (pid INT MUKHYA KUNJI, tag TEXT);");
    ok(e, "BANAO TABLE c (id INT, p INT SANDARBH p(pid), a INT, b INT, ANOKHA (a, b));");
    ok(e, "DAALO MEIN p MAAN (1, 'x');");
    ok(e, "DAALO MEIN c MAAN (1, 1, 5, 6);");
    ok(e, "SUDHARO TABLE p CHANGE pid pid2 INT MUKHYA KUNJI;");  // the FK in c follows
    REQUIRE(e.catalog().get("c").columns[1].refColumn.value() == "pid2");
    ok(e, "DAALO MEIN c MAAN (2, 1, 7, 8);");
    REQUIRE_THAT(errorOf(e, "DAALO MEIN c MAAN (3, 2, 9, 9);"), ContainsSubstring("SANDARBH"));
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE p MODIFY pid2 TEXT;"), ContainsSubstring("c.p"));  // referenced: type is fixed
    ok(e, "SUDHARO TABLE c CHANGE a aa INT;");                                                    // composite group follows
    REQUIRE(e.catalog().get("c").compositeUnique[0] == std::vector<std::string>{"aa", "b"});
    REQUIRE_THAT(errorOf(e, "DAALO MEIN c MAAN (3, 1, 5, 6);"), ContainsSubstring("Duplicate"));
}

TEST_CASE("CHANGE refuses a rename that a SHART uses and a clashing name", "[sql2][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (a INT, b INT SHART (b > a));");
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE t CHANGE a aa INT;"), ContainsSubstring("SHART"));
    REQUIRE_THAT(errorOf(e, "SUDHARO TABLE t CHANGE a b INT;"), ContainsSubstring("pehle se hai"));
    ok(e, "SUDHARO TABLE t MODIFY a FLOAT;");  // same name is fine
    ok(e, "SUDHARO TABLE t CHANGE COLUMN a a FLOAT ZAROORI;");
    REQUIRE(e.catalog().get("t").columns[0].notNull);
}

TEST_CASE("CHANGE keeps data, the key and works for DATETIME / ENUM / AUTO_INCREMENT", "[sql2][alter]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, s TEXT);");
    ok(e, "DAALO MEIN t MAAN (1, 'open'), (2, 'closed');");
    ok(e, "SUDHARO TABLE t MODIFY s ENUM('open', 'closed', 'late');");
    REQUIRE(q(e, "BATAO t;") == "id|INT|MUKHYA KUNJI;s|ENUM('open','closed','late')|");
    ok(e, "SUDHARO TABLE t MODIFY id INT AUTO_INCREMENT;");
    ok(e, "DAALO MEIN t (s) MAAN ('late');");
    REQUIRE(q(e, "DIKHAO id, s SE t KRAM id;") == "1|open;2|closed;3|late");
    ok(e, "SUDHARO TABLE t CHANGE s state TEXT;");  // back to plain text
    ok(e, "DAALO MEIN t (state) MAAN ('anything');");
    REQUIRE(q(e, "DIKHAO state SE t JAHAN id = 4;") == "anything");
    // the changed table reloads
    Engine again(dir.str());
    REQUIRE(q(again, "DIKHAO COUNT(*) SE t;") == "4");
}

// --------------------------------------------------------------------- REPLACE ----

TEST_CASE("REPLACE INTO replaces rows that collide on a key", "[sql2][replace]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, email TEXT ANOKHA, naam TEXT);");
    ok(e, "DAALO MEIN t MAAN (1, 'a@x', 'one'), (2, 'b@x', 'two');");
    Result r = run(e, "REPLACE INTO t MAAN (3, 'c@x', 'three');");
    REQUIRE(r.message == "1 row(s) affected (1 daali, 0 purani row(s) replace hui)");  // plain insert: 1
    r = run(e, "REPLACE INTO t MAAN (1, 'a2@x', 'uno');");
    REQUIRE(r.message == "2 row(s) affected (1 daali, 1 purani row(s) replace hui)");  // replace: 2
    REQUIRE(q(e, "DIKHAO id, email, naam SE t KRAM id;") == "1|a2@x|uno;2|b@x|two;3|c@x|three");
    // collides on the OTHER unique column: that row goes
    r = run(e, "BADAL DAALO MEIN t MAAN (9, 'b@x', 'nine');");
    REQUIRE(r.message == "2 row(s) affected (1 daali, 1 purani row(s) replace hui)");
}

TEST_CASE("REPLACE with a column list, several rows, one that hits two rows, and INSERT ... SELECT", "[sql2][replace]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, email TEXT ANOKHA, naam TEXT WARNA 'none');");
    ok(e, "DAALO MEIN t MAAN (1, 'a@x', 'one'), (2, 'b@x', 'two'), (3, 'c@x', 'three');");
    // id 1 and email b@x belong to different rows: both are replaced by the one new row
    Result r = run(e, "REPLACE INTO t (id, email) MAAN (1, 'b@x');");
    REQUIRE(r.message == "3 row(s) affected (1 daali, 2 purani row(s) replace hui)");
    REQUIRE(q(e, "DIKHAO id, email, naam SE t KRAM id;") == "1|b@x|none;3|c@x|three");
    // several rows, one of them replacing an earlier row of the same statement
    r = run(e, "REPLACE INTO t (id, email) MAAN (5, 'p@x'), (6, 'q@x'), (5, 'r@x');");
}

TEST_CASE("REPLACE obeys foreign keys", "[sql2][replace]") {
    TempDir dir;
    Engine e(dir.str());
    seedFk(e, "", "");
    REQUIRE_THAT(errorOf(e, "REPLACE INTO d MAAN (1, 'new');"), ContainsSubstring("abhi bhi inhe use karta hai"));
    REQUIRE(q(e, "DIKHAO naam SE d JAHAN id = 1;") == "cs");  // nothing changed
    REQUIRE_THAT(errorOf(e, "REPLACE INTO e MAAN (10, 99, 'x');"), ContainsSubstring("SANDARBH"));
    REQUIRE(q(e, "DIKHAO naam SE e JAHAN id = 10;") == "a");
    ok(e, "REPLACE INTO e MAAN (10, 2, 'moved');");  // child side: fine
    REQUIRE(q(e, "DIKHAO did, naam SE e JAHAN id = 10;") == "2|moved");

    TempDir dir2;
    Engine e2(dir2.str());
    seedFk(e2, "ON DELETE CASCADE", "");
    ok(e2, "REPLACE INTO d MAAN (1, 'new');");  // the children of the replaced row are cascaded away
    REQUIRE(q(e2, "DIKHAO id SE e KRAM id;") == "12;13");
    REQUIRE(q(e2, "DIKHAO naam SE d JAHAN id = 1;") == "new");
}

TEST_CASE("REPLACE fires delete and insert triggers and takes AUTO_INCREMENT numbers", "[sql2][replace]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT AUTO_INCREMENT MUKHYA KUNJI, naam TEXT);");
    ok(e, "BANAO TABLE log (msg TEXT);");
    ok(e, "BANAO TRIGGER tdel BAAD MITAO PAR t SHURU DAALO MEIN log MAAN ('del ' || PURANA.naam); KHATAM");
    ok(e, "BANAO TRIGGER tins BAAD DAALO PAR t SHURU DAALO MEIN log MAAN ('ins ' || NAYA.naam); KHATAM");
    ok(e, "DAALO MEIN t (naam) MAAN ('a');");
    ok(e, "REPLACE INTO t MAAN (1, 'b');");
    REQUIRE(q(e, "DIKHAO msg SE log;") == "ins a;del a;ins b");
    ok(e, "REPLACE INTO t (naam) MAAN ('c');");  // id omitted: 2
    REQUIRE(q(e, "DIKHAO id, naam SE t KRAM id;") == "1|b;2|c");
}

TEST_CASE("REPLACE needs MITAO as well as DAALO for a restricted user", "[sql2][replace]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin(inst);
    ok(admin, "BANAO TABLE t (id INT MUKHYA KUNJI);");
    ok(admin, "BANAO USER u GUPT 'pw';");
    ok(admin, "ADHIKAR DO DAALO PAR t KO u;");
    Engine user(inst);
    user.user = "u";
    REQUIRE_THAT(errorOf(user, "REPLACE INTO t MAAN (1);"), ContainsSubstring("MITAO"));
    ok(admin, "ADHIKAR DO MITAO PAR t KO u;");
    ok(user, "REPLACE INTO t MAAN (1);");
}

TEST_CASE("REPLACE parse details", "[sql2][replace]") {
    auto stmts = parseScript("REPLACE INTO t (a) MAAN (1);");
    auto* ins = dynamic_cast<ast::Insert*>(stmts[0].get());
    REQUIRE(ins != nullptr);
    REQUIRE(ins->replace);
    REQUIRE(ins->table == "t");
    stmts = parseScript("badal daalo mein t DIKHAO 1 SE x;");
    REQUIRE(dynamic_cast<ast::Insert*>(stmts[0].get())->replace);
    REQUIRE(dynamic_cast<ast::Insert*>(parseScript("DAALO MEIN t MAAN (1);")[0].get())->replace == false);
    REQUIRE_THROWS_AS(parseScript("REPLACE INTO t MAAN (1) TAKRAAV PAR BADLO a = 1;"), ParseError);
    REQUIRE_THROWS_AS(parseScript("REPLACE t MAAN (1);"), ParseError);
    // REPLACE stays usable as a column name and as the scalar function
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE r (replace TEXT);");
    ok(e, "DAALO MEIN r MAAN ('abc');");
    REQUIRE(q(e, "DIKHAO REPLACE(replace, 'b', 'X') SE r;") == "aXc");
}

// ----------------------------------------------------- small gaps: no SE, LIMIT, / ----

TEST_CASE("DIKHAO without SE returns exactly one row", "[sql2][noFrom]") {
    TempDir dir;
    Engine e(dir.str());
    Result r = run(e, "DIKHAO NOW();");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(formatValue(r.rows[0][0]).size() == 19);
    REQUIRE(q(e, "DIKHAO 1+1, UPPER('a');") == "2|A");
    REQUIRE(run(e, "DIKHAO 1+1, UPPER('a');").columns == std::vector<std::string>{"1 + 1", "UPPER('a')"});
    REQUIRE(q(e, "DIKHAO 1 KAHO x;") == "1");
    REQUIRE(q(e, "DIKHAO GINO(*);") == "1");
    REQUIRE(q(e, "DIKHAO 5 JAHAN 1 = 2;") == "");
    REQUIRE(q(e, "DIKHAO 5 JAHAN 1 = 1;") == "5");
    REQUIRE(q(e, "DIKHAO 'a' SANYUKT DIKHAO 'b';") == "a;b");
    REQUIRE(q(e, "DIKHAO (DIKHAO 41 + 1);") == "42");
    REQUIRE(q(e, "DIKHAO 1 LIMIT 5;") == "1");
    REQUIRE(q(e, "DIKHAO 1 SIRF 0;") == "");
    REQUIRE(q(e, "DIKHAO ALAG 1;") == "1");
    ok(e, "BANAO TABLE t (a INT);");
    ok(e, "DAALO MEIN t (a) DIKHAO 7;");
    ok(e, "DAALO MEIN t (a) DIKHAO 8 + 1;");
    REQUIRE(q(e, "DIKHAO a SE t KRAM a;") == "7;9");
    ok(e, "BANAO VIEW v KAHO DIKHAO 3 KAHO three;");
    REQUIRE(q(e, "DIKHAO three SE v;") == "3");
    REQUIRE(q(e, "SAMJHAO DIKHAO 1;").find("one constant row") != std::string::npos);
    REQUIRE_THAT(errorOf(e, "DIKHAO naam;"), ContainsSubstring("Column 'naam' ke liye table chahiye"));
    REQUIRE_THAT(errorOf(e, "DIKHAO 1 JAHAN naam = 1;"), ContainsSubstring("table chahiye"));
    REQUIRE_THAT(errorOf(e, "DIKHAO 1 KRAM zz;"), ContainsSubstring("table chahiye"));
}

TEST_CASE("DIKHAO without SE still parses the older errors", "[sql2][noFrom]") {
    REQUIRE_THROWS_WITH(parseScript("DIKHAO 'abc' 3.5 SE t;"), ContainsSubstring("'SE' expected tha"));
    REQUIRE_THROWS_WITH(parseScript("DIKHAO a b SE t;"), ContainsSubstring("'SE' expected tha"));
}

TEST_CASE("LIMIT is an alias of SIRF, and still a usable name", "[sql2][limit]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT, limit INT);");
    ok(e, "DAALO MEIN t MAAN (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);");
    REQUIRE(q(e, "DIKHAO id SE t KRAM id LIMIT 2;") == "1;2");
    REQUIRE(q(e, "DIKHAO id SE t KRAM id limit 2 OFFSET 1;") == "2;3");
    REQUIRE(q(e, "DIKHAO id SE t KRAM id LIMIT 1, 2;") == "2;3");  // LIMIT m, n
    REQUIRE(q(e, "DIKHAO id SE t KRAM id SIRF 1 OFFSET 4;") == "5");
    REQUIRE(q(e, "DIKHAO limit SE t JAHAN limit > 30 KRAM limit;") == "40;50");  // a column called limit
    REQUIRE(q(e, "DIKHAO id SE t JAHAN limit = 20;") == "2");
    REQUIRE(q(e, "DIKHAO id SE t LIMIT 2;") == "1;2");        // not taken as the table's alias
    REQUIRE(q(e, "DIKHAO t.id SE t t LIMIT 1;") == "1");      // ... and an explicit alias still works
    REQUIRE(q(e, "DIKHAO a.id SE t a MILAO t b PAR a.id = b.id KRAM a.id LIMIT 2;") == "1;2");
    REQUIRE(q(e, "DIKHAO id SE (DIKHAO id SE t KRAM id LIMIT 3) x KRAM id ULTA LIMIT 2;") == "3;2");
    REQUIRE(q(e, "DIKHAO id SE t JAHAN id MEIN (DIKHAO id SE t KRAM id LIMIT 2) KRAM id;") == "1;2");
    REQUIRE(q(e, "DIKHAO id SE t KRAM id LIMIT 2 SANYUKT DIKHAO id SE t KRAM id ULTA LIMIT 1;") == "1;2;5");
    ok(e, "BANAO TABLE lim KAHO DIKHAO id SE t KRAM id LIMIT 2;");
    REQUIRE(q(e, "DIKHAO COUNT(*) SE lim;") == "2");
    REQUIRE(q(e, "SAMJHAO DIKHAO id SE t LIMIT 2 OFFSET 1;").find("LIMIT  SIRF 2 OFFSET 1") != std::string::npos);
}

TEST_CASE("slash divides exactly and DIV truncates", "[sql2][divide]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE(q(e, "DIKHAO 7 / 2, 6 / 2, -7 / 2, 1 / 4;") == "3.5|3|-3.5|0.25");
    REQUIRE(q(e, "DIKHAO 7 DIV 2, -7 DIV 2, 6 DIV 2, 7.9 DIV 2, 7 DIV 2.0;") == "3|-3|3|3|3");
    REQUIRE(q(e, "DIKHAO 10 / 4 * 2, 10 DIV 4 * 2, 2 + 7 DIV 2;") == "5.0|4|5");
    Result r = run(e, "DIKHAO 6 / 2, 7 / 2;");
    REQUIRE(std::holds_alternative<int64_t>(r.rows[0][0].data));  // an exact quotient stays INT
    REQUIRE(std::holds_alternative<double>(r.rows[0][1].data));
    REQUIRE_THAT(errorOf(e, "DIKHAO 1 / 0;"), ContainsSubstring("Zero se divide"));
    REQUIRE_THAT(errorOf(e, "DIKHAO 1 DIV 0;"), ContainsSubstring("Zero se divide"));
    REQUIRE(q(e, "DIKHAO 9223372036854775807 / 1, -9223372036854775807 - 1 / 1;") != "");
    // DIV is a soft word: usable as a column name
    ok(e, "BANAO TABLE d (div INT);");
    ok(e, "DAALO MEIN d MAAN (9);");
    REQUIRE(q(e, "DIKHAO div, div DIV 2, div / 2 SE d;") == "9|4|4.5");
    ok(e, "BANAO TABLE h (a INT, b INT SHART (b DIV 2 = 1));");
    REQUIRE_THAT(errorOf(e, "DAALO MEIN h MAAN (1, 5);"), ContainsSubstring("SHART"));
    ok(e, "DAALO MEIN h MAAN (1, 3);");
}
