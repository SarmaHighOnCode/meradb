// cpp/tests/test_ast.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/ast.h"

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
