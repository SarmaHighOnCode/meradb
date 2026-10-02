// cpp/tests/test_repl_input.cpp -- the shell's line sources.
#include <catch2/catch_test_macros.hpp>
#include "meradb/repl.h"
#include <sstream>

using namespace meradb;

namespace {

// Reads everything the source has, as "line|line|...|EOF", and what it wrote as prompts.
std::string drain(const std::string& input, std::string* prompts = nullptr, const std::string& prompt = "> ") {
    std::istringstream in(input);
    std::ostringstream out;
    repl::StreamLineSource source(in, out);
    std::string result;
    for (;;) {
        std::string line;
        const auto status = source.read(prompt, line);
        if (status == sys::ReadStatus::Eof) {
            result += "EOF";
            break;
        }
        REQUIRE(status == sys::ReadStatus::Line);
        result += line + "|";
    }
    if (prompts) *prompts = out.str();
    return result;
}

}  // namespace

TEST_CASE("shell input: a line ends at LF, CRLF or a lone CR", "[shell]") {
    CHECK(drain("a\nb\r\nc\rd\n\ne") == "a|b|c|d||e|EOF");
    CHECK(drain("a\r\n\r\nb\r\n") == "a||b|EOF");
    CHECK(drain("a\r") == "a|EOF");
    CHECK(drain("\r\r\n\n") == "|||EOF");
}

TEST_CASE("shell input: a last line without a terminator still counts", "[shell]") {
    CHECK(drain("x") == "x|EOF");
    CHECK(drain("x\n") == "x|EOF");
    CHECK(drain("") == "EOF");
    CHECK(drain("\n") == "|EOF");
}

TEST_CASE("shell input: the prompt is written before every read, including the one that finds the end", "[shell]") {
    std::string prompts;
    drain("a\nb\n", &prompts, "P ");
    CHECK(prompts == "P P P ");  // a, b, then end of input
}

TEST_CASE("shell input: bytes pass through untouched", "[shell]") {
    CHECK(drain(std::string("caf\xC3\xA9 \xF0\x9F\x98\x80\n")) == "caf\xC3\xA9 \xF0\x9F\x98\x80|EOF");
    CHECK(drain(std::string("a\x1a" "b\n")) == "a\x1a" "b|EOF");  // Ctrl+Z is an ordinary character in a pipe
    CHECK(drain(std::string("\xEF\xBB\xBF" "x\n")) == "\xEF\xBB\xBF" "x|EOF");  // a BOM is not stripped (Python keeps it too)
    const std::string withNul("a\0b\n", 4);
    CHECK(drain(withNul) == std::string("a\0b|EOF", 7));
}

TEST_CASE("shell input: a stream source never reports a pending interrupt", "[shell]") {
    std::istringstream in("x\n");
    std::ostringstream out;
    repl::StreamLineSource source(in, out);
    CHECK_FALSE(source.takePendingInterrupt());
}

TEST_CASE("shell input: a console source reports Ctrl+C that arrived while it was not reading", "[shell]") {
    sys::InterruptGuard guard;
    std::ostringstream out;
    repl::ConsoleLineSource source(out);
    CHECK_FALSE(source.takePendingInterrupt());
    sys::InterruptGuard::trigger();
    CHECK(source.takePendingInterrupt());
    CHECK_FALSE(source.takePendingInterrupt());  // once per Ctrl+C
}
