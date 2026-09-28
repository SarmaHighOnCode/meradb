// cpp/tests/test_planner.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/planner.h"
#include "meradb/storage.h"
#include "meradb/table.h"
#include "test_util.h"
#include <functional>

using namespace meradb;
using namespace meradb::ast;
using meradb_test::TempDir;

namespace {
TableSchema studentsSchema() {
    TableSchema s; s.name = "students";
    Column id; id.name = "id"; id.typeName = "INT"; id.primaryKey = true;
    Column naam; naam.name = "naam"; naam.typeName = "TEXT";
    Column email; email.name = "email"; email.typeName = "TEXT"; email.unique = true;
    Column cid; cid.name = "cid"; cid.typeName = "INT";
    s.columns = {id, naam, email, cid};
    return s;
}

TableSchema coursesSchema() {
    TableSchema s; s.name = "courses";
    Column id; id.name = "id"; id.typeName = "INT"; id.primaryKey = true;
    Column title; title.name = "title"; title.typeName = "TEXT";
    s.columns = {id, title};
    return s;
}

// s = students, c = courses
Scope twoTables() { return Scope({{"s", studentsSchema()}, {"c", coursesSchema()}}); }

std::string messageOf(const std::function<void()>& fn) {
    try { fn(); } catch (const MeraDBError& e) { return e.message(); }
    return "<no error>";
}

const ColumnRef& asRef(const Expr& e) { return dynamic_cast<const ColumnRef&>(e); }
}  // namespace

// ---------------------------------------------------------------- Scope ----

TEST_CASE("Scope resolves an unqualified column to alias.col", "[planner]") {
    Scope scope({{"s", studentsSchema()}});
    REQUIRE(scope.resolve(ColumnRef("naam")) == "s.naam");
    REQUIRE(scope.resolve(ColumnRef("naam", "s")) == "s.naam");
}

TEST_CASE("Scope::resolve reports unknown columns and aliases like Python", "[planner]") {
    Scope one({{"s", studentsSchema()}});
    REQUIRE(messageOf([&] { one.resolve(ColumnRef("nope")); }) ==
            "Table 'students' mein column 'nope' nahi hai");
    REQUIRE(messageOf([&] { one.resolve(ColumnRef("nope", "s")); }) ==
            "Table 'students' mein column 'nope' nahi hai");
    REQUIRE(messageOf([&] { one.resolve(ColumnRef("id", "x")); }) ==
            "'x' is query mein koi table ya alias nahi hai");
    Scope two = twoTables();
    REQUIRE(messageOf([&] { two.resolve(ColumnRef("nope")); }) == "Column 'nope' kisi bhi table mein nahi hai");
}

TEST_CASE("Scope::resolve rejects an ambiguous bare column", "[planner]") {
    Scope scope = twoTables();
    REQUIRE(messageOf([&] { scope.resolve(ColumnRef("id")); }) ==
            "Column 'id' ek se zyada tables mein hai -- s.id ya c.id likho");
    REQUIRE(scope.resolve(ColumnRef("title")) == "c.title");
}

TEST_CASE("Scope rejects the same alias twice", "[planner]") {
    REQUIRE(messageOf([] { Scope({{"s", studentsSchema()}, {"s", coursesSchema()}}); }) ==
            "'s' query mein do baar hai -- alag alias do (jaise: SE students s MILAO students t ...)");
}

TEST_CASE("Scope builds keys, rows, null rows and source indexes", "[planner]") {
    Scope scope = twoTables();
    REQUIRE(scope.keys(1) == std::vector<std::string>{"c.id", "c.title"});
    REQUIRE(scope.allKeys().size() == 6);
    auto r = scope.row(1, {Value(int64_t{7}), Value(std::string("DBMS"))});
    REQUIRE(std::get<int64_t>(r.at("c.id").data) == 7);
    auto n = scope.nullRow(0);
    REQUIRE(n.size() == 4);
    REQUIRE(n.at("s.email").isNull());
    REQUIRE(scope.sourceIndex("c.title") == 1);
    REQUIRE(scope.display("c.title") == "c.title");
    REQUIRE(Scope({{"s", studentsSchema()}}).display("s.naam") == "naam");
}

TEST_CASE("Scope::expandStar labels columns and qualifies only clashing names", "[planner]") {
    Scope scope = twoTables();
    Star all;
    auto cols = scope.expandStar(all);
    REQUIRE(cols.size() == 6);
    REQUIRE(cols[0].first == "s.id");
    REQUIRE(cols[1].first == "naam");
    REQUIRE(cols[4].first == "c.id");
    REQUIRE(cols[4].second.table.value() == "c");
    REQUIRE(cols[4].second.name == "id");

    Star onlyC; onlyC.table = "c";
    REQUIRE(scope.expandStar(onlyC).size() == 2);
    Star bad; bad.table = "x";
    REQUIRE(messageOf([&] { scope.expandStar(bad); }) == "'x' is query mein koi table ya alias nahi hai");
}

// ----------------------------------------------------------------- bind ----

TEST_CASE("bind rewrites every ColumnRef to its qualified key", "[planner]") {
    Scope scope({{"s", studentsSchema()}});
    auto expr = parseExpression("naam");
    auto bound = bind(*expr, scope);
    auto* ref = dynamic_cast<ColumnRef*>(bound.get());
    REQUIRE(ref != nullptr);
    REQUIRE(ref->table.value() == "s");
    REQUIRE(ref->name == "naam");
}

TEST_CASE("bind recurses through every expression shape", "[planner]") {
    Scope scope = twoTables();
    auto expr = parseExpression(
        "AGAR NAHI title HAI KHALI TAB PEHLA(naam, email) WARNA -cid KHATAM = 'x' AUR GINO(*) > 1 YA KUL(s.id) > 2");
    auto bound = bind(*expr, scope);
    // spot-check: no unqualified ColumnRef survives anywhere
    std::function<void(const Expr&)> check = [&](const Expr& e) {
        if (auto* r = dynamic_cast<const ColumnRef*>(&e)) { REQUIRE(r->table.has_value()); return; }
        if (auto* b = dynamic_cast<const BinaryOp*>(&e)) { check(*b->left); check(*b->right); return; }
        if (auto* u = dynamic_cast<const UnaryOp*>(&e)) { check(*u->operand); return; }
        if (auto* i = dynamic_cast<const IsNull*>(&e)) { check(*i->expr); return; }
        if (auto* f = dynamic_cast<const FuncCall*>(&e)) { check(*f->arg); return; }
        if (auto* c = dynamic_cast<const Coalesce*>(&e)) { for (auto& a : c->args) check(*a); return; }
        if (auto* w = dynamic_cast<const CaseWhen*>(&e)) {
            for (auto& [c, v] : w->branches) { check(*c); check(*v); }
            if (w->elseExpr) check(*w->elseExpr);
        }
    };
    check(*bound);
    REQUIRE(bind(static_cast<const Expr*>(nullptr), scope) == nullptr);
}

TEST_CASE("bind leaves a subquery body alone but binds an InSubquery's left side", "[planner]") {
    Scope scope({{"s", studentsSchema()}});
    auto expr = parseExpression("cid MEIN (DIKHAO id SE courses)");
    auto bound = bind(*expr, scope);
    auto* in = dynamic_cast<InSubquery*>(bound.get());
    REQUIRE(in != nullptr);
    REQUIRE(asRef(*in->left).table.value() == "s");
    auto& inner = asRef(*in->subquery->statement->columns[0]);
    REQUIRE_FALSE(inner.table.has_value());
    REQUIRE(inner.name == "id");
}

TEST_CASE("bind fails early on an unknown column", "[planner]") {
    Scope scope({{"s", studentsSchema()}});
    auto expr = parseExpression("umar > 3");
    REQUIRE_THROWS_AS(bind(*expr, scope), ExecutionError);
}

// ------------------------------------------------------- natural join ----

TEST_CASE("naturalJoinCondition ANDs every shared column name", "[planner]") {
    TableSchema enroll; enroll.name = "enroll";
    Column id; id.name = "id"; id.typeName = "INT";
    Column cid; cid.name = "cid"; cid.typeName = "INT";
    enroll.columns = {id, cid};
    Scope scope({{"s", studentsSchema()}, {"e", enroll}});
    auto on = naturalJoinCondition(scope, 1);
    auto parts = conjuncts(*on);
    REQUIRE(parts.size() == 2);
    auto& first = dynamic_cast<const BinaryOp&>(*parts[0]);
    REQUIRE(first.op == "=");
    REQUIRE(asRef(*first.left).table.value() == "s");
    REQUIRE(asRef(*first.left).name == "id");
    REQUIRE(asRef(*first.right).table.value() == "e");
}

TEST_CASE("naturalJoinCondition fails when nothing is shared", "[planner]") {
    TableSchema other; other.name = "other";
    Column z; z.name = "z"; z.typeName = "INT";
    other.columns = {z};
    Scope scope({{"s", studentsSchema()}, {"o", other}});
    REQUIRE(messageOf([&] { naturalJoinCondition(scope, 1); }) ==
            "'o' ka SAMAAN MILAO fail hua -- 's' se koi column naam match nahi karta");
}

// ------------------------------------------------------------ conjuncts ----

TEST_CASE("conjuncts splits an AND-chain into individual predicates", "[planner]") {
    auto expr = parseExpression("a = 1 AUR b = 2 AUR c = 3");
    REQUIRE(conjuncts(*expr).size() == 3);
    auto orExpr = parseExpression("a = 1 YA b = 2");
    REQUIRE(conjuncts(*orExpr).size() == 1);
    REQUIRE(conjuncts(static_cast<const Expr*>(nullptr)).empty());
}

// --------------------------------------------------------- access path ----

TEST_CASE("chooseAccess finds an index lookup on a unique-column equality", "[planner]") {
    TempDir dir;
    std::string path = dir.file("students.tbl");
    HeapFile(path).create();
    auto schema = studentsSchema();
    Table t(schema, path);
    Scope scope({{"s", schema}});

    auto where = bind(*parseExpression("naam = 'Ravi' AUR id = 1"), scope);
    auto access = chooseAccess(t, scope, where.get());
    REQUIRE(access.has_value());
    REQUIRE(access->column == 0);
    REQUIRE(access->columnName == "id");
    REQUIRE(access->describe(t) == "INDEX LOOKUP students PAR id = 1  [hash index, MUKHYA KUNJI]");

    // literal on the left works too, and ANOKHA columns count as unique
    auto flipped = bind(*parseExpression("'a@b.c' = email"), scope);
    auto byEmail = chooseAccess(t, scope, flipped.get());
    REQUIRE(byEmail.has_value());
    REQUIRE(byEmail->describe(t) == "INDEX LOOKUP students PAR email = 'a@b.c'  [hash index, ANOKHA]");
}

TEST_CASE("chooseAccess coerces the constant to the column type", "[planner]") {
    TempDir dir;
    std::string path = dir.file("students.tbl");
    HeapFile(path).create();
    auto schema = studentsSchema();
    Table t(schema, path);
    Scope scope({{"s", schema}});
    auto where = bind(*parseExpression("id = 2.0"), scope);
    auto access = chooseAccess(t, scope, where.get());
    REQUIRE(access.has_value());
    REQUIRE(std::get<int64_t>(access->value.data) == 2);
    // a type mismatch is left for the scan to report
    auto bad = bind(*parseExpression("id = 'x'"), scope);
    REQUIRE_FALSE(chooseAccess(t, scope, bad.get()).has_value());
}

TEST_CASE("chooseAccess coerces constants for DATE and FLOAT unique columns", "[planner]") {
    TableSchema s; s.name = "people";
    Column dob; dob.name = "dob"; dob.typeName = "DATE"; dob.unique = true;
    Column score; score.name = "score"; score.typeName = "FLOAT"; score.unique = true;
    s.columns = {dob, score};
    TempDir dir;
    std::string path = dir.file("people.tbl");
    HeapFile(path).create();
    Table t(s, path);
    Scope scope({{"p", s}});

    auto byDate = bind(*parseExpression("dob = '2005-01-01'"), scope);
    auto access = chooseAccess(t, scope, byDate.get());
    REQUIRE(access.has_value());
    REQUIRE(std::get<Date>(access->value.data) == parseDate("2005-01-01"));
    REQUIRE(access->describe(t) == "INDEX LOOKUP people PAR dob = 2005-01-01  [hash index, ANOKHA]");
    auto badDate = bind(*parseExpression("dob = 'kal'"), scope);
    REQUIRE_FALSE(chooseAccess(t, scope, badDate.get()).has_value());

    auto byScore = bind(*parseExpression("score = 3"), scope);
    auto s3 = chooseAccess(t, scope, byScore.get());
    REQUIRE(s3.has_value());
    REQUIRE(std::get<double>(s3->value.data) == 3.0);
    REQUIRE(s3->describe(t) == "INDEX LOOKUP people PAR score = 3.0  [hash index, ANOKHA]");
}

TEST_CASE("chooseAccess returns nullopt when no usable unique-column equality exists", "[planner]") {
    TempDir dir;
    std::string path = dir.file("students.tbl");
    HeapFile(path).create();
    auto schema = studentsSchema();
    Table t(schema, path);
    Scope scope({{"s", schema}});
    for (const char* text : {"naam = 'Ravi'", "id > 1", "id = 1 YA id = 2", "id = KHALI", "id = cid"}) {
        auto where = bind(*parseExpression(text), scope);
        REQUIRE_FALSE(chooseAccess(t, scope, where.get()).has_value());
    }
    REQUIRE_FALSE(chooseAccess(t, scope, nullptr).has_value());
    // only a BOUND reference counts (the engine always binds first)
    auto unbound = parseExpression("id = 1");
    REQUIRE_FALSE(chooseAccess(t, scope, unbound.get()).has_value());
}

TEST_CASE("chooseAccess never uses an index for a view", "[planner]") {
    auto schema = studentsSchema();
    MaterializedTable view(schema, {});
    Scope scope({{"s", schema}});
    auto where = bind(*parseExpression("id = 1"), scope);
    REQUIRE_FALSE(chooseAccess(view, scope, where.get()).has_value());
}

// -------------------------------------------------------- join strategy ----

TEST_CASE("chooseJoin picks hash-join keys from an equality across sources", "[planner]") {
    Scope scope = twoTables();
    auto on = bind(*parseExpression("c.id = s.cid AUR c.title = 'x'"), scope);
    auto keys = chooseJoin(on.get(), scope, 1);
    REQUIRE(keys.has_value());
    REQUIRE(keys->first == "s.cid");
    REQUIRE(keys->second == "c.id");

    auto nonEqui = bind(*parseExpression("s.cid < c.id"), scope);
    REQUIRE_FALSE(chooseJoin(nonEqui.get(), scope, 1).has_value());
    auto sameSide = bind(*parseExpression("c.id = c.id"), scope);
    REQUIRE_FALSE(chooseJoin(sameSide.get(), scope, 1).has_value());
    REQUIRE_FALSE(chooseJoin(nullptr, scope, 1).has_value());
}

TEST_CASE("checkJoinCondition rejects a table joined later", "[planner]") {
    TableSchema third; third.name = "third";
    Column k; k.name = "k"; k.typeName = "INT";
    third.columns = {k};
    Scope scope({{"s", studentsSchema()}, {"c", coursesSchema()}, {"t", third}});
    auto ok = bind(*parseExpression("s.cid = c.id"), scope);
    REQUIRE_NOTHROW(checkJoinCondition(ok.get(), scope, 1));
    REQUIRE_NOTHROW(checkJoinCondition(nullptr, scope, 1));
    auto early = bind(*parseExpression("s.cid = c.id AUR PEHLA(t.k, 0) = 1"), scope);
    REQUIRE(messageOf([&] { checkJoinCondition(early.get(), scope, 1); }) ==
            "PAR mein 't.k' abhi use nahi ho sakta -- wo table baad mein MILAO hoti hai");
    REQUIRE_NOTHROW(checkJoinCondition(early.get(), scope, 2));
}
