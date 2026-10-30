// cpp/tests/test_sql_batch1.cpp -- more MySQL-style SQL: scalar functions, CAST, ||, SIRF .. OFFSET,
// MILAO .. USING, EXISTS / ANY / ALL, derived tables, multi-column SAMOOH.
//
// These features are MeraDB-C++ only (the Python engine does not have them), so there is no golden script:
// every expectation is written out here.
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/scalar_funcs.h"
#include "test_util.h"
#include <ctime>

using namespace meradb;
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

// One value computed by a scalar expression (no table needed: a one-row table supplies the row).
std::string one(Engine& e, const std::string& expr) {
    auto r = run(e, "DIKHAO " + expr + " SE one;");
    REQUIRE(r.rows.size() == 1);
    return formatValue(r.rows[0][0]);
}

void seedOne(Engine& e) {
    run(e, "BANAO TABLE one (id INT);");
    run(e, "DAALO MEIN one (id) MAAN (1);");
}

void seedSchool(Engine& e) {
    run(e, "BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT);");
    run(e, "BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, cgpa FLOAT, cid INT, shehar TEXT);");
    run(e, "DAALO MEIN courses (id, title) MAAN (10, 'DBMS'), (20, 'OS'), (30, 'AI');");
    run(e, "DAALO MEIN students (id, naam, cgpa, cid, shehar) MAAN "
           "(1, 'Ravi', 8.4, 10, 'Delhi'), (2, 'Priya', 9.1, 10, 'Pune'), (3, 'Aman', 7.0, 20, 'Delhi'), "
           "(4, 'Zoya', 6.5, KHALI, 'Pune'), (5, 'Neha', 9.1, 20, 'Delhi');");
}

// The error message of a failing statement, without the "[... Galti] " stage prefix ("" if it ran fine).
std::string errorOf(Engine& e, const std::string& sql) {
    std::string error = meradb_test::runLast(e, sql).error;
    size_t close = error.find("Galti] ");
    return close == std::string::npos ? error : error.substr(close + 7);
}

}  // namespace

// ------------------------------------------------------------------ strings ----

TEST_CASE("string functions", "[sql][functions]") {
    TempDir dir;
    Engine e(dir.str());
    seedOne(e);
    REQUIRE(one(e, "UPPER('Ravi')") == "RAVI");
    REQUIRE(one(e, "upper('Ravi')") == "RAVI");  // function names are case-insensitive
    REQUIRE(one(e, "LOWER('RAVI')") == "ravi");
    REQUIRE(one(e, "LENGTH('Ravi')") == "4");
    REQUIRE(one(e, "CHAR_LENGTH('Ravi')") == "4");
    REQUIRE(one(e, "LENGTH('\xE0\xA4\xB0\xE0\xA4\xB5\xE0\xA4\xBF')") == "3");  // characters, not bytes
    REQUIRE(one(e, "TRIM('  hi  ')") == "hi");
    REQUIRE(one(e, "LTRIM('  hi  ')") == "hi  ");
    REQUIRE(one(e, "RTRIM('  hi  ')") == "  hi");
    REQUIRE(one(e, "SUBSTRING('Hello World', 7)") == "World");
    REQUIRE(one(e, "SUBSTRING('Hello World', 1, 5)") == "Hello");
    REQUIRE(one(e, "SUBSTR('Hello World', 7, 3)") == "Wor");
    REQUIRE(one(e, "SUBSTRING('Hello', -3)") == "llo");
    REQUIRE(one(e, "SUBSTRING('Hello', 0)").empty());
    REQUIRE(one(e, "SUBSTRING('Hello', 9)").empty());
    REQUIRE(one(e, "CONCAT('a', 'b', 'c')") == "abc");
    REQUIRE(one(e, "CONCAT('id-', 42, '-', 1.5)") == "id-42-1.5");
    REQUIRE(one(e, "CONCAT('a', KHALI)") == "KHALI");
    REQUIRE(one(e, "REPLACE('banana', 'an', 'AN')") == "bANANa");
    REQUIRE(one(e, "LEFT('Hello', 2)") == "He");
    REQUIRE(one(e, "RIGHT('Hello', 3)") == "llo");
    REQUIRE(one(e, "LEFT('Hi', 10)") == "Hi");
    REQUIRE(one(e, "'ab' || 'cd'") == "abcd");
    REQUIRE(one(e, "'n=' || 5") == "n=5");
    REQUIRE(one(e, "'a' || KHALI") == "KHALI");
}

TEST_CASE("KHALI in gives KHALI out", "[sql][functions]") {
    TempDir dir;
    Engine e(dir.str());
    seedOne(e);
    for (const char* f : {"UPPER(KHALI)", "LOWER(KHALI)", "LENGTH(KHALI)", "TRIM(KHALI)", "SUBSTRING(KHALI, 1)",
                          "REPLACE(KHALI, 'a', 'b')", "LEFT(KHALI, 1)", "ROUND(KHALI)", "FLOOR(KHALI)", "ABS(KHALI)",
                          "SQRT(KHALI)", "POWER(2, KHALI)", "MOD(KHALI, 2)", "YEAR(KHALI)", "DATEDIFF(KHALI, '2020-01-01')",
                          "DATE_FORMAT(KHALI, '%Y')", "CAST(KHALI AS INT)"})
        REQUIRE(one(e, f) == "KHALI");
    REQUIRE(one(e, "NULLIF(KHALI, 1)") == "KHALI");
    REQUIRE(one(e, "NULLIF(5, KHALI)") == "5");
}

TEST_CASE("function arguments come from columns and work in WHERE, ORDER BY, HAVING, UPDATE and INSERT", "[sql][functions]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchool(e);
    REQUIRE(flat(run(e, "DIKHAO UPPER(naam) SE students JAHAN LENGTH(naam) = 4 KRAM UPPER(naam);")) == "AMAN;NEHA;RAVI;ZOYA");
    REQUIRE(flat(run(e, "DIKHAO naam SE students KRAM LENGTH(naam) ULTA, naam SIRF 2;")) == "Priya;Aman");
    REQUIRE(flat(run(e, "DIKHAO shehar, GINO(*) SE students SAMOOH shehar JINKA LENGTH(shehar) = 5 KRAM shehar;")) ==
            "Delhi|3");
    REQUIRE(flat(run(e, "DIKHAO UPPER(shehar), ROUND(AUSAT(cgpa), 1) SE students SAMOOH UPPER(shehar) KRAM UPPER(shehar);")) ==
            "DELHI|8.2;PUNE|7.8");
    run(e, "BADLO students RAKHO naam = UPPER(naam) || '!' JAHAN id = 1;");
    REQUIRE(flat(run(e, "DIKHAO naam SE students JAHAN id = 1;")) == "RAVI!");
    run(e, "DAALO MEIN students (id, naam, cgpa) MAAN (9, LOWER('KIRAN'), ROUND(7.456, 1));");
    REQUIRE(flat(run(e, "DIKHAO naam, cgpa SE students JAHAN id = 9;")) == "kiran|7.5");
    REQUIRE(run(e, "DIKHAO UPPER(naam) SE students SIRF 1;").columns[0] == "UPPER(naam)");
    REQUIRE(run(e, "DIKHAO UPPER(naam) KAHO n SE students SIRF 1;").columns[0] == "n");
}

// ------------------------------------------------------------------ numbers ----

TEST_CASE("numeric functions", "[sql][functions]") {
    TempDir dir;
    Engine e(dir.str());
    seedOne(e);
    REQUIRE(one(e, "ROUND(2.5)") == "3.0");
    REQUIRE(one(e, "ROUND(-2.5)") == "-3.0");
    REQUIRE(one(e, "ROUND(3.14159, 2)") == "3.14");
    REQUIRE(one(e, "ROUND(1234, -2)") == "1200");
    REQUIRE(one(e, "ROUND(7)") == "7");
    REQUIRE(one(e, "FLOOR(2.7)") == "2");
    REQUIRE(one(e, "FLOOR(-2.1)") == "-3");
    REQUIRE(one(e, "CEIL(2.1)") == "3");
    REQUIRE(one(e, "CEILING(-2.9)") == "-2");
    REQUIRE(one(e, "ABS(-5)") == "5");
    REQUIRE(one(e, "ABS(-2.5)") == "2.5");
    REQUIRE(one(e, "MOD(10, 3)") == "1");
    REQUIRE(one(e, "10 % 3") == "1");
    REQUIRE(one(e, "MOD(10.5, 3)") == "1.5");
    REQUIRE(one(e, "MOD(-7, 3)") == "-1");   // MySQL: the sign follows the dividend
    REQUIRE(one(e, "MOD(7, -3)") == "1");
    REQUIRE(one(e, "-7 % 3") == "2");        // the operator keeps the engine's rule
    REQUIRE(one(e, "POWER(2, 10)") == "1024");
    REQUIRE(one(e, "POW(2, 0.5)").substr(0, 6) == "1.4142");
    REQUIRE(one(e, "POWER(2, -1)") == "0.5");
    REQUIRE(one(e, "SQRT(16)") == "4.0");
    REQUIRE(one(e, "SQRT(-1)") == "KHALI");
    REQUIRE(errorOf(e, "DIKHAO MOD(1, 0) SE one;").find("Zero") != std::string::npos);
    REQUIRE(errorOf(e, "DIKHAO ABS('x') SE one;").find("ABS ko number chahiye") == 0);
    REQUIRE(one(e, "IFNULL(KHALI, 7)") == "7");
    REQUIRE(one(e, "IFNULL(3, 7)") == "3");
    REQUIRE(one(e, "NULLIF(3, 3)") == "KHALI");
    REQUIRE(one(e, "NULLIF(3, 4)") == "3");
}

// -------------------------------------------------------------------- dates ----

TEST_CASE("date functions", "[sql][functions]") {
    TempDir dir;
    Engine e(dir.str());
    seedOne(e);
    run(e, "BANAO TABLE ev (id INT, d DATE);");
    run(e, "DAALO MEIN ev (id, d) MAAN (1, '2024-03-15'), (2, '2023-12-31');");
    REQUIRE(flat(run(e, "DIKHAO YEAR(d), MONTH(d), DAY(d) SE ev KRAM id;")) == "2024|3|15;2023|12|31");
    REQUIRE(one(e, "YEAR('2020-02-29')") == "2020");
    REQUIRE(one(e, "DATEDIFF('2024-03-15', '2024-03-01')") == "14");
    REQUIRE(one(e, "DATEDIFF('2024-03-01', '2024-03-15')") == "-14");
    REQUIRE(flat(run(e, "DIKHAO DATEDIFF(a.d, b.d) SE ev a MILAO ev b PAR a.id = 1 AUR b.id = 2;")) == "75");
    REQUIRE(one(e, "DATE_FORMAT('2024-03-05', '%Y-%m-%d')") == "2024-03-05");
    REQUIRE(one(e, "DATE_FORMAT('2024-03-05', '%d/%m/%y')") == "05/03/24");
    REQUIRE(one(e, "DATE_FORMAT('2024-03-05', '%e %M %Y')") == "5 March 2024");
    REQUIRE(one(e, "DATE_FORMAT('2024-03-05', '%b %W %a')") == "Mar Tuesday Tue");
    REQUIRE(one(e, "DATE_FORMAT('2024-03-05', '%j')") == "065");
    REQUIRE(one(e, "DATE_FORMAT('2024-03-05 14:07:09', '%H:%i:%s')") == "14:07:09");
    REQUIRE(one(e, "DATE_FORMAT('2024-03-05', '100%%')") == "100%");
    REQUIRE(errorOf(e, "DIKHAO YEAR('nope') SE one;").find("valid DATE nahi") != std::string::npos);

    // NOW() is 'YYYY-MM-DD HH:MM:SS' in local time; CURDATE() / CURRENT_DATE are DATEs
    std::string now = one(e, "NOW()");
    REQUIRE(now.size() == 19);
    REQUIRE(now[4] == '-');
    REQUIRE(now[10] == ' ');
    REQUIRE(now[13] == ':');
    std::time_t t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    REQUIRE(std::stoi(now.substr(0, 4)) >= local.tm_year + 1900 - 1);
    REQUIRE(one(e, "CURDATE()").size() == 10);
    REQUIRE(one(e, "CURRENT_DATE") == one(e, "CURDATE()"));
    REQUIRE(one(e, "DATEDIFF(CURDATE(), CURRENT_DATE)") == "0");
    REQUIRE(one(e, "DATEDIFF(NOW(), CURDATE())") == "0");
    REQUIRE(one(e, "YEAR(NOW())") == now.substr(0, 4));
    auto r = run(e, "DIKHAO CURDATE() SE one;");
    REQUIRE(std::holds_alternative<Date>(r.rows[0][0].data));
    run(e, "BANAO TABLE logs (d DATE);");
    run(e, "DAALO MEIN logs (d) MAAN (CURDATE());");
    REQUIRE(run(e, "DIKHAO d SE logs;").rows.size() == 1);
}

// --------------------------------------------------------------------- CAST ----

TEST_CASE("CAST", "[sql][functions]") {
    TempDir dir;
    Engine e(dir.str());
    seedOne(e);
    REQUIRE(one(e, "CAST('42' AS INT)") == "42");
    REQUIRE(one(e, "CAST(3.7 AS INT)") == "4");
    REQUIRE(one(e, "CAST(5 AS FLOAT)") == "5.0");
    REQUIRE(one(e, "CAST('2.5' AS FLOAT)") == "2.5");
    REQUIRE(one(e, "CAST(12 AS TEXT)") == "12");
    REQUIRE(one(e, "CAST(12 KAHO SHABD)") == "12");
    REQUIRE(one(e, "CAST('7' AS ANK) + 1") == "8");
    REQUIRE(one(e, "CAST(1.5 AS DASHAMLAV)") == "1.5");
    REQUIRE(one(e, "CAST('2024-03-05' AS DATE)") == "2024-03-05");
    REQUIRE(one(e, "CAST('2024-03-05' AS TAREEKH)") == "2024-03-05");
    REQUIRE(one(e, "CAST(1 AS BOOL)") == "SACH");
    REQUIRE(one(e, "CAST(0 AS BOOL)") == "JHOOTH");
    REQUIRE(one(e, "CAST('sach' AS BOOL)") == "SACH");
    REQUIRE(one(e, "CAST(SACH AS INT)") == "1");
    REQUIRE(one(e, "cast('9' as int)") == "9");
    REQUIRE(errorOf(e, "DIKHAO CAST('abc' AS INT) SE one;").find("cast nahi kar sakte") != std::string::npos);
    REQUIRE(errorOf(e, "DIKHAO CAST(1 AS BLOB) SE one;").find("CAST ke liye type expected tha") != std::string::npos);
    REQUIRE(errorOf(e, "DIKHAO CAST(1) SE one;").find("KAHO (ya AS) expected tha") != std::string::npos);
    REQUIRE(run(e, "DIKHAO CAST(id AS TEXT) SE one;").columns[0] == "CAST(id AS TEXT)");
}

// ------------------------------------------------------------ parser details ----

TEST_CASE("scalar function parsing: arity, zero arguments, nesting", "[sql][parser]") {
    TempDir dir;
    Engine e(dir.str());
    seedOne(e);
    REQUIRE(errorOf(e, "DIKHAO UPPER('a', 'b') SE one;").find("UPPER ko 1 argument chahiye (2 mile)") == 0);
    REQUIRE(errorOf(e, "DIKHAO UPPER() SE one;").find("UPPER ko 1 argument chahiye (0 mile)") == 0);
    REQUIRE(errorOf(e, "DIKHAO NOW(1) SE one;").find("NOW ko 0 argument chahiye (1 mile)") == 0);
    REQUIRE(errorOf(e, "DIKHAO SUBSTRING('a') SE one;").find("SUBSTRING ko 2 ya 3 argument chahiye") == 0);
    REQUIRE(errorOf(e, "DIKHAO CONCAT() SE one;").find("CONCAT ko kam se kam 1 argument chahiye") == 0);
    REQUIRE(one(e, "UPPER(LEFT(TRIM('  hello  '), 3))") == "HEL");
    REQUIRE(one(e, "LENGTH('ab' || 'cd') + 1") == "5");
    auto e1 = parseExpression("LENGTH(naam)");
    auto* sf = dynamic_cast<ast::ScalarFunc*>(e1.get());
    REQUIRE(sf != nullptr);
    REQUIRE(sf->name == "LENGTH");
    REQUIRE(sf->args.size() == 1);
    REQUIRE(dynamic_cast<ast::Coalesce*>(parseExpression("IFNULL(a, 0)").get()) != nullptr);
    REQUIRE(findScalarFunc("DATE_FORMAT") != nullptr);
    REQUIRE(findScalarFunc("FOO") == nullptr);
}

TEST_CASE("the unknown-function error lists every supported name", "[sql][functions]") {
    TempDir dir;
    Engine e(dir.str());
    seedOne(e);
    std::string message = errorOf(e, "DIKHAO FOO(id) SE one;");
    REQUIRE(message.find("Function 'FOO' nahi pata. Ye chalte hain: GINO, KUL, AUSAT, NYUNTAM, ADHIKTAM, PEHLA, IFNULL, UPPER") == 0);
    REQUIRE(message.find("DATE_FORMAT") != std::string::npos);
    REQUIRE(message.find("CAST") != std::string::npos);
}

// ------------------------------------------------------------ LIMIT / OFFSET ----

TEST_CASE("SIRF n OFFSET m and SIRF m, n", "[sql][limit]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchool(e);
    REQUIRE(flat(run(e, "DIKHAO id SE students KRAM id SIRF 2 OFFSET 1;")) == "2;3");
    REQUIRE(flat(run(e, "DIKHAO id SE students KRAM id SIRF 2, 3;")) == "3;4;5");  // MySQL: SIRF offset, count
    REQUIRE(flat(run(e, "DIKHAO id SE students KRAM id SIRF 10 OFFSET 3;")) == "4;5");
    REQUIRE(flat(run(e, "DIKHAO id SE students KRAM id SIRF 3 OFFSET 99;")).empty());
    REQUIRE(flat(run(e, "DIKHAO id SE students KRAM id SIRF 2 offset 0;")) == "1;2");
    REQUIRE(flat(run(e, "DIKHAO id SE students KRAM id SIRF 0 OFFSET 1;")).empty());
    REQUIRE(flat(run(e, "DIKHAO id SE students KRAM id SIRF 2;")) == "1;2");
    REQUIRE(errorOf(e, "DIKHAO id SE students SIRF 2 OFFSET x;").find("OFFSET ke baad ek whole number") == 0);
    REQUIRE(errorOf(e, "DIKHAO id SE students SIRF 2, x;").find("ke baad ek whole number") != std::string::npos);
    auto plan = run(e, "SAMJHAO DIKHAO id SE students SIRF 2 OFFSET 1;");
    REQUIRE(formatValue(plan.rows.back()[0]).find("LIMIT  SIRF 2 OFFSET 1") != std::string::npos);
}

TEST_CASE("GROUP BY two columns, HAVING, ORDER BY and OFFSET combine", "[sql][limit]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchool(e);
    run(e, "BANAO TABLE sales (region TEXT, product TEXT, qty INT);");
    run(e, "DAALO MEIN sales (region, product, qty) MAAN ('N', 'a', 5), ('N', 'a', 7), ('N', 'b', 1), ('S', 'a', 2), "
           "('S', 'b', 9), ('S', 'b', 4), ('E', 'a', 3);");
    REQUIRE(flat(run(e, "DIKHAO region, product, KUL(qty) SE sales SAMOOH region, product KRAM region, product;")) ==
            "E|a|3;N|a|12;N|b|1;S|a|2;S|b|13");
    REQUIRE(flat(run(e, "DIKHAO region, product, KUL(qty) KAHO total SE sales SAMOOH region, product "
                        "JINKA KUL(qty) > 2 KRAM total ULTA SIRF 2 OFFSET 1;")) == "N|a|12;E|a|3");
    REQUIRE(flat(run(e, "DIKHAO region, GINO(*) SE sales SAMOOH region, product JINKA GINO(*) > 1 KRAM region;")) ==
            "N|2;S|2");
    REQUIRE(flat(run(e, "DIKHAO region, product SE sales SAMOOH region, product KRAM region, product SIRF 1, 2;")) ==
            "N|a;N|b");
}

// ------------------------------------------------------------------- USING ----

TEST_CASE("MILAO .. USING joins on the listed columns and shows them once in *", "[sql][join]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchool(e);
    run(e, "BANAO TABLE grades (id INT, grade TEXT);");
    run(e, "DAALO MEIN grades (id, grade) MAAN (1, 'A'), (2, 'B'), (7, 'C');");
    auto r = run(e, "DIKHAO * SE students s MILAO grades g USING (id) KRAM id;");
    INFO(r.error);
    REQUIRE(r.columns == std::vector<std::string>{"id", "naam", "cgpa", "cid", "shehar", "grade"});
    REQUIRE(flat(r) == "1|Ravi|8.4|10|Delhi|A;2|Priya|9.1|10|Pune|B");

    r = run(e, "DIKHAO s.naam, g.grade SE students s MILAO grades g USING (id) KRAM s.id;");
    REQUIRE(flat(r) == "Ravi|A;Priya|B");
    // unqualified use of the shared column is not ambiguous in the output, but is in an expression (like SQL)
    REQUIRE(flat(run(e, "DIKHAO g.* SE students s MILAO grades g USING (id) KRAM s.id;")) == "1|A;2|B");

    r = run(e, "DIKHAO * SE students s BAAYAN MILAO grades g USING (id) KRAM id;");
    REQUIRE(r.rows.size() == 5);
    REQUIRE(formatValue(r.rows[2][5]) == "KHALI");

    r = run(e, "DIKHAO * SE students s DAHINA MILAO grades g USING (id) KRAM id;");
    REQUIRE(r.rows.size() == 3);
    REQUIRE(r.columns[0] == "id");
    bool sawSeven = false;
    for (auto& row : r.rows) sawSeven = sawSeven || formatValue(row[0]) == "7";  // the unmatched right row keeps its id
    REQUIRE(sawSeven);

    r = run(e, "DIKHAO * SE students s DONO MILAO grades g USING (id);");
    REQUIRE(r.rows.size() == 6);

    // two columns
    run(e, "BANAO TABLE a (x INT, y INT, p TEXT);");
    run(e, "BANAO TABLE b (x INT, y INT, q TEXT);");
    run(e, "DAALO MEIN a (x, y, p) MAAN (1, 1, 'a11'), (1, 2, 'a12'), (2, 2, 'a22');");
    run(e, "DAALO MEIN b (x, y, q) MAAN (1, 1, 'b11'), (1, 2, 'b12'), (2, 1, 'b21');");
    REQUIRE(flat(run(e, "DIKHAO * SE a MILAO b USING (x, y) KRAM x, y;")) == "1|1|a11|b11;1|2|a12|b12");

    REQUIRE(errorOf(e, "DIKHAO * SE students s MILAO grades g USING (nope);").find("USING mein 'nope' table 'g' mein nahi hai") == 0);
    REQUIRE(errorOf(e, "DIKHAO * SE students s MILAO grades g USING (grade);").find("pehle ki kisi table mein nahi hai") != std::string::npos);
    auto plan = run(e, "SAMJHAO DIKHAO * SE students s MILAO grades g USING (id);");
    REQUIRE(formatValue(plan.rows[1][0]).find("HASH JOIN grades g PAR") != std::string::npos);
}

// ------------------------------------------------------- EXISTS / ANY / ALL ----

TEST_CASE("EXISTS and NOT EXISTS, correlated and not", "[sql][subquery]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchool(e);
    REQUIRE(flat(run(e, "DIKHAO title SE courses c JAHAN EXISTS (DIKHAO 1 SE students s JAHAN s.cid = c.id) KRAM title;")) ==
            "DBMS;OS");
    REQUIRE(flat(run(e, "DIKHAO title SE courses c JAHAN NAHI EXISTS (DIKHAO * SE students s JAHAN s.cid = c.id);")) == "AI");
    REQUIRE(flat(run(e, "DIKHAO title SE courses c JAHAN NOT EXISTS (DIKHAO * SE students s JAHAN s.cid = c.id);")) == "AI");
    REQUIRE(run(e, "DIKHAO id SE courses JAHAN EXISTS (DIKHAO id SE students JAHAN cgpa > 9);").rows.size() == 3);
    REQUIRE(run(e, "DIKHAO id SE courses JAHAN EXISTS (DIKHAO id SE students JAHAN cgpa > 99);").rows.empty());
    REQUIRE(run(e, "DIKHAO id SE courses JAHAN NAHI EXISTS (DIKHAO id SE students JAHAN cgpa > 99);").rows.size() == 3);
    // as a value in the select list, and combined with AUR / YA
    REQUIRE(flat(run(e, "DIKHAO title, EXISTS (DIKHAO 1 SE students s JAHAN s.cid = c.id) KAHO has SE courses c KRAM title;")) ==
            "AI|JHOOTH;DBMS|SACH;OS|SACH");
    REQUIRE(flat(run(e, "DIKHAO title SE courses c JAHAN id > 10 AUR EXISTS (DIKHAO 1 SE students s JAHAN s.cid = c.id);")) == "OS");
    REQUIRE(flat(run(e, "DIKHAO naam SE students s JAHAN EXISTS (DIKHAO 1 SE courses c JAHAN c.id = s.cid AUR c.title = 'OS') KRAM naam;")) ==
            "Aman;Neha");
    REQUIRE(run(e, "DIKHAO title, EXISTS (DIKHAO 1 SE students) SE courses;").columns[1] == "EXISTS (DIKHAO ...)");
    auto plan = run(e, "SAMJHAO DIKHAO title SE courses c JAHAN EXISTS (DIKHAO 1 SE students s JAHAN s.cid = c.id);");
    REQUIRE(plan.error.empty());
    REQUIRE(flat(plan).find("SUBQUERY (correlated)") != std::string::npos);
}

TEST_CASE("x op ANY / SOME / ALL (subquery)", "[sql][subquery]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchool(e);
    REQUIRE(flat(run(e, "DIKHAO naam SE students JAHAN cgpa > ANY (DIKHAO cgpa SE students JAHAN shehar = 'Pune') KRAM naam;")) ==
            "Aman;Neha;Priya;Ravi");
    REQUIRE(flat(run(e, "DIKHAO naam SE students JAHAN cgpa > ALL (DIKHAO cgpa SE students JAHAN shehar = 'Pune') KRAM naam;")).empty());
    REQUIRE(flat(run(e, "DIKHAO naam SE students JAHAN cgpa >= ALL (DIKHAO cgpa SE students) KRAM naam;")) == "Neha;Priya");
    REQUIRE(flat(run(e, "DIKHAO naam SE students JAHAN cgpa = SOME (DIKHAO cgpa SE students JAHAN id = 1);")) == "Ravi");
    REQUIRE(flat(run(e, "DIKHAO naam SE students JAHAN id = ANY (DIKHAO cid SE students) KRAM naam;")) == "");
    REQUIRE(flat(run(e, "DIKHAO title SE courses JAHAN id = ANY (DIKHAO cid SE students) KRAM title;")) == "DBMS;OS");
    // <> ALL is NOT IN, with the KHALI trap: cid contains a KHALI, so nothing is "<> ALL"
    REQUIRE(flat(run(e, "DIKHAO title SE courses JAHAN id <> ALL (DIKHAO cid SE students) KRAM title;")).empty());
    REQUIRE(flat(run(e, "DIKHAO title SE courses JAHAN id <> ALL (DIKHAO cid SE students JAHAN cid HAI NAHI KHALI) KRAM title;")) == "AI");
    // empty subquery: ANY is JHOOTH, ALL is SACH
    REQUIRE(run(e, "DIKHAO id SE courses JAHAN id > ANY (DIKHAO id SE students JAHAN id > 99);").rows.empty());
    REQUIRE(run(e, "DIKHAO id SE courses JAHAN id > ALL (DIKHAO id SE students JAHAN id > 99);").rows.size() == 3);
    // correlated
    REQUIRE(flat(run(e, "DIKHAO naam SE students s JAHAN cgpa >= ALL (DIKHAO cgpa SE students t JAHAN t.shehar = s.shehar) KRAM naam;")) ==
            "Neha;Priya");
    REQUIRE(errorOf(e, "DIKHAO id SE courses JAHAN id = ANY (DIKHAO id, title SE courses);").find("sirf 1 column") != std::string::npos);
    REQUIRE(run(e, "DIKHAO id SE courses JAHAN id < ANY (DIKHAO id SE courses);").columns.size() == 1);
}

// ----------------------------------------------------------- derived tables ----

TEST_CASE("derived tables: a subquery in FROM or as a JOIN operand", "[sql][derived]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchool(e);
    auto r = run(e, "DIKHAO t.shehar, t.n SE (DIKHAO shehar, GINO(*) KAHO n SE students SAMOOH shehar) t KRAM t.shehar;");
    REQUIRE(r.columns == std::vector<std::string>{"t.shehar", "t.n"});
    REQUIRE(flat(r) == "Delhi|3;Pune|2");
    // without a qualifier, with a KAHO on the derived table's alias, and filtered
    REQUIRE(flat(run(e, "DIKHAO shehar SE (DIKHAO shehar, GINO(*) KAHO n SE students SAMOOH shehar) KAHO t JAHAN n > 2;")) == "Delhi");
    REQUIRE(flat(run(e, "DIKHAO AUSAT(n) SE (DIKHAO shehar, GINO(*) KAHO n SE students SAMOOH shehar) t;")) == "2.5");
    REQUIRE(flat(run(e, "DIKHAO * SE (DIKHAO id, UPPER(naam) KAHO big SE students JAHAN cgpa > 9) d KRAM id;")) == "2|PRIYA;5|NEHA");
    // as a JOIN operand
    r = run(e, "DIKHAO c.title, x.n SE courses c MILAO (DIKHAO cid, GINO(*) KAHO n SE students SAMOOH cid) x PAR x.cid = c.id KRAM c.title;");
    REQUIRE(flat(r) == "DBMS|2;OS|2");
    r = run(e, "DIKHAO c.title, x.n SE courses c BAAYAN MILAO (DIKHAO cid, GINO(*) KAHO n SE students SAMOOH cid) x PAR x.cid = c.id KRAM c.title;");
    REQUIRE(flat(r) == "AI|KHALI;DBMS|2;OS|2");
    // derived table joined with USING, nested derived tables, a derived table of a derived table
    r = run(e, "DIKHAO * SE (DIKHAO id, naam SE students JAHAN id < 3) a MILAO (DIKHAO id, cgpa SE students) b USING (id) KRAM id;");
    REQUIRE(flat(r) == "1|Ravi|8.4;2|Priya|9.1");
    REQUIRE(flat(run(e, "DIKHAO m SE (DIKHAO ADHIKTAM(n) KAHO m SE (DIKHAO shehar, GINO(*) KAHO n SE students SAMOOH shehar) t) u;")) == "3");
    // derived table with ORDER BY / LIMIT / OFFSET inside
    REQUIRE(flat(run(e, "DIKHAO id SE (DIKHAO id SE students KRAM id ULTA SIRF 3 OFFSET 1) q KRAM id;")) == "2;3;4");
    // inside a WHERE subquery
    REQUIRE(flat(run(e, "DIKHAO naam SE students JAHAN id MEIN (DIKHAO id SE (DIKHAO id SE students JAHAN id > 3) z) KRAM naam;")) == "Neha;Zoya");
    // errors
    REQUIRE(errorOf(e, "DIKHAO * SE (DIKHAO id SE students);").find("ko alias chahiye") != std::string::npos);
    REQUIRE(errorOf(e, "DIKHAO * SE (DIKHAO id, id SE students) t;").find("column 'id' do baar hai") != std::string::npos);
    REQUIRE(errorOf(e, "DIKHAO t.nope SE (DIKHAO id SE students) t;").find("nahi") != std::string::npos);
    auto plan = run(e, "SAMJHAO DIKHAO * SE (DIKHAO id SE students) t;");
    REQUIRE(formatValue(plan.rows[0][0]).find("FULL SCAN DERIVED TABLE t") != std::string::npos);
}

TEST_CASE("derived tables respect privileges of the tables they read", "[sql][derived]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin(inst);
    seedSchool(admin);
    admin.execute("BANAO USER ravi GUPT 'pw'; ADHIKAR DO DIKHAO PAR students KO ravi;");
    Engine ravi(inst);
    ravi.user = "ravi";
    REQUIRE(errorOf(ravi, "DIKHAO * SE (DIKHAO id SE students) t;").empty());
    REQUIRE(errorOf(ravi, "DIKHAO * SE (DIKHAO id SE courses) t;").find("ko table 'courses' par DIKHAO ka adhikar nahi hai") != std::string::npos);
    REQUIRE(errorOf(ravi, "DIKHAO * SE (DIKHAO id SE students) t MILAO (DIKHAO id SE courses) c USING (id);")
                .find("table 'courses'") != std::string::npos);
}
