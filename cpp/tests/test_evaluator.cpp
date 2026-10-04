// cpp/tests/test_evaluator.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/evaluator.h"
#include "meradb/parser.h"
#include "meradb/pyvalue.h"
#include <cmath>
#include <functional>
#include <unordered_set>

using namespace meradb;

namespace {
// Marks every qualified ColumnRef as bound, the shape the planner's bind()
// produces, so these tests can write rows keyed "alias.col" directly.
void markBound(ast::Expr* e) {
    using namespace ast;
    if (e == nullptr) return;
    if (auto* r = dynamic_cast<ColumnRef*>(e)) { r->bound = r->table.has_value(); return; }
    if (auto* b = dynamic_cast<BinaryOp*>(e)) { markBound(b->left.get()); markBound(b->right.get()); return; }
    if (auto* u = dynamic_cast<UnaryOp*>(e)) { markBound(u->operand.get()); return; }
    if (auto* i = dynamic_cast<IsNull*>(e)) { markBound(i->expr.get()); return; }
    if (auto* f = dynamic_cast<FuncCall*>(e)) { markBound(f->arg.get()); return; }
    if (auto* in = dynamic_cast<InSubquery*>(e)) { markBound(in->left.get()); return; }
    if (auto* c = dynamic_cast<Coalesce*>(e)) { for (auto& a : c->args) markBound(a.get()); return; }
    if (auto* w = dynamic_cast<CaseWhen*>(e)) {
        for (auto& [c, v] : w->branches) { markBound(c.get()); markBound(v.get()); }
        markBound(w->elseExpr.get());
    }
}

std::unique_ptr<ast::Expr> parseBound(const std::string& text) {
    auto expr = parseExpression(text);
    markBound(expr.get());
    return expr;
}

Value ev(const std::string& exprText, Row row = {}) {
    auto expr = parseBound(exprText);
    return evaluate(*expr, row);
}

std::string messageOf(const std::function<void()>& fn) {
    try { fn(); } catch (const MeraDBError& e) { return e.message(); }
    return "<no error>";
}

std::string evError(const std::string& exprText, Row row = {}) {
    return messageOf([&] { ev(exprText, row); });
}

bool B(const Value& v) { return std::get<bool>(v.data); }
int64_t I(const Value& v) { return std::get<int64_t>(v.data); }
double D(const Value& v) { return std::get<double>(v.data); }
std::string S(const Value& v) { return std::get<std::string>(v.data); }
Value txt(const char* s) { return Value(std::string(s)); }
}  // namespace

// ----------------------------------------------------------- arithmetic ----

TEST_CASE("evaluate computes arithmetic", "[evaluator]") {
    REQUIRE(I(ev("2 + 3 * 4")) == 14);
    REQUIRE(I(ev("7 - 10")) == -3);
    REQUIRE(D(ev("1 + 0.5")) == 1.5);
    REQUIRE(S(ev("'Ra' + 'vi'")) == "Ravi");
}

TEST_CASE("integer division truncates and modulo follows Python", "[evaluator]") {
    REQUIRE(I(ev("7 / 2")) == 3);
    REQUIRE(I(ev("-7 / 2")) == -3);   // int(-3.5), not floor
    REQUIRE(I(ev("-7 % 2")) == 1);    // Python floor modulo
    REQUIRE(I(ev("7 % -2")) == -1);
    REQUIRE(D(ev("7.0 / 2")) == 3.5);
    REQUIRE(D(ev("-7.5 % 2")) == 0.5);
}

TEST_CASE("integer division is correctly rounded before truncating, like Python", "[evaluator]") {
    // expected values printed by the reference Python: int(a / b)
    struct Case { int64_t a, b, q; };
    const Case cases[] = {
        {9007199254740993, 3, 3002399751580331},
        {-9007199254740993, 3, -3002399751580331},
        {INT64_MAX, 3, 3074457345618258432},
        {INT64_MIN, 7, -1317624576693539328},
        {4611686018427400249, -9876543210987, -466933},
        {123456789012345678, 1000000007, 123456788},
        {36028797018963969, 18014398509481987, 1},
        {1, 1152921504606846977, 0},
        {-9007199254740993, -9007199254740994, 0},
        {INT64_MAX, INT64_MAX - 1, 1},
    };
    for (const auto& c : cases) {
        Row row;
        row.emplace("s.a", Value(c.a));
        row.emplace("s.b", Value(c.b));
        REQUIRE(I(ev("s.a / s.b", row)) == c.q);
    }
}

TEST_CASE("pyTrueDivide returns Python's exact double", "[evaluator]") {
    // repr(a / b) from the reference Python
    REQUIRE(pyTrueDivide(9007199254740993, 3) == 3002399751580331.0);
    REQUIRE(pyTrueDivide(INT64_MAX, 3) == 3.0744573456182584e+18);
    REQUIRE(pyTrueDivide(4611686018427400249, -9876543210987) == -466933.20931327523);
    REQUIRE(pyTrueDivide(123456789012345678, 1000000007) == 123456788.14814816);
    REQUIRE(pyTrueDivide(36028797018963969, 18014398509481987) == 1.9999999999999998);
    REQUIRE(pyTrueDivide(1, 1152921504606846977) == 8.673617379884035e-19);
    REQUIRE(pyTrueDivide(-9007199254740993, -9007199254740994) == 0.9999999999999999);
    REQUIRE(pyTrueDivide(INT64_MAX, INT64_MAX - 1) == 1.0);
    REQUIRE(pyTrueDivide(7, 2) == 3.5);
    REQUIRE(std::signbit(pyTrueDivide(0, -5)));  // Python: 0 / -5 -> -0.0
}

TEST_CASE("division by zero and non-numbers are errors", "[evaluator]") {
    REQUIRE(evError("1 / 0") == "Zero se divide nahi kar sakte");
    REQUIRE(evError("1 % 0.0") == "Zero se divide nahi kar sakte");
    REQUIRE(evError("'a' * 2") == "'*' sirf numbers ke saath chalta hai, 'a' mila");
    REQUIRE(evError("1 + SACH") == "'+' sirf numbers ke saath chalta hai, 'SACH' mila");
    REQUIRE(evError("-s.naam", {{"s.naam", txt("x")}}) == "'-' sirf numbers ke saath chalta hai, 'x' mila");
}

TEST_CASE("integer overflow is reported instead of wrapping", "[evaluator]") {
    Row row = {{"s.big", Value(INT64_MAX)}, {"s.min", Value(INT64_MIN)}};
    REQUIRE_THROWS_AS(ev("s.big + 1", row), ExecutionError);
    REQUIRE_THROWS_AS(ev("s.big * 2", row), ExecutionError);
    REQUIRE_THROWS_AS(ev("s.min - 1", row), ExecutionError);
    REQUIRE_THROWS_AS(ev("-s.min", row), ExecutionError);
    REQUIRE_THROWS_AS(ev("s.min / -1", row), ExecutionError);
    REQUIRE(I(ev("s.min % -1", row)) == 0);
    REQUIRE(I(ev("s.big - 1", row)) == INT64_MAX - 1);
}

TEST_CASE("evaluate resolves ColumnRef from the row map", "[evaluator]") {
    REQUIRE(I(ev("s.umar", {{"s.umar", Value(int64_t{20})}})) == 20);
    REQUIRE(I(ev("umar", {{"umar", Value(int64_t{21})}})) == 21);
    REQUIRE(evError("s.nope") == "Column 's.nope' nahi mila");
}

TEST_CASE("an UNBOUND qualified column is looked up by its bare name, like Python", "[evaluator]") {
    // Python evaluates MAAN tuples and TAKRAAV PAR BADLO assignments without
    // binding them; `t.naam` then reads the row's plain `naam` key.
    auto expr = parseExpression("t.naam");
    REQUIRE(S(evaluate(*expr, {{"id", Value(int64_t{1})}, {"naam", txt("b")}})) == "b");
    REQUIRE(messageOf([&] { evaluate(*expr, {{"t.naam", txt("x")}}); }) == "Column 'naam' nahi mila");
    REQUIRE(refKey(dynamic_cast<const ast::ColumnRef&>(*expr)) == "naam");
    REQUIRE(exprLabel(*expr) == "t.naam");  // the header still shows the qualifier
}

TEST_CASE("TAKRAAV PAR BADLO naam = t.naam reads the attempted row", "[evaluator]") {
    auto stmts = parseScript("DAALO MEIN t MAAN (1,'b') TAKRAAV PAR BADLO naam = t.naam;");
    auto& ins = dynamic_cast<ast::Insert&>(*stmts.at(0));
    REQUIRE(ins.onConflictUpdate.has_value());
    auto& [column, value] = ins.onConflictUpdate->at(0);
    REQUIRE(column == "naam");
    Row env = {{"id", Value(int64_t{1})}, {"naam", txt("b")}};  // Python: dict(zip(column_names, attempted))
    REQUIRE(S(evaluate(*value, env)) == "b");
}

TEST_CASE("MAAN (2, x.y) reports the bare column name, like Python", "[evaluator]") {
    auto stmts = parseScript("DAALO MEIN t MAAN (2, x.y);");
    auto& ins = dynamic_cast<ast::Insert&>(*stmts.at(0));
    REQUIRE(I(evaluate(*ins.rows.at(0).at(0), {})) == 2);
    REQUIRE(messageOf([&] { evaluate(*ins.rows.at(0).at(1), {}); }) == "Column 'y' nahi mila");
}

TEST_CASE("evaluate propagates KHALI through arithmetic and comparison", "[evaluator]") {
    REQUIRE(ev("s.umar + 1", {{"s.umar", Value()}}).isNull());
    REQUIRE(ev("s.umar = s.umar", {{"s.umar", Value()}}).isNull());
    REQUIRE(ev("-s.umar", {{"s.umar", Value()}}).isNull());
    REQUIRE(ev("NAHI s.umar", {{"s.umar", Value()}}).isNull());
}

// ----------------------------------------------------------- comparison ----

TEST_CASE("comparisons follow Python ordering per type", "[evaluator]") {
    REQUIRE(B(ev("1 = 1.0")));
    REQUIRE(B(ev("2 > 1.5")));
    REQUIRE(B(ev("'abc' < 'abd'")));
    REQUIRE(B(ev("'Z' < 'a'")));
    REQUIRE(B(ev("JHOOTH < SACH")));
    REQUIRE(B(ev("3 != 4")));
    REQUIRE(B(ev("3 <= 3")));
    REQUIRE_FALSE(B(ev("3 >= 4")));
}

TEST_CASE("int vs float comparison is exact, like Python", "[evaluator]") {
    // 2^53 + 1 is not representable as a double; Python still says unequal
    Row row = {{"s.i", Value(int64_t{9007199254740993})}, {"s.f", Value(9007199254740992.0)}};
    REQUIRE(B(ev("s.i != s.f", row)));
    REQUIRE(B(ev("s.i > s.f", row)));
    REQUIRE(B(ev("s.f < s.i", row)));
}

TEST_CASE("NaN compares unequal to everything", "[evaluator]") {
    Row row = {{"s.n", Value(std::nan(""))}, {"s.i", Value(int64_t{1})}};
    REQUIRE_FALSE(B(ev("s.n = s.n", row)));
    REQUIRE(B(ev("s.n != s.i", row)));
    REQUIRE_FALSE(B(ev("s.i < s.n", row)));
}

TEST_CASE("comparing a DATE with text parses the text as a date", "[evaluator]") {
    Row row = {{"s.dob", Value(parseDate("2005-06-01"))}};
    REQUIRE(B(ev("s.dob > '2005-01-01'", row)));
    REQUIRE(B(ev("'2005-06-01' = s.dob", row)));
    REQUIRE(evError("s.dob > 'kal'", row) == "'kal' valid DATE nahi hai -- 'YYYY-MM-DD' format chahiye");
}

TEST_CASE("comparing different types is an error", "[evaluator]") {
    REQUIRE(evError("1 = 'a'") == "'1' aur 'a' ko compare nahi kar sakte (alag types)");
    REQUIRE(evError("SACH = 1") == "'SACH' aur '1' ko compare nahi kar sakte (alag types)");
}

// -------------------------------------------------------- AUR / YA / NAHI ----

TEST_CASE("evaluate implements three-valued AUR/YA", "[evaluator]") {
    Row nul = {{"s.x", Value()}};
    auto v = ev("s.x AUR JHOOTH", nul);
    REQUIRE(std::holds_alternative<bool>(v.data));
    REQUIRE_FALSE(B(v));
    REQUIRE_FALSE(B(ev("JHOOTH AUR s.x", nul)));
    REQUIRE(ev("SACH AUR s.x", nul).isNull());
    REQUIRE(B(ev("SACH YA s.x", nul)));
    REQUIRE(B(ev("s.x YA SACH", nul)));
    REQUIRE(ev("JHOOTH YA s.x", nul).isNull());
    REQUIRE(B(ev("SACH AUR SACH")));
    REQUIRE_FALSE(B(ev("JHOOTH YA JHOOTH")));
}

TEST_CASE("AUR short-circuits before evaluating the right side", "[evaluator]") {
    // the right side would divide by zero
    REQUIRE_FALSE(B(ev("JHOOTH AUR 1 / 0 = 1")));
    REQUIRE(B(ev("SACH YA 1 / 0 = 1")));
}

TEST_CASE("AUR/YA/NAHI need boolean operands", "[evaluator]") {
    REQUIRE(evError("1 AUR SACH") == "'AUR' ko SACH/JHOOTH condition chahiye, '1' mila");
    REQUIRE(evError("JHOOTH YA 'x'") == "'YA' ko SACH/JHOOTH condition chahiye, 'x' mila");
    REQUIRE(evError("NAHI 5") == "'NAHI' ko SACH/JHOOTH condition chahiye, '5' mila");
    REQUIRE(B(ev("NAHI JHOOTH")));
}

TEST_CASE("isTrue treats KHALI and JHOOTH both as not-true", "[evaluator]") {
    REQUIRE_FALSE(isTrue(Value()));
    REQUIRE_FALSE(isTrue(Value(false)));
    REQUIRE_FALSE(isTrue(Value(int64_t{1})));
    REQUIRE(isTrue(Value(true)));
}

TEST_CASE("evaluate handles IS NULL / IS NOT NULL", "[evaluator]") {
    REQUIRE(B(ev("s.x HAI KHALI", {{"s.x", Value()}})));
    REQUIRE(B(ev("s.x HAI NAHI KHALI", {{"s.x", Value(int64_t{1})}})));
    REQUIRE_FALSE(B(ev("s.x HAI KHALI", {{"s.x", Value(int64_t{1})}})));
}

// ---------------------------------------------------------------- JAISA ----

TEST_CASE("evaluate matches JAISA (LIKE) patterns", "[evaluator]") {
    Row ravi = {{"s.naam", txt("Ravi")}};
    REQUIRE(B(ev("s.naam JAISA 'R%'", ravi)));
    REQUIRE_FALSE(B(ev("s.naam JAISA 'X%'", ravi)));
    REQUIRE(B(ev("s.naam JAISA 'r_v_'", ravi)));     // case-insensitive, _ = one char
    REQUIRE_FALSE(B(ev("s.naam JAISA 'Rav'", ravi)));  // whole-string match
    REQUIRE(B(ev("s.naam JAISA '%'", ravi)));
    REQUIRE(B(ev("s.naam JAISA '%a%i'", ravi)));
    REQUIRE(ev("s.naam JAISA 'R%'", {{"s.naam", Value()}}).isNull());
}

TEST_CASE("JAISA treats regex characters literally and _ as one character", "[evaluator]") {
    REQUIRE(B(ev("'a.b(c)' JAISA 'a.b(c)'")));
    REQUIRE_FALSE(B(ev("'axb' JAISA 'a.b'")));
    // 'राम' is 3 code points (9 UTF-8 bytes)
    REQUIRE(B(ev("'\xE0\xA4\xB0\xE0\xA4\xBE\xE0\xA4\xAE' JAISA '___'")));
    REQUIRE_FALSE(B(ev("'\xE0\xA4\xB0\xE0\xA4\xBE\xE0\xA4\xAE' JAISA '__'")));
    // É / é fold together
    REQUIRE(B(ev("'\xC3\x89t\xC3\xA9' JAISA '\xC3\xA9T\xC3\x89'")));
    REQUIRE(B(ev("'a\nb' JAISA 'a%b'")));
}

TEST_CASE("JAISA folds case like Python's re.IGNORECASE beyond Latin-1", "[evaluator]") {
    // Łódź vs łódź (Latin Extended-A)
    REQUIRE(B(ev("'\xC5\x81\xC3\xB3" "d\xC5\xBA' JAISA '\xC5\x82\xC3\xB3" "d\xC5\xBA'")));
    // Kelvin sign K matches k; long s matches S; final sigma matches sigma
    REQUIRE(B(ev("'\xE2\x84\xAA' JAISA 'k'")));
    REQUIRE(B(ev("'\xC5\xBF' JAISA 'S'")));
    REQUIRE(B(ev("'\xCF\x82' JAISA '\xCE\xA3'")));
    // micro sign matches Greek mu
    REQUIRE(B(ev("'\xC2\xB5' JAISA '\xCE\x9C'")));
    REQUIRE_FALSE(B(ev("'\xC5\x81' JAISA 'L'")));
}

TEST_CASE("pyEquals and PyValueHash follow Python dict-key semantics", "[evaluator]") {
    REQUIRE(pyEquals(Value(int64_t{1}), Value(1.0)));
    REQUIRE(pyEquals(Value(true), Value(int64_t{1})));
    REQUIRE(pyEquals(Value(), Value()));
    REQUIRE_FALSE(pyEquals(Value(), Value(int64_t{0})));
    REQUIRE_FALSE(pyEquals(txt("2005-01-01"), Value(parseDate("2005-01-01"))));
    REQUIRE_FALSE(pyEquals(Value(std::nan("")), Value(std::nan(""))));
    REQUIRE_FALSE(pyEquals(Value(int64_t{9007199254740993}), Value(9007199254740992.0)));
    PyValueHash h;
    REQUIRE(h(Value(int64_t{1})) == h(Value(1.0)));
    REQUIRE(h(Value(true)) == h(Value(int64_t{1})));
    REQUIRE(h(Value(-0.0)) == h(Value(int64_t{0})));
    std::unordered_set<Value, PyValueHash, PyValueEq> set = {Value(int64_t{1}), Value(1.0), Value(true), txt("1")};
    REQUIRE(set.size() == 2);
    std::unordered_set<std::vector<Value>, PyValuesHash, PyValuesEq> rows = {
        {Value(int64_t{1}), Value()}, {Value(1.0), Value()}, {Value(int64_t{2}), Value()}};
    REQUIRE(rows.size() == 2);
}

TEST_CASE("JAISA needs text on both sides", "[evaluator]") {
    REQUIRE(evError("5 JAISA '5'") == "JAISA sirf TEXT ke saath chalta hai");
}

// -------------------------------------------------------- PEHLA / AGAR ----

TEST_CASE("evaluate resolves PEHLA/COALESCE to first non-null argument", "[evaluator]") {
    REQUIRE(S(ev("PEHLA(s.x, 'fallback')", {{"s.x", Value()}})) == "fallback");
    REQUIRE(ev("PEHLA(s.x, s.x)", {{"s.x", Value()}}).isNull());
}

TEST_CASE("evaluate resolves CASE WHEN branches in order", "[evaluator]") {
    auto v = ev("AGAR s.n < 18 TAB 'minor' WARNA 'adult' KHATAM", {{"s.n", Value(int64_t{10})}});
    REQUIRE(S(v) == "minor");
    REQUIRE(S(ev("AGAR s.n < 18 TAB 'minor' WARNA 'adult' KHATAM", {{"s.n", Value(int64_t{30})}})) == "adult");
    // unknown condition is not true; no WARNA -> KHALI
    REQUIRE(ev("AGAR s.n < 18 TAB 'minor' KHATAM", {{"s.n", Value()}}).isNull());
}

// ------------------------------------------------ aggregates / star / sub ----

TEST_CASE("evaluate reads a precomputed aggregate from the row", "[evaluator]") {
    auto expr = parseExpression("GINO(*) + 1");
    std::vector<const ast::FuncCall*> aggs;
    findAggregates(*expr, aggs);
    REQUIRE(aggs.size() == 1);
    Row row = {{aggKey(*aggs[0]), Value(int64_t{4})}};
    REQUIRE(I(evaluate(*expr, row)) == 5);
    REQUIRE(evError("KUL(s.x) > 1") ==
            "KUL(s.x) yahan nahi chal sakta -- aggregates sirf DIKHAO list, JINKA aur KRAM mein chalte hain");
}

TEST_CASE("evaluate throws when Star is evaluated as a value", "[evaluator]") {
    ast::Star star;
    REQUIRE_THROWS_AS(evaluate(star, {}), ExecutionError);
    REQUIRE(messageOf([&] { evaluate(star, {}); }) == "'*' yahan use nahi ho sakta");
}

TEST_CASE("evaluate looks subquery results up by node identity", "[evaluator]") {
    auto expr = parseBound("s.id MEIN (DIKHAO sid SE enroll) AUR s.umar > (DIKHAO 1 SE t)");
    std::vector<const ast::Expr*> subs;
    findSubqueries(*expr, subs);
    REQUIRE(subs.size() == 2);
    REQUIRE(dynamic_cast<const ast::InSubquery*>(subs[0]) != nullptr);
    REQUIRE(dynamic_cast<const ast::Subquery*>(subs[1]) != nullptr);

    SubqueryResults results;
    results[subs[0]].values = {Value(), Value(1.0), Value(int64_t{5})};  // 1 == 1.0, KHALI ignored
    results[subs[1]].scalar = Value(int64_t{18});
    Row row = {{"s.id", Value(int64_t{1})}, {"s.umar", Value(int64_t{20})}};
    REQUIRE(B(evaluate(*expr, row, &results)));
    row["s.id"] = Value(int64_t{2});
    REQUIRE_FALSE(B(evaluate(*expr, row, &results)));
    row["s.id"] = Value();
    REQUIRE(evaluate(*expr, row, &results).isNull());  // KHALI MEIN (...) -> KHALI, and KHALI AUR SACH -> KHALI
    // missing result is an internal error
    REQUIRE(messageOf([&] { evaluate(*expr, row); }) == "Subquery ka result pehle se ready nahi tha (internal error)");
}

TEST_CASE("NAHI MEIN negates membership", "[evaluator]") {
    auto expr = parseBound("s.id NAHI MEIN (DIKHAO sid SE enroll)");
    std::vector<const ast::Expr*> subs;
    findSubqueries(*expr, subs);
    REQUIRE(subs.size() == 1);
    SubqueryResults results;
    results[subs[0]].values = {Value(int64_t{1})};
    REQUIRE(B(evaluate(*expr, {{"s.id", Value(int64_t{2})}}, &results)));
    REQUIRE_FALSE(B(evaluate(*expr, {{"s.id", Value(int64_t{1})}}, &results)));
}

// ------------------------------------------------------------ utilities ----

TEST_CASE("exprLabel renders readable headers like Python", "[evaluator]") {
    auto label = [](const std::string& text) { return exprLabel(*parseExpression(text)); };
    REQUIRE(label("s.umar + 1") == "s.umar + 1");
    REQUIRE(label("'Ravi'") == "'Ravi'");
    REQUIRE(label("2.5") == "2.5");
    REQUIRE(label("KHALI") == "KHALI");
    REQUIRE(label("GINO(*)") == "GINO(*)");
    REQUIRE(label("COUNT(naam)") == "COUNT(naam)");
    REQUIRE(label("naam HAI NAHI KHALI") == "naam HAI NAHI KHALI");
    REQUIRE(label("PEHLA(a, 0)") == "PEHLA(a, 0)");
    REQUIRE(label("AGAR a TAB 1 KHATAM") == "AGAR ... KHATAM");
    // the parser turns infix `a NAHI MEIN (...)` into NAHI(a MEIN (...)), like Python
    REQUIRE(label("a NAHI MEIN (DIKHAO b SE t)") == "NAHI a MEIN (DIKHAO ...)");
    ast::InSubquery negated;
    negated.left = std::make_unique<ast::ColumnRef>("a");
    negated.negated = true;
    REQUIRE(exprLabel(negated) == "a NAHI MEIN (DIKHAO ...)");
    REQUIRE(label("NAHI a") == "NAHI a");
    REQUIRE(label("-a") == "-a");
}

TEST_CASE("columnRefs finds columns and can skip aggregate arguments", "[evaluator]") {
    auto expr = parseExpression("a + KUL(b) > PEHLA(c, AGAR d TAB e WARNA f KHATAM)");
    REQUIRE(columnRefKeys(expr.get()) == std::vector<std::string>{"a", "b", "c", "d", "e", "f"});
    REQUIRE(columnRefKeys(expr.get(), true) == std::vector<std::string>{"a", "c", "d", "e", "f"});
    REQUIRE(columnRefKeys(nullptr).empty());
    std::vector<const ast::ColumnRef*> refs;
    auto sub = parseExpression("s.x MEIN (DIKHAO y SE t)");
    columnRefs(*sub, refs);
    REQUIRE(refs.empty());  // no descent into subqueries
}

TEST_CASE("columnRefNodes keeps qualification visible and skips PEHLA/AGAR", "[evaluator]") {
    std::vector<const ast::ColumnRef*> refs;
    auto expr = parseExpression("s.a > 1 AUR GINO(b) HAI KHALI AUR PEHLA(c, 1) = 1");
    columnRefNodes(*expr, refs);
    REQUIRE(refs.size() == 2);
    REQUIRE(refs[0]->table.value() == "s");
    REQUIRE(refs[1]->name == "b");
}

TEST_CASE("findAggregates does not look inside an aggregate", "[evaluator]") {
    std::vector<const ast::FuncCall*> aggs;
    auto expr = parseExpression("KUL(a) / GINO(*) + PEHLA(ADHIKTAM(b), 0) YA NYUNTAM(c) HAI KHALI");
    findAggregates(*expr, aggs);
    REQUIRE(aggs.size() == 4);
    REQUIRE(aggKey(*aggs[0]) != aggKey(*aggs[1]));
}
