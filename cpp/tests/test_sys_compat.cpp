// cpp/tests/test_sys_compat.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/sys_compat.h"
#include <cctype>

using namespace meradb;

TEST_CASE("sys_compat getEnv distinguishes unset from set", "[sys_compat]") {
    REQUIRE_FALSE(sys::getEnv("MERADB_SURELY_NOT_SET_VARIABLE_XYZ").has_value());
    // PATH exists on every platform CI/dev machines use (Windows lookup is case-insensitive).
    auto path = sys::getEnv("PATH");
    REQUIRE(path.has_value());
    REQUIRE_FALSE(path->empty());
}

TEST_CASE("sys_compat processId is positive", "[sys_compat]") {
    REQUIRE(sys::processId() > 0);
}

TEST_CASE("sys_compat timestamps have Python's isoformat shape", "[sys_compat]") {
    std::string iso = sys::localIsoSeconds();  // 2026-09-29T14:03:07
    REQUIRE(iso.size() == 19);
    REQUIRE(iso[4] == '-');
    REQUIRE(iso[7] == '-');
    REQUIRE(iso[10] == 'T');
    REQUIRE(iso[13] == ':');
    REQUIRE(iso[16] == ':');
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u})
        REQUIRE(std::isdigit(static_cast<unsigned char>(iso[i])));

    std::string stamp = sys::localLogStamp();  // 2026-09-29 14:03:07
    REQUIRE(stamp.size() == 19);
    REQUIRE(stamp[10] == ' ');
    REQUIRE(stamp.substr(0, 10) == iso.substr(0, 10));
}

TEST_CASE("sys_compat randomBytes returns fresh bytes each call", "[sys_compat]") {
    auto a = sys::randomBytes(16);
    auto b = sys::randomBytes(16);
    REQUIRE(a.size() == 16);
    REQUIRE(b.size() == 16);
    REQUIRE(a != b);  // 2^-128 chance of a false failure
    REQUIRE(sys::randomBytes(0).empty());
}

TEST_CASE("sys_compat homeDir is never empty", "[sys_compat]") {
    REQUIRE_FALSE(sys::homeDir().empty());
}
