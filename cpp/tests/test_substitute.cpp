// cpp/tests/test_substitute.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/parser.h"
#include "meradb/substitute.h"

using namespace meradb;

namespace {
// naya.x -> the given int
RefReplacer nayaX(int64_t value) {
    return [value](const ast::ColumnRef& ref) -> std::optional<Value> {
        if (ref.table && *ref.table == "naya" && ref.name == "x") return Value(value);
        return std::nullopt;
    };
}
// a BARE (unqualified) `pct` -> the given int, like a procedure parameter
RefReplacer paramPct(int64_t value) {
    return [value](const ast::ColumnRef& ref) -> std::optional<Value> {
        if (!ref.table && ref.name == "pct") return Value(value);
        return std::nullopt;
    };
}
const ast::Literal* asLiteral(const ast::Expr* e) { return dynamic_cast<const ast::Literal*>(e); }
int64_t intOf(const ast::Expr* e) {
    auto* lit = asLiteral(e);
    REQUIRE(lit != nullptr);
    return std::get<int64_t>(lit->value.data);
}
template <typename T>
T& first(std::vector<std::unique_ptr<ast::Statement>>& stmts) {
    auto* p = dynamic_cast<T*>(stmts.at(0).get());
    REQUIRE(p != nullptr);
    return *p;
}
}  // namespace

TEST_CASE("substitute UPDATE assignments and WHERE", "[substitute]") {
    auto stmts = parseScript("BADLO t RAKHO a = naya.x + 1 JAHAN id = naya.x;");
    auto& upd = first<ast::Update>(stmts);
    substituteStatementInPlace(upd, nayaX(5));
    auto* sum = dynamic_cast<const ast::BinaryOp*>(upd.assignments[0].second.get());
    REQUIRE(sum != nullptr);
    CHECK(intOf(sum->left.get()) == 5);
    auto* eq = dynamic_cast<const ast::BinaryOp*>(upd.where.get());
    REQUIRE(eq != nullptr);
    CHECK(dynamic_cast<const ast::ColumnRef*>(eq->left.get()) != nullptr);  // `id` is not a NAYA reference
    CHECK(intOf(eq->right.get()) == 5);
}

TEST_CASE("substitute INSERT rows, INSERT..SELECT and upsert assignments", "[substitute]") {
    auto stmts = parseScript(
        "DAALO MEIN t MAAN (naya.x, 'a'), (naya.x + 1, 'b') TAKRAAV PAR BADLO a = naya.x;"
        "DAALO MEIN t DIKHAO naya.x SE u JAHAN id = naya.x;");
    auto& ins = first<ast::Insert>(stmts);
    substituteStatementInPlace(ins, nayaX(7));
    CHECK(intOf(ins.rows[0][0].get()) == 7);
    auto* plus = dynamic_cast<const ast::BinaryOp*>(ins.rows[1][0].get());
    REQUIRE(plus != nullptr);
    CHECK(intOf(plus->left.get()) == 7);
    REQUIRE(ins.onConflictUpdate.has_value());
    CHECK(intOf((*ins.onConflictUpdate)[0].second.get()) == 7);

    auto& sel = *dynamic_cast<ast::Insert&>(*stmts[1]).select;
    substituteStatementInPlace(*stmts[1], nayaX(9));
    CHECK(intOf(sel.columns[0].get()) == 9);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(sel.where.get())->right.get()) == 9);
}

TEST_CASE("substitute SELECT columns, join ON, group by, having, order by", "[substitute]") {
    auto stmts = parseScript(
        "DIKHAO naya.x, GINO(*) SE t MILAO u PAR t.id = naya.x JAHAN naya.x > 0 "
        "SAMOOH naya.x JINKA GINO(*) > naya.x KRAM naya.x;");
    auto& sel = first<ast::Select>(stmts);
    substituteStatementInPlace(sel, nayaX(3));
    CHECK(intOf(sel.columns[0].get()) == 3);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(sel.joins[0].on.get())->right.get()) == 3);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(sel.where.get())->left.get()) == 3);
    CHECK(intOf(sel.groupBy[0].get()) == 3);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(sel.having.get())->right.get()) == 3);
    CHECK(intOf(sel.orderBy[0].expr.get()) == 3);
}

TEST_CASE("substitute leaves subqueries and non-DML statements alone", "[substitute]") {
    auto stmts = parseScript("DIKHAO * SE t JAHAN id MEIN (DIKHAO naya.x SE u);");
    auto& sel = first<ast::Select>(stmts);
    substituteStatementInPlace(sel, nayaX(1));
    auto* in = dynamic_cast<const ast::InSubquery*>(sel.where.get());
    REQUIRE(in != nullptr);
    // the subquery's own NAYA reference is NOT replaced (Python: "out of scope")
    CHECK(dynamic_cast<const ast::ColumnRef*>(in->subquery->statement->columns[0].get()) != nullptr);

    auto ddl = parseScript("BANAO TABLE x (id INT);");
    substituteStatementInPlace(*ddl[0], nayaX(1));  // no effect, no throw
    CHECK(dynamic_cast<ast::CreateTable*>(ddl[0].get()) != nullptr);
}

TEST_CASE("substitute procedure parameters replace only BARE names", "[substitute]") {
    auto stmts = parseScript("BADLO emp RAKHO salary = salary + pct JAHAN emp.pct = pct;");
    auto& upd = first<ast::Update>(stmts);
    substituteStatementInPlace(upd, paramPct(10));
    auto* sum = dynamic_cast<const ast::BinaryOp*>(upd.assignments[0].second.get());
    CHECK(intOf(sum->right.get()) == 10);
    auto* eq = dynamic_cast<const ast::BinaryOp*>(upd.where.get());
    CHECK(dynamic_cast<const ast::ColumnRef*>(eq->left.get()) != nullptr);  // emp.pct is qualified: untouched
    CHECK(intOf(eq->right.get()) == 10);
}

TEST_CASE("substitute reaches CASE, COALESCE, IS NULL and unary operands", "[substitute]") {
    // PEHLA(...) = COALESCE, AGAR ... TAB ... WARNA ... KHATAM = CASE, HAI KHALI = IS NULL
    auto stmts = parseScript(
        "BADLO t RAKHO a = PEHLA(naya.x, 0), b = AGAR naya.x > 1 TAB -naya.x WARNA 0 KHATAM JAHAN naya.x HAI KHALI;");
    auto& upd = first<ast::Update>(stmts);
    substituteStatementInPlace(upd, nayaX(4));
    auto* co = dynamic_cast<const ast::Coalesce*>(upd.assignments[0].second.get());
    REQUIRE(co != nullptr);
    CHECK(intOf(co->args[0].get()) == 4);
    auto* cw = dynamic_cast<const ast::CaseWhen*>(upd.assignments[1].second.get());
    REQUIRE(cw != nullptr);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(cw->branches[0].first.get())->left.get()) == 4);
    auto* neg = dynamic_cast<const ast::UnaryOp*>(cw->branches[0].second.get());
    REQUIRE(neg != nullptr);
    CHECK(intOf(neg->operand.get()) == 4);
    auto* isnull = dynamic_cast<const ast::IsNull*>(upd.where.get());
    REQUIRE(isnull != nullptr);
    CHECK(intOf(isnull->expr.get()) == 4);
}

TEST_CASE("substitute CALL arguments", "[substitute]") {
    auto stmts = parseScript("CHALAO other(naya.x, 5);");
    auto& call = first<ast::CallProcedure>(stmts);
    substituteStatementInPlace(call, nayaX(2));
    CHECK(intOf(call.args[0].get()) == 2);
    CHECK(intOf(call.args[1].get()) == 5);
}
