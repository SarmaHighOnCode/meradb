// cpp/tests/test_ast.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/ast.h"
#include "meradb/ast_util.h"

using namespace meradb::ast;

TEST_CASE("expression nodes construct and nest", "[ast]") {
    auto left = std::make_unique<Literal>(Value(int64_t{1}));
    auto right = std::make_unique<Literal>(Value(int64_t{2}));
    BinaryOp add("+", std::move(left), std::move(right));
    REQUIRE(add.op == "+");
    REQUIRE(std::get<int64_t>(static_cast<Literal*>(add.left.get())->value.data) == 1);
}

TEST_CASE("ColumnRef defaults table to nullopt", "[ast]") {
    ColumnRef ref("naam");
    REQUIRE(ref.name == "naam");
    REQUIRE_FALSE(ref.table.has_value());
}

TEST_CASE("Select statement holds joins and order items", "[ast]") {
    Select sel;
    sel.table = "students";
    sel.columns.push_back(std::make_unique<Star>());
    Join j;
    j.table = "courses"; j.alias = "c"; j.kind = "LEFT";
    sel.joins.push_back(std::move(j));
    REQUIRE(sel.joins.size() == 1);
    REQUIRE(sel.joins[0].kind == "LEFT");
}

TEST_CASE("CreateTable holds ColumnDef list and composite constraints", "[ast]") {
    CreateTable ct;
    ct.name = "students";
    ColumnDef col;
    col.name = "id"; col.typeName = "INT"; col.primaryKey = true;
    ct.columns.push_back(col);
    ct.compositeUnique.push_back({"student_id", "course_id"});
    REQUIRE(ct.columns[0].primaryKey);
    REQUIRE(ct.compositeUnique[0].size() == 2);
}

TEST_CASE("ast astClassName matches Python class names", "[ast][phase2]") {
    using namespace meradb::ast;
    CHECK(meradb::astClassName(CreateDatabase()) == "CreateDatabase");
    CHECK(meradb::astClassName(DropDatabase()) == "DropDatabase");
    CHECK(meradb::astClassName(UseDatabase()) == "UseDatabase");
    CHECK(meradb::astClassName(ShowTables()) == "ShowTables");
    CHECK(meradb::astClassName(Describe()) == "Describe");
    CHECK(meradb::astClassName(CreateView()) == "CreateView");
    CHECK(meradb::astClassName(DropView()) == "DropView");
    CHECK(meradb::astClassName(ShowViews()) == "ShowViews");
    CHECK(meradb::astClassName(CreateTable()) == "CreateTable");
    CHECK(meradb::astClassName(AlterAddComposite()) == "AlterAddComposite");
    CHECK(meradb::astClassName(DropTable()) == "DropTable");
    CHECK(meradb::astClassName(AlterAddColumn()) == "AlterAddColumn");
    CHECK(meradb::astClassName(AlterDropColumn()) == "AlterDropColumn");
    CHECK(meradb::astClassName(RenameTable()) == "RenameTable");
    CHECK(meradb::astClassName(RenameColumn()) == "RenameColumn");
    CHECK(meradb::astClassName(TruncateTable()) == "TruncateTable");
    CHECK(meradb::astClassName(CompactTable()) == "CompactTable");
    CHECK(meradb::astClassName(Begin()) == "Begin");
    CHECK(meradb::astClassName(Commit()) == "Commit");
    CHECK(meradb::astClassName(Rollback()) == "Rollback");
    CHECK(meradb::astClassName(Explain()) == "Explain");
    CHECK(meradb::astClassName(Insert()) == "Insert");
    CHECK(meradb::astClassName(Select()) == "Select");
    CHECK(meradb::astClassName(SetOp()) == "SetOp");
    CHECK(meradb::astClassName(Update()) == "Update");
    CHECK(meradb::astClassName(Delete()) == "Delete");
    CHECK(meradb::astClassName(CreateUser()) == "CreateUser");
    CHECK(meradb::astClassName(DropUser()) == "DropUser");
    CHECK(meradb::astClassName(Grant()) == "Grant");
    CHECK(meradb::astClassName(Revoke()) == "Revoke");
    CHECK(meradb::astClassName(CreateTrigger()) == "CreateTrigger");
    CHECK(meradb::astClassName(DropTrigger()) == "DropTrigger");
    CHECK(meradb::astClassName(CreateProcedure()) == "CreateProcedure");
    CHECK(meradb::astClassName(DropProcedure()) == "DropProcedure");
    CHECK(meradb::astClassName(CallProcedure()) == "CallProcedure");
}

TEST_CASE("ast Phase 2 statements hold their fields", "[ast][phase2]") {
    Grant g;
    g.privileges = {"DIKHAO", "DAALO"};
    g.table = "students";
    g.user = "ravi";
    REQUIRE(g.privileges.size() == 2);

    CreateProcedure p;
    p.name = "raise";
    p.params.push_back({"pct", "INT"});
    p.bodyText = "BADLO t RAKHO x = 1;";
    REQUIRE(p.params[0].typeName == "INT");
}
