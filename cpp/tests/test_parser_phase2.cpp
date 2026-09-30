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

TEST_CASE("parser_phase2 BANAO TRIGGER captures the raw body text", "[parser][phase2]") {
    auto stmts = parseScript(
        "BANAO TRIGGER note_it BAAD DAALO PAR accounts SHURU\n"
        "  DAALO MEIN audit_log MAAN (NAYA.id, 'new');\n"
        "  BADLO counters RAKHO n = n + 1;\n"
        "KHATAM;");
    REQUIRE(stmts.size() == 1);
    auto& t = as<ast::CreateTrigger>(stmts[0]);
    CHECK(t.name == "note_it");
    CHECK(t.timing == "BAAD");
    CHECK(t.event == "DAALO");
    CHECK(t.table == "accounts");
    // starts at the first body token, ends right after the last ';' (no leading/trailing whitespace)
    CHECK(t.bodyText == "DAALO MEIN audit_log MAAN (NAYA.id, 'new');\n  BADLO counters RAKHO n = n + 1;");
}

TEST_CASE("parser_phase2 trigger timing and event alternatives", "[parser][phase2]") {
    auto stmts = parseScript(
        "BANAO TRIGGER a PEHLE BADLO PAR t SHURU DIKHAO * SE t; KHATAM;"
        "BANAO TRIGGER b PEHLE MITAO PAR t SHURU DIKHAO * SE t; KHATAM;");
    CHECK(as<ast::CreateTrigger>(stmts[0]).timing == "PEHLE");
    CHECK(as<ast::CreateTrigger>(stmts[0]).event == "BADLO");
    CHECK(as<ast::CreateTrigger>(stmts[1]).event == "MITAO");
}

TEST_CASE("parser_phase2 BANAO TRIGGER errors", "[parser][phase2]") {
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x DAALO PAR t SHURU DIKHAO * SE t; KHATAM;"),
                        ContainsSubstring("BANAO TRIGGER naam ke baad PEHLE ya BAAD expected tha"));
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE SAAF PAR t SHURU DIKHAO * SE t; KHATAM;"),
                        ContainsSubstring("PEHLE/BAAD ke baad DAALO, BADLO ya MITAO expected tha"));
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO t SHURU DIKHAO * SE t; KHATAM;"),
                        ContainsSubstring("'PAR' expected tha"));
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO PAR t SHURU KHATAM;"),
                        ContainsSubstring("TRIGGER ke SHURU...KHATAM ke andar kam se kam ek statement chahiye"));
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO PAR t SHURU DIKHAO * SE t;"),
                        ContainsSubstring("TRIGGER ka SHURU...KHATAM band nahi hua (KHATAM missing)"));
    // a typo inside the body is caught at CREATE time
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO PAR t SHURU DAALO ; KHATAM;"),
                        ContainsSubstring("expected tha"));
    // every statement in the body needs its own ';'
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO PAR t SHURU DIKHAO * SE t KHATAM;"),
                        ContainsSubstring("';' expected tha"));
}

TEST_CASE("parser_phase2 BANAO PROCEDURE with parameters", "[parser][phase2]") {
    auto stmts = parseScript(
        "BANAO PROCEDURE badhao(dept TEXT, pct ANK) SHURU\n"
        "  BADLO emp RAKHO salary = salary + pct JAHAN d = dept;\n"
        "KHATAM;"
        "BANAO PROCEDURE kuch() SHURU DIKHAO * SE t; KHATAM;");
    auto& p = as<ast::CreateProcedure>(stmts[0]);
    CHECK(p.name == "badhao");
    REQUIRE(p.params.size() == 2);
    CHECK(p.params[0].name == "dept");
    CHECK(p.params[0].typeName == "TEXT");
    CHECK(p.params[1].name == "pct");
    CHECK(p.params[1].typeName == "INT");  // ANK is an alias, normalised at parse time
    CHECK(p.bodyText == "BADLO emp RAKHO salary = salary + pct JAHAN d = dept;");
    CHECK(as<ast::CreateProcedure>(stmts[1]).params.empty());
}

TEST_CASE("parser_phase2 BANAO PROCEDURE errors", "[parser][phase2]") {
    REQUIRE_THROWS_WITH(parseScript("BANAO PROCEDURE p(x nonsense) SHURU DIKHAO * SE t; KHATAM;"),
                        ContainsSubstring("Parameter 'x' ka type expected tha (INT/ANK, FLOAT, TEXT/SHABD, BOOL, DATE/TAREEKH, ...)"));
    REQUIRE_THROWS_WITH(parseScript("BANAO PROCEDURE p(x INT SHURU DIKHAO * SE t; KHATAM;"), ContainsSubstring("')' expected tha"));
    REQUIRE_THROWS_WITH(parseScript("BANAO PROCEDURE p() SHURU KHATAM;"),
                        ContainsSubstring("PROCEDURE ke SHURU...KHATAM ke andar kam se kam ek statement chahiye"));
}

TEST_CASE("parser_phase2 SHURU inside a body parses as Begin, like Python", "[parser][phase2]") {
    auto stmts = parseScript("BANAO PROCEDURE p() SHURU SHURU; KHATAM;");
    CHECK(as<ast::CreateProcedure>(stmts[0]).bodyText == "SHURU;");
}
