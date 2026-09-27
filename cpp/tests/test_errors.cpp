// cpp/tests/test_errors.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"

using namespace meradb;

TEST_CASE("MeraDBError formats stage and message", "[errors]") {
    ExecutionError e("naam column not found");
    REQUIRE(e.stage() == "Execution");
    REQUIRE(e.message() == "naam column not found");
    REQUIRE(std::string(e.what()) == "[Execution Galti] naam column not found");
}

TEST_CASE("Each error subclass reports its own stage", "[errors]") {
    REQUIRE(TokenizerError("x").stage() == "Tokenizer");
    REQUIRE(ParseError("x").stage() == "Parser");
    REQUIRE(StorageError("x").stage() == "Storage");
    REQUIRE(ConnectionFailed("x").stage() == "Connection");
    REQUIRE(ServerUnavailable("x").stage() == "Connection");
}

TEST_CASE("ServerUnavailable is-a ConnectionFailed is-a MeraDBError", "[errors]") {
    ServerUnavailable e("no server");
    const MeraDBError& base = e;
    REQUIRE(base.stage() == "Connection");
    const ConnectionFailed& mid = e;
    REQUIRE(mid.message() == "no server");
}
