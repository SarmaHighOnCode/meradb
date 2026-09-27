// cpp/tests/test_parser_expressions.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/parser.h"
#include "meradb/errors.h"

using namespace meradb;
using namespace meradb::ast;

TEST_CASE("parseExpression parses arithmetic with correct precedence", "[parser][expr]") {
    auto e = parseExpression("1 + 2 * 3");
    auto* bin = dynamic_cast<BinaryOp*>(e.get());
    REQUIRE(bin != nullptr);
    REQUIRE(bin->op == "+");
    REQUIRE(dynamic_cast<Literal*>(bin->left.get()) != nullptr);
    REQUIRE(dynamic_cast<BinaryOp*>(bin->right.get()) != nullptr);  // "2 * 3" binds tighter
}

TEST_CASE("parseExpression folds unary minus on numeric literals", "[parser][expr]") {
    auto e = parseExpression("-5");
    auto* lit = dynamic_cast<Literal*>(e.get());
    REQUIRE(lit != nullptr);
    REQUIRE(std::get<int64_t>(lit->value.data) == -5);
}

TEST_CASE("parseExpression desugars BEECH into AND of range comparisons", "[parser][expr]") {
    auto e = parseExpression("umar BEECH 18 AUR 25");
    auto* outer = dynamic_cast<BinaryOp*>(e.get());
    REQUIRE(outer != nullptr);
    REQUIRE(outer->op == "AUR");
    REQUIRE(dynamic_cast<BinaryOp*>(outer->left.get())->op == ">=");
    REQUIRE(dynamic_cast<BinaryOp*>(outer->right.get())->op == "<=");
}

TEST_CASE("parseExpression desugars MEIN literal list into OR chain", "[parser][expr]") {
    auto e = parseExpression("shehar MEIN ('Delhi', 'Pune')");
    auto* outer = dynamic_cast<BinaryOp*>(e.get());
    REQUIRE(outer != nullptr);
    REQUIRE(outer->op == "YA");
}

TEST_CASE("parseExpression parses IS NULL and IS NOT NULL", "[parser][expr]") {
    auto e1 = parseExpression("umar HAI KHALI");
    REQUIRE(dynamic_cast<IsNull*>(e1.get())->negated == false);
    auto e2 = parseExpression("umar HAI NAHI KHALI");
    REQUIRE(dynamic_cast<IsNull*>(e2.get())->negated == true);
}

TEST_CASE("parseExpression parses CASE WHEN with WARNA else and KHATAM end", "[parser][expr]") {
    auto e = parseExpression("AGAR umar < 18 TAB 'minor' WARNA 'adult' KHATAM");
    auto* cw = dynamic_cast<CaseWhen*>(e.get());
    REQUIRE(cw != nullptr);
    REQUIRE(cw->branches.size() == 1);
    REQUIRE(cw->elseExpr != nullptr);
}

TEST_CASE("parseExpression parses multiple CASE WHEN branches", "[parser][expr]") {
    auto e = parseExpression("AGAR umar < 13 TAB 'child' AGAR umar < 18 TAB 'teen' WARNA 'adult' KHATAM");
    auto* cw = dynamic_cast<CaseWhen*>(e.get());
    REQUIRE(cw != nullptr);
    REQUIRE(cw->branches.size() == 2);
    REQUIRE(cw->elseExpr != nullptr);
}

TEST_CASE("parseExpression parses PEHLA as COALESCE", "[parser][expr]") {
    auto e = parseExpression("PEHLA(grade, 'Ungraded')");
    auto* c = dynamic_cast<Coalesce*>(e.get());
    REQUIRE(c != nullptr);
    REQUIRE(c->args.size() == 2);
}

TEST_CASE("parseExpression parses COALESCE spelled out", "[parser][expr]") {
    auto e = parseExpression("COALESCE(grade, 'Ungraded')");
    auto* c = dynamic_cast<Coalesce*>(e.get());
    REQUIRE(c != nullptr);
    REQUIRE(c->args.size() == 2);
}

TEST_CASE("parseExpression parses aggregate function calls", "[parser][expr]") {
    auto e = parseExpression("GINO(*)");
    auto* f = dynamic_cast<FuncCall*>(e.get());
    REQUIRE(f != nullptr);
    REQUIRE(f->name == "GINO");
    REQUIRE(dynamic_cast<Star*>(f->arg.get()) != nullptr);
}

TEST_CASE("parseExpression parses qualified column and qualified star", "[parser][expr]") {
    auto e1 = parseExpression("s.naam");
    auto* col = dynamic_cast<ColumnRef*>(e1.get());
    REQUIRE(col != nullptr);
    REQUIRE(col->name == "naam");
    REQUIRE(col->table.value() == "s");

    auto e2 = parseExpression("s.*");
    auto* star = dynamic_cast<Star*>(e2.get());
    REQUIRE(star != nullptr);
    REQUIRE(star->table.value() == "s");
}

TEST_CASE("parseExpression parses infix NAHI JAISA / BEECH / MEIN", "[parser][expr]") {
    auto e1 = parseExpression("naam NAHI JAISA 'A%'");
    auto* u1 = dynamic_cast<UnaryOp*>(e1.get());
    REQUIRE(u1 != nullptr);
    REQUIRE(u1->op == "NAHI");
    REQUIRE(dynamic_cast<BinaryOp*>(u1->operand.get())->op == "JAISA");

    auto e2 = parseExpression("umar NAHI BEECH 18 AUR 25");
    auto* u2 = dynamic_cast<UnaryOp*>(e2.get());
    REQUIRE(u2 != nullptr);
    REQUIRE(u2->op == "NAHI");

    auto e3 = parseExpression("shehar NAHI MEIN ('Delhi', 'Pune')");
    auto* u3 = dynamic_cast<UnaryOp*>(e3.get());
    REQUIRE(u3 != nullptr);
    REQUIRE(u3->op == "NAHI");
}

TEST_CASE("parseExpression throws ParseError on malformed input", "[parser][expr]") {
    REQUIRE_THROWS_AS(parseExpression("1 + "), ParseError);
}
