// cpp/tests/test_repl.cpp -- running text and files, dot-commands and the read-eval-print loop.
#include <catch2/catch_test_macros.hpp>
#include "meradb/repl.h"
#include "meradb/repl_text.h"
#include "golden_shell.h"
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

TEST_CASE("shell dot-commands: .exit, .quit and .nikal leave, in any case, ignoring extra words", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    for (const char* line : {".exit", ".quit", ".nikal", ".EXIT", ".Quit now", ".nikal   please"})
        CHECK_FALSE(repl::handleDotCommand(backend, line, out, kPlain));
    CHECK(out.str().empty());
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell dot-commands: .tables and .schema run the matching statements", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".tables", out, kPlain));
    CHECK(repl::handleDotCommand(backend, ".schema students", out, kPlain));
    CHECK(repl::handleDotCommand(backend, ".SCHEMA a b c", out, kPlain));  // only the first word counts
    CHECK(backend.scripts == std::vector<std::string>{"DIKHAO TABLES;", "BATAO students;", "BATAO a;"});
    CHECK(out.str() == "ok\n\nok\n\nok\n\n");
}

TEST_CASE("shell dot-commands: .schema and .run need an argument, otherwise they are unknown", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".schema", out, kPlain));
    CHECK(repl::handleDotCommand(backend, ".run", out, kPlain));
    CHECK(out.str() ==
          "Ye shell command nahi pata: .schema  (.help dekho)\n"
          "Ye shell command nahi pata: .run  (.help dekho)\n");
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell dot-commands: .run runs a file, cut at the first space", "[shell]") {
    TempDir dir;
    writeBinary(dir.file("one.mdb"), "DIKHAO TABLES;");
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".run " + dir.file("one.mdb") + " ignored words", out, kPlain));
    CHECK(backend.scripts == std::vector<std::string>{"DIKHAO TABLES;"});
    out.str("");
    CHECK(repl::handleDotCommand(backend, ".run nodir/missing.mdb", out, kPlain));
    CHECK(out.str() == "File nahi khuli: [Errno 2] No such file or directory: 'nodir/missing.mdb'\n");
}

TEST_CASE("shell dot-commands: .help prints the whole reference or just the matches", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".help", out, kPlain));
    CHECK(out.str() == golden_shell::get("help_full", false));
    out.str("");
    CHECK(repl::handleDotCommand(backend, ".HELP  join  ", out, kPlain));  // the topic is what follows the command, stripped
    CHECK(out.str() == golden_shell::get("help_join", false));
    out.str("");
    CHECK(repl::handleDotCommand(backend, ".help foreign key", out, kPlain));
    CHECK(out.str() == golden_shell::get("help_two_words", false));
    out.str("");
    CHECK(repl::handleDotCommand(backend, ".help zzzz", out, kPlain));
    CHECK(out.str() == golden_shell::get("help_none", false));
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell dot-commands: anything else is unknown, including .hexdump", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".bogus a b", out, kPlain));
    CHECK(repl::handleDotCommand(backend, ".hexdump students", out, kPlain));  // a ROADMAP exercise, not ours: unknown here too
    CHECK(repl::handleDotCommand(backend, ".", out, kPlain));
    CHECK(out.str() ==
          "Ye shell command nahi pata: .bogus a b  (.help dekho)\n"
          "Ye shell command nahi pata: .hexdump students  (.help dekho)\n"
          "Ye shell command nahi pata: .  (.help dekho)\n");
}
