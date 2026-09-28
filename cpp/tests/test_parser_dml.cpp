// cpp/tests/test_parser_dml.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/parser.h"
#include "meradb/errors.h"

using namespace meradb;
using namespace meradb::ast;

TEST_CASE("parseScript parses INSERT with explicit columns", "[parser][dml]") {
    auto stmts = parseScript("DAALO MEIN students (id, naam) MAAN (1, 'Ravi');");
    auto* ins = dynamic_cast<Insert*>(stmts[0].get());
    REQUIRE(ins->table == "students");
    REQUIRE(ins->columns.has_value());
    REQUIRE(ins->columns->size() == 2);
    REQUIRE(ins->rows.size() == 1);
    REQUIRE(ins->rows[0].size() == 2);
}

TEST_CASE("parseScript parses multi-row INSERT", "[parser][dml]") {
    auto stmts = parseScript("DAALO MEIN t MAAN (1), (2), (3);");
    REQUIRE(dynamic_cast<Insert*>(stmts[0].get())->rows.size() == 3);
}

TEST_CASE("parseScript parses INSERT ... SELECT", "[parser][dml]") {
    auto stmts = parseScript("DAALO MEIN backup DIKHAO * SE students;");
    auto* ins = dynamic_cast<Insert*>(stmts[0].get());
    REQUIRE(ins->select != nullptr);
    REQUIRE(ins->rows.empty());
}

TEST_CASE("parseScript parses upsert TAKRAAV PAR BADLO", "[parser][dml]") {
    auto stmts = parseScript("DAALO MEIN t MAAN (1, 'x') TAKRAAV PAR BADLO naam = naam;");
    auto* ins = dynamic_cast<Insert*>(stmts[0].get());
    REQUIRE(ins->onConflictUpdate.has_value());
    REQUIRE(ins->onConflictUpdate->size() == 1);
}

TEST_CASE("parseScript parses SELECT with WHERE/ORDER/LIMIT", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO naam, cgpa SE students JAHAN cgpa > 8 KRAM cgpa ULTA SIRF 5;");
    auto* sel = dynamic_cast<Select*>(stmts[0].get());
    REQUIRE(sel->columns.size() == 2);
    REQUIRE(sel->where != nullptr);
    REQUIRE(sel->orderBy.size() == 1);
    REQUIRE(sel->orderBy[0].descending);
    REQUIRE(sel->limit == 5);
}

TEST_CASE("parseScript parses SELECT aliases, DISTINCT and qualified star", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO ALAG s.*, naam KAHO n SE students s JAHAN s.id > 1;");
    auto* sel = dynamic_cast<Select*>(stmts[0].get());
    REQUIRE(sel->distinct);
    REQUIRE(dynamic_cast<Star*>(sel->columns[0].get())->table.value() == "s");
    REQUIRE_FALSE(sel->aliases[0].has_value());
    REQUIRE(sel->aliases[1].value() == "n");
    REQUIRE(sel->alias.value() == "s");
    REQUIRE(sel->where != nullptr);
}

TEST_CASE("parseScript never takes a keyword as a table alias", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO * SE students JAHAN id = 1;");
    auto* sel = dynamic_cast<Select*>(stmts[0].get());
    REQUIRE_FALSE(sel->alias.has_value());
    REQUIRE(sel->where != nullptr);
    auto s2 = parseScript("DIKHAO * SE a MILAO b PAR a.id = b.id;");
    auto* sel2 = dynamic_cast<Select*>(s2[0].get());
    REQUIRE_FALSE(sel2->alias.has_value());
    REQUIRE(sel2->joins.size() == 1);
    REQUIRE(sel2->joins[0].kind == "INNER");
    REQUIRE(sel2->joins[0].alias == "b");  // defaults to the table name
}

TEST_CASE("parseScript parses LEFT/RIGHT/FULL/NATURAL JOIN", "[parser][dml]") {
    auto s1 = parseScript("DIKHAO * SE a BAAYAN MILAO b PAR a.id = b.id;");
    REQUIRE(dynamic_cast<Select*>(s1[0].get())->joins[0].kind == "LEFT");
    auto s2 = parseScript("DIKHAO * SE a DAHINA MILAO b PAR a.id = b.id;");
    REQUIRE(dynamic_cast<Select*>(s2[0].get())->joins[0].kind == "RIGHT");
    auto s3 = parseScript("DIKHAO * SE a DONO MILAO b PAR a.id = b.id;");
    REQUIRE(dynamic_cast<Select*>(s3[0].get())->joins[0].kind == "FULL");
    auto s4 = parseScript("DIKHAO * SE a SAMAAN MILAO b;");
    auto& natural = dynamic_cast<Select*>(s4[0].get())->joins[0];
    REQUIRE(natural.kind == "NATURAL");
    REQUIRE(natural.on == nullptr);
}

TEST_CASE("parseScript requires PAR on non-NATURAL joins", "[parser][dml]") {
    REQUIRE_THROWS_AS(parseScript("DIKHAO * SE a MILAO b;"), ParseError);
}

TEST_CASE("parseScript parses GROUP BY / HAVING", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO shehar, GINO(*) SE students SAMOOH shehar JINKA GINO(*) > 1;");
    auto* sel = dynamic_cast<Select*>(stmts[0].get());
    REQUIRE(sel->groupBy.size() == 1);
    REQUIRE(sel->having != nullptr);
}

TEST_CASE("parseScript parses set operations into SetOp nodes", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO id SE a SANYUKT DIKHAO id SE b;");
    auto* so = dynamic_cast<SetOp*>(stmts[0].get());
    REQUIRE(so != nullptr);
    REQUIRE(so->op == "SANYUKT");
}

TEST_CASE("parseScript chains set operations left-associatively", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO id SE a SANYUKT DIKHAO id SE b CHHODKAR DIKHAO id SE c;");
    auto* outer = dynamic_cast<SetOp*>(stmts[0].get());
    REQUIRE(outer != nullptr);
    REQUIRE(outer->op == "CHHODKAR");
    auto* inner = dynamic_cast<SetOp*>(outer->left.get());
    REQUIRE(inner != nullptr);
    REQUIRE(inner->op == "SANYUKT");
    REQUIRE(dynamic_cast<Select*>(outer->right.get()) != nullptr);
}

TEST_CASE("parseScript parses DIKHAO TABLES and DIKHAO VIEWS", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO TABLES; DIKHAO VIEWS;");
    REQUIRE(dynamic_cast<ShowTables*>(stmts[0].get()) != nullptr);
    REQUIRE(dynamic_cast<ShowViews*>(stmts[1].get()) != nullptr);
}

TEST_CASE("parseScript captures CREATE VIEW body as raw source text", "[parser][dml]") {
    auto stmts = parseScript("BANAO VIEW toppers KAHO DIKHAO naam SE students JAHAN cgpa > 9;");
    auto* cv = dynamic_cast<CreateView*>(stmts[0].get());
    REQUIRE(cv != nullptr);
    REQUIRE(cv->name == "toppers");
    REQUIRE(cv->queryText == "DIKHAO naam SE students JAHAN cgpa > 9");
}

TEST_CASE("parseScript captures SHART source text", "[parser][dml]") {
    auto stmts = parseScript("BANAO TABLE t (umar INT SHART (umar >= 0 AUR umar < 150));");
    auto* ct = dynamic_cast<CreateTable*>(stmts[0].get());
    REQUIRE(ct->columns[0].check.value() == "umar >= 0 AUR umar < 150");
}

TEST_CASE("parseScript parses subqueries in expressions", "[parser][dml]") {
    auto stmts = parseScript(
        "DIKHAO naam SE students JAHAN id NAHI MEIN (DIKHAO student_id SE banned) "
        "AUR cgpa > (DIKHAO AUSAT(cgpa) SE students);");
    auto* sel = dynamic_cast<Select*>(stmts[0].get());
    auto* aur = dynamic_cast<BinaryOp*>(sel->where.get());
    REQUIRE(aur != nullptr);
    auto* notIn = dynamic_cast<UnaryOp*>(aur->left.get());
    REQUIRE(notIn != nullptr);
    REQUIRE(dynamic_cast<InSubquery*>(notIn->operand.get()) != nullptr);
    auto* gt = dynamic_cast<BinaryOp*>(aur->right.get());
    REQUIRE(dynamic_cast<Subquery*>(gt->right.get()) != nullptr);
}

TEST_CASE("parseExpression rejects trailing tokens", "[parser][dml]") {
    REQUIRE_THROWS_AS(parseExpression("umar > 0 umar"), ParseError);
}

TEST_CASE("parseScript parses UPDATE and DELETE", "[parser][dml]") {
    auto s1 = parseScript("BADLO students RAKHO umar = umar + 1 JAHAN umar HAI NAHI KHALI;");
    auto* upd = dynamic_cast<Update*>(s1[0].get());
    REQUIRE(upd->assignments.size() == 1);
    REQUIRE(upd->where != nullptr);
    auto s2 = parseScript("MITAO SE students JAHAN umar < 0;");
    REQUIRE(dynamic_cast<Delete*>(s2[0].get()) != nullptr);
    REQUIRE_THROWS_AS(parseScript("MITAO students;"), ParseError);  // SE is mandatory
}

TEST_CASE("parseScript parses bare transaction statements", "[parser][dml]") {
    auto stmts = parseScript("SHURU; PAKKA; WAPAS;");
    REQUIRE(dynamic_cast<Begin*>(stmts[0].get()) != nullptr);
    REQUIRE(dynamic_cast<Commit*>(stmts[1].get()) != nullptr);
    REQUIRE(dynamic_cast<Rollback*>(stmts[2].get()) != nullptr);
}

TEST_CASE("parseScript parses SAMJHAO wrapping a SELECT", "[parser][dml]") {
    auto stmts = parseScript("SAMJHAO DIKHAO * SE students;");
    auto* ex = dynamic_cast<Explain*>(stmts[0].get());
    REQUIRE(ex != nullptr);
    REQUIRE(dynamic_cast<Select*>(ex->statement.get()) != nullptr);
}
