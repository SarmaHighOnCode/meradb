// cpp/tests/test_repl.cpp -- running text and files, dot-commands and the read-eval-print loop.
#include <catch2/catch_test_macros.hpp>
#include "meradb/repl.h"
#include "meradb/repl_text.h"
#include "repl_test_util.h"
#include "test_util.h"
#include <fstream>
#include <sstream>

using namespace meradb;
using meradb_test::FakeBackend;
using meradb_test::TempDir;

namespace {

void writeBinary(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

std::vector<Result> oneMessage(const std::string& text) {
    Result r;
    r.message = text;
    return {r};
}

const term::Style kPlain = term::Style::none();

}  // namespace

TEST_CASE("shell runText prints each result followed by a blank line", "[shell]") {
    FakeBackend backend;
    backend.onRun = [](const std::string&) {
        Result table;
        table.columns = {"a"};
        table.rows = {{Value(int64_t(1))}};
        table.message = "1 row(s)";
        Result error;
        error.error = "[Execution Galti] boom";
        Result silent;  // nothing to print, but Python still prints the blank line
        return std::vector<Result>{table, error, silent};
    };
    std::ostringstream out;
    CHECK_FALSE(repl::runText(backend, "x;", out, kPlain));
    CHECK(out.str() == "+---+\n| a |\n+---+\n| 1 |\n+---+\n1 row(s)\n\n[Execution Galti] boom\n\n\n");
    CHECK(backend.scripts == std::vector<std::string>{"x;"});
}

TEST_CASE("shell runText is true and silent for an empty script", "[shell]") {
    FakeBackend backend;
    backend.onRun = [](const std::string&) { return std::vector<Result>{}; };
    std::ostringstream out;
    CHECK(repl::runText(backend, ";\n", out, kPlain));
    CHECK(out.str().empty());
}

TEST_CASE("shell runText colours when the style says so", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::runText(backend, "x;", out, term::Style::colored()));
    CHECK(out.str() == "\x1b[32mok\x1b[0m\n\n");
}

TEST_CASE("shell runText prints a dropped connection and carries on", "[shell]") {
    FakeBackend backend;
    int calls = 0;
    backend.onRun = [&](const std::string&) -> std::vector<Result> {
        if (++calls == 1) throw ConnectionFailed("Server ne connection band kar diya");
        return oneMessage("back");
    };
    std::ostringstream out;
    CHECK_FALSE(repl::runText(backend, "a;", out, kPlain));
    CHECK(out.str() == "[Connection Galti] Server ne connection band kar diya\n");  // str(e), on stdout
    out.str("");
    CHECK(repl::runText(backend, "b;", out, kPlain));
    CHECK(out.str() == "back\n\n");
}

TEST_CASE("shell runFile reads UTF-8 with a BOM and any newline style", "[shell]") {
    TempDir dir;
    writeBinary(dir.file("s.mdb"), "\xEF\xBB\xBF" "A\r\nB;\rC;");
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::runFile(backend, dir.file("s.mdb"), out, kPlain));
    CHECK(backend.scripts == std::vector<std::string>{"A\nB;\nC;"});
}

TEST_CASE("shell runFile reports a missing file with Python's wording", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK_FALSE(repl::runFile(backend, "definitely_missing_script.mdb", out, kPlain));
    CHECK(out.str() == "File nahi khuli: [Errno 2] No such file or directory: 'definitely_missing_script.mdb'\n");
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell runFile survives a dropped connection and says so on stdout", "[shell]") {
    TempDir dir;
    writeBinary(dir.file("s.mdb"), "X;");
    FakeBackend backend;
    backend.onRun = [](const std::string&) -> std::vector<Result> { throw ConnectionFailed("Server ne connection band kar diya"); };
    std::ostringstream out;
    CHECK_FALSE(repl::runFile(backend, dir.file("s.mdb"), out, kPlain));
    CHECK(out.str() == "[Connection Galti] Server ne connection band kar diya\n");
}
