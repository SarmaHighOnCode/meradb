// cpp/tests/test_parser_ddl.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/parser.h"

using namespace meradb;
using namespace meradb::ast;

TEST_CASE("parseScript parses CREATE TABLE with constraints", "[parser][ddl]") {
    auto stmts = parseScript(
        "BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT ZAROORI, "
        "umar ANK SHART (umar >= 0 AUR umar < 150), active BOOL WARNA SACH);");
    REQUIRE(stmts.size() == 1);
    auto* ct = dynamic_cast<CreateTable*>(stmts[0].get());
    REQUIRE(ct != nullptr);
    REQUIRE(ct->name == "students");
    REQUIRE(ct->columns.size() == 4);
    REQUIRE(ct->columns[0].primaryKey);
    REQUIRE(ct->columns[1].notNull);
    REQUIRE(ct->columns[2].check.has_value());
    REQUIRE(ct->columns[3].defaultValue.has_value());
}

TEST_CASE("parseScript parses composite UNIQUE constraint", "[parser][ddl]") {
    auto stmts = parseScript(
        "BANAO TABLE enroll (student_id INT, course_id INT, ANOKHA (student_id, course_id));");
    auto* ct = dynamic_cast<CreateTable*>(stmts[0].get());
    REQUIRE(ct->compositeUnique.size() == 1);
    REQUIRE(ct->compositeUnique[0] == std::vector<std::string>{"student_id", "course_id"});
}

TEST_CASE("parseScript parses composite MUKHYA KUNJI constraint", "[parser][ddl]") {
    auto stmts = parseScript(
        "BANAO TABLE enroll (student_id INT, course_id INT, MUKHYA KUNJI (student_id, course_id));");
    auto* ct = dynamic_cast<CreateTable*>(stmts[0].get());
    REQUIRE(ct->compositePk.has_value());
    REQUIRE(*ct->compositePk == std::vector<std::string>{"student_id", "course_id"});
}

TEST_CASE("parseScript parses ALTER TABLE ADD/DROP COLUMN", "[parser][ddl]") {
    auto stmts = parseScript("SUDHARO TABLE students JODO COLUMN city TEXT;");
    REQUIRE(dynamic_cast<AlterAddColumn*>(stmts[0].get()) != nullptr);
    auto stmts2 = parseScript("SUDHARO TABLE students HATAO COLUMN city;");
    REQUIRE(dynamic_cast<AlterDropColumn*>(stmts2[0].get()) != nullptr);
}

TEST_CASE("parseScript parses ALTER TABLE ADD composite constraint", "[parser][ddl]") {
    auto stmts = parseScript("SUDHARO TABLE enroll JODO ANOKHA (student_id, course_id);");
    auto* aac = dynamic_cast<AlterAddComposite*>(stmts[0].get());
    REQUIRE(aac != nullptr);
    REQUIRE(aac->kind == "ANOKHA");
    auto stmts2 = parseScript("SUDHARO TABLE enroll JODO MUKHYA KUNJI (student_id, course_id);");
    auto* aac2 = dynamic_cast<AlterAddComposite*>(stmts2[0].get());
    REQUIRE(aac2 != nullptr);
    REQUIRE(aac2->kind == "MUKHYA");
}

TEST_CASE("parseScript parses RENAME TABLE and RENAME COLUMN", "[parser][ddl]") {
    auto stmts = parseScript("SUDHARO TABLE students NAYA_NAAM learners;");
    REQUIRE(dynamic_cast<RenameTable*>(stmts[0].get())->newName == "learners");
    auto stmts2 = parseScript("SUDHARO TABLE students COLUMN naam NAYA_NAAM full_name;");
    auto* rc = dynamic_cast<RenameColumn*>(stmts2[0].get());
    REQUIRE(rc->column == "naam");
    REQUIRE(rc->newName == "full_name");
}

TEST_CASE("parseScript captures CREATE VIEW body as raw source text", "[parser][ddl]") {
    auto stmts = parseScript("BANAO VIEW toppers KAHO DIKHAO naam SE students JAHAN cgpa > 9;");
    auto* cv = dynamic_cast<CreateView*>(stmts[0].get());
    REQUIRE(cv != nullptr);
    REQUIRE(cv->name == "toppers");
    REQUIRE(cv->queryText.find("DIKHAO") == 0);
}

TEST_CASE("parseScript parses DROP TABLE/VIEW/DATABASE and TRUNCATE/VACUUM", "[parser][ddl]") {
    REQUIRE(dynamic_cast<DropTable*>(parseScript("HATAO TABLE students;")[0].get()) != nullptr);
    REQUIRE(dynamic_cast<DropView*>(parseScript("HATAO VIEW toppers;")[0].get()) != nullptr);
    REQUIRE(dynamic_cast<DropDatabase*>(parseScript("HATAO DATABASE d1;")[0].get()) != nullptr);
    REQUIRE(dynamic_cast<TruncateTable*>(parseScript("SAAF TABLE students;")[0].get()) != nullptr);
    REQUIRE(dynamic_cast<CompactTable*>(parseScript("SIKODO TABLE students;")[0].get()) != nullptr);
}

TEST_CASE("parseScript parses BATAO with and without TABLE keyword", "[parser][ddl]") {
    auto* d1 = dynamic_cast<Describe*>(parseScript("BATAO students;")[0].get());
    REQUIRE(d1 != nullptr);
    REQUIRE(d1->table == "students");
    auto* d2 = dynamic_cast<Describe*>(parseScript("BATAO TABLE students;")[0].get());
    REQUIRE(d2 != nullptr);
    REQUIRE(d2->table == "students");
}

TEST_CASE("parseScript parses multiple ;-separated statements", "[parser][ddl]") {
    auto stmts = parseScript("BANAO DATABASE d1; ISTEMAL d1; BATAO students;");
    REQUIRE(stmts.size() == 3);
    REQUIRE(dynamic_cast<CreateDatabase*>(stmts[0].get()) != nullptr);
    REQUIRE(dynamic_cast<UseDatabase*>(stmts[1].get()) != nullptr);
    REQUIRE(dynamic_cast<Describe*>(stmts[2].get()) != nullptr);
}
