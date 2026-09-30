// cpp/tests/test_parser_phase2.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "meradb/errors.h"
#include "meradb/parser.h"

using namespace meradb;
using Catch::Matchers::ContainsSubstring;

namespace {
template <typename T>
const T& as(const std::unique_ptr<ast::Statement>& s) {
    auto* p = dynamic_cast<const T*>(s.get());
    REQUIRE(p != nullptr);
    return *p;
}
}  // namespace

TEST_CASE("parser_phase2 BANAO USER and HATAO USER", "[parser][phase2]") {
    auto stmts = parseScript("BANAO USER ravi GUPT 'se cret'; HATAO USER ravi;");
    REQUIRE(stmts.size() == 2);
    auto& create = as<ast::CreateUser>(stmts[0]);
    CHECK(create.name == "ravi");
    CHECK(create.password == "se cret");
    CHECK(as<ast::DropUser>(stmts[1]).name == "ravi");
}

TEST_CASE("parser_phase2 BANAO USER errors use Python's wording", "[parser][phase2]") {
    REQUIRE_THROWS_WITH(parseScript("BANAO USER ravi;"),
                        "[Parser Galti] 'GUPT' expected tha, par ';' mila (line 1, col 16)");
    REQUIRE_THROWS_WITH(parseScript("BANAO USER ravi GUPT secret;"),
                        "[Parser Galti] password (quotes ke andar ek string) expected tha, par 'secret' mila (line 1, col 22)");
    REQUIRE_THROWS_WITH(parseScript("BANAO USER GUPT 'x';"), ContainsSubstring("user ka naam expected tha"));
}

TEST_CASE("parser_phase2 GRANT and REVOKE", "[parser][phase2]") {
    auto stmts = parseScript(
        "ADHIKAR DO DIKHAO, DAALO PAR students KO ravi;"
        "ADHIKAR DO SAB PAR students KO asha;"
        "ADHIKAR WAPAS DAALO PAR students SE ravi;");
    REQUIRE(stmts.size() == 3);
    auto& grant = as<ast::Grant>(stmts[0]);
    CHECK(grant.privileges == std::vector<std::string>{"DIKHAO", "DAALO"});
    CHECK(grant.table == "students");
    CHECK(grant.user == "ravi");
    CHECK(as<ast::Grant>(stmts[1]).privileges == std::vector<std::string>{"DIKHAO", "DAALO", "BADLO", "MITAO"});
    auto& revoke = as<ast::Revoke>(stmts[2]);
    CHECK(revoke.privileges == std::vector<std::string>{"DAALO"});
    CHECK(revoke.user == "ravi");
}

TEST_CASE("parser_phase2 ADHIKAR errors", "[parser][phase2]") {
    REQUIRE_THROWS_WITH(parseScript("ADHIKAR;"),
                        "[Parser Galti] ADHIKAR ke baad DO (grant) ya WAPAS (revoke) expected tha, par ';' mila (line 1, col 8)");
    REQUIRE_THROWS_WITH(parseScript("ADHIKAR DO CHALAO PAR t KO u;"),
                        "[Parser Galti] Adhikar ka naam expected tha (DIKHAO, DAALO, BADLO, MITAO, ya SAB), par 'CHALAO' mila (line 1, col 12)");
    REQUIRE_THROWS_WITH(parseScript("ADHIKAR DO DIKHAO PAR t SE u;"), ContainsSubstring("'KO' expected tha"));
    REQUIRE_THROWS_WITH(parseScript("ADHIKAR WAPAS DIKHAO PAR t KO u;"), ContainsSubstring("'SE' expected tha"));
}

TEST_CASE("parser_phase2 HATAO TRIGGER and HATAO PROCEDURE", "[parser][phase2]") {
    auto stmts = parseScript("HATAO TRIGGER audit_it; HATAO PROCEDURE badhao;");
    CHECK(as<ast::DropTrigger>(stmts[0]).name == "audit_it");
    CHECK(as<ast::DropProcedure>(stmts[1]).name == "badhao");
}

TEST_CASE("parser_phase2 CHALAO parses arguments as expressions", "[parser][phase2]") {
    auto stmts = parseScript("CHALAO badhao(10, 'CS', 2 + 3); CHALAO nothing();");
    auto& call = as<ast::CallProcedure>(stmts[0]);
    CHECK(call.name == "badhao");
    REQUIRE(call.args.size() == 3);
    CHECK(dynamic_cast<const ast::Literal*>(call.args[0].get()) != nullptr);
    CHECK(dynamic_cast<const ast::BinaryOp*>(call.args[2].get()) != nullptr);
    CHECK(as<ast::CallProcedure>(stmts[1]).args.empty());
    REQUIRE_THROWS_WITH(parseScript("CHALAO badhao 10;"), ContainsSubstring("'(' expected tha"));
}
