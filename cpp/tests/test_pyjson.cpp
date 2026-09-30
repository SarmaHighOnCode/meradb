// cpp/tests/test_pyjson.cpp
//
// Expected strings were produced by Python's json.dumps (see the task text);
// they are the wire format both engines must speak.
#include <catch2/catch_test_macros.hpp>
#include "meradb/pyjson.h"
#include <cmath>
#include <limits>

using namespace meradb::pyjson;

TEST_CASE("pyjson dump matches json.dumps for a Result message", "[pyjson]") {
    // json.dumps of a real one-row result: unicode, an astral character, a quote, a backslash-free DEL
    Json cell_text = std::string("h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80 \\\" \x7f");
    Json date = Json::object();
    date["$date"] = "2024-01-05";
    Json row = Json::array({1.0, date, cell_text, true, nullptr});
    Json result = Json::object();
    result["columns"] = Json::array({"a", "d", "t", "b", "n"});
    result["rows"] = Json::array({row});
    result["message"] = "1 row(s)";
    result["error"] = "";
    Json message = Json::object();
    message["ok"] = true;
    message["results"] = Json::array({result});

    CHECK(dump(message) ==
          "{\"ok\": true, \"results\": [{\"columns\": [\"a\", \"d\", \"t\", \"b\", \"n\"], \"rows\": "
          "[[1.0, {\"$date\": \"2024-01-05\"}, \"h\\u00e9llo \\u4e16\\u754c \\ud83d\\ude00 \\\\\\\" \\u007f\", true, null]], "
          "\"message\": \"1 row(s)\", \"error\": \"\"}]}");
}

TEST_CASE("pyjson round-trips Python's own output byte for byte", "[pyjson]") {
    // json.dumps([1e20, 1e-5, -0.0, 0.1, 1e16, 123456789.125, inf, -inf, nan, 2**64-ish, control chars, {}, [], {"k": []}])
    const std::string python =
        "[1e+20, 1e-05, -0.0, 0.1, 1e+16, 123456789.125, Infinity, -Infinity, NaN, 12345678901234567890, "
        "\"a\\tb\\n\\u0001\", {}, [], {\"k\": []}]";
    Json parsed = parse(python);
    REQUIRE(parsed.is_array());
    CHECK(parsed[0].is_number_float());
    CHECK(std::isinf(parsed[6].get<double>()));
    CHECK(parsed[6].get<double>() > 0);
    CHECK(parsed[7].get<double>() < 0);
    CHECK(std::isnan(parsed[8].get<double>()));
    CHECK(parsed[9].is_number_unsigned());
    CHECK(dump(parsed) == python);
}

TEST_CASE("pyjson keeps object key order", "[pyjson]") {
    Json o = Json::object();
    o["z"] = 1;
    o["a"] = Json::object({{"y", 2}, {"b", 3}});
    CHECK(dump(o) == "{\"z\": 1, \"a\": {\"y\": 2, \"b\": 3}}");
    CHECK(dump(parse("{\"z\": 1, \"a\": 2}")) == "{\"z\": 1, \"a\": 2}");
}

TEST_CASE("pyjson bare constants inside strings are left alone", "[pyjson]") {
    Json parsed = parse("[\"Infinity\", \"say NaN\", \"-Infinity\"]");
    CHECK(parsed[0] == "Infinity");
    CHECK(parsed[1] == "say NaN");
    CHECK(parsed[2] == "-Infinity");
    CHECK(dump(parsed) == "[\"Infinity\", \"say NaN\", \"-Infinity\"]");
}

TEST_CASE("pyjson writes non-finite doubles as bare tokens", "[pyjson]") {
    Json values = Json::array({std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()});
    CHECK(dump(values) == "[Infinity, -Infinity, NaN]");
}

TEST_CASE("pyjson dump escapes invalid UTF-8 as U+FFFD instead of failing", "[pyjson]") {
    CHECK(dump(Json(std::string("a\xC3(b"))) == "\"a\\ufffd(b\"");
}

TEST_CASE("pyjson rejects malformed and over-nested text", "[pyjson]") {
    CHECK_THROWS_AS(parse(""), ParseFailure);
    CHECK_THROWS_AS(parse("{"), ParseFailure);
    CHECK_THROWS_AS(parse("[1] x"), ParseFailure);
    CHECK_THROWS_AS(parse("{'a': 1}"), ParseFailure);
    const std::string deep = std::string(kMaxParseDepth + 1, '[') + std::string(kMaxParseDepth + 1, ']');
    CHECK_THROWS_AS(parse(deep), ParseFailure);
    const std::string fine = std::string(kMaxParseDepth, '[') + std::string(kMaxParseDepth, ']');
    CHECK_NOTHROW(parse(fine));
}

TEST_CASE("pyjson isValidUtf8 is strict", "[pyjson]") {
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("h\xC3\xA9llo \xE4\xB8\x96 \xF0\x9F\x98\x80"));
    CHECK_FALSE(isValidUtf8("\xC3("));          // bad continuation
    CHECK_FALSE(isValidUtf8("\xC0\x80"));       // overlong NUL
    CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));   // a UTF-16 surrogate
    CHECK_FALSE(isValidUtf8("\xE4\xB8"));       // truncated
    CHECK_FALSE(isValidUtf8("\xF4\x90\x80\x80"));  // above U+10FFFF
    CHECK_FALSE(isValidUtf8("\xFF"));
}
