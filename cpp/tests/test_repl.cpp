// cpp/tests/test_repl.cpp -- running text and files, dot-commands and the read-eval-print loop.
#include <catch2/catch_test_macros.hpp>
#include "meradb/datatypes.h"
#include "meradb/repl.h"
#include "meradb/repl_text.h"
#include "golden_shell.h"
#include "golden_transcripts.h"
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

TEST_CASE("shell runFile formats the file name like Python's repr() and picks the errno like open()", "[shell]") {
    FakeBackend backend;
    const auto message = [&](const std::string& path) {
        std::ostringstream out;
        CHECK_FALSE(repl::runFile(backend, path, out, kPlain));
        return out.str();
    };
    CHECK(message("'x.mdb'") == "File nahi khuli: [Errno 2] No such file or directory: \"'x.mdb'\"\n");
    CHECK(message("n\xC3\xA5me.mdb") == "File nahi khuli: [Errno 2] No such file or directory: 'n\xC3\xA5me.mdb'\n");
    CHECK(message("my file.mdb") == "File nahi khuli: [Errno 2] No such file or directory: 'my file.mdb'\n");
    CHECK(message("no_such_dir\\x.mdb") == "File nahi khuli: [Errno 2] No such file or directory: 'no_such_dir\\\\x.mdb'\n");
#ifdef _WIN32
    CHECK(message("a?b.mdb") == "File nahi khuli: [Errno 22] Invalid argument: 'a?b.mdb'\n");
    CHECK(message("\"x.mdb\"") == "File nahi khuli: [Errno 22] Invalid argument: '\"x.mdb\"'\n");
#endif
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell runFile on a directory fails like Python's open() does", "[shell]") {
    TempDir dir;
    FakeBackend backend;
    std::ostringstream out;
    const std::string path = dir.path().string();
    CHECK_FALSE(repl::runFile(backend, path, out, kPlain));
#ifdef _WIN32
    CHECK(out.str() == "File nahi khuli: [Errno 13] Permission denied: " + pyRepr(path) + "\n");
#else
    CHECK(out.str() == "File nahi khuli: [Errno 21] Is a directory: " + pyRepr(path) + "\n");
#endif
    CHECK(backend.scripts.empty());
}

#ifdef _WIN32
// Python on Windows: open('file.mdb\\') and open('file.mdb/') both give [Errno 22] Invalid argument (not 20).
TEST_CASE("shell runFile on a file name with a trailing separator says EINVAL like Python on Windows", "[shell]") {
    TempDir dir;
    writeBinary(dir.file("s.mdb"), "X;");
    FakeBackend backend;
    for (const char* separator : {"\\", "/", "\\\\"}) {
        std::ostringstream out;
        const std::string path = dir.file("s.mdb") + separator;
        CHECK_FALSE(repl::runFile(backend, path, out, kPlain));
        CHECK(out.str() == "File nahi khuli: [Errno 22] Invalid argument: " + pyRepr(path) + "\n");
    }
    CHECK(backend.scripts.empty());
}
#endif

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

// A scripted terminal: hands out lines, then end of input (or Ctrl+C at the prompt).
namespace {

class ScriptedSource : public repl::LineSource {
public:
    ScriptedSource(std::vector<std::string> lines, std::ostream& out) : lines_(std::move(lines)), out_(out) {}

    sys::ReadStatus read(const std::string& prompt, std::string& line) override {
        out_ << prompt;
        ++reads;
        if (next_ >= lines_.size()) {
            if (failAtEnd) return sys::ReadStatus::Failed;
            return interruptAtEnd ? sys::ReadStatus::Interrupted : sys::ReadStatus::Eof;
        }
        line = lines_[next_++];
        return sys::ReadStatus::Line;
    }
    bool takePendingInterrupt() override {
        const bool was = pending;
        pending = false;
        return was;
    }

    bool pending = false;         // "Ctrl+C arrived while a statement was running"
    bool interruptAtEnd = false;  // the end of the script is a Ctrl+C at the prompt
    bool failAtEnd = false;       // the end of the script is a terminal read error
    int reads = 0;

private:
    std::vector<std::string> lines_;
    std::size_t next_ = 0;
    std::ostream& out_;
};

std::string bannerFor(const std::string& where) {
    std::ostringstream out;
    repl::printBanner(out, kPlain, "1.0.0", where, 0);
    return out.str() + "\n";
}

}  // namespace

TEST_CASE("shell loop: banner, prompt, then end of input says goodbye", "[shell]") {
    FakeBackend backend;
    backend.where = "local (/data/x)";
    std::ostringstream out;
    ScriptedSource source({}, out);
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(out.str() == golden_shell::get("banner", false) + "\n" + "meradb:main> " + "\nPhir milenge!\n");
}

TEST_CASE("shell loop: a failed terminal read ends the shell with exit 1 and no goodbye", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"DIKHAO TABLES;"}, out);
    source.failAtEnd = true;
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 1);
    CHECK(backend.scripts == std::vector<std::string>{"DIKHAO TABLES;\n"});
    CHECK(source.reads == 2);
    CHECK(out.str().find("Phir milenge!") == std::string::npos);
}

TEST_CASE("shell loop: the prompt shows the database and an open transaction", "[shell]") {
    FakeBackend backend;
    backend.db = "college";
    backend.txn = true;
    std::ostringstream out;
    ScriptedSource source({}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(out.str() == bannerFor("fake:1") + "meradb:college*> \nPhir milenge!\n");
}

TEST_CASE("shell loop: a statement is collected until a line ends with a semicolon", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"DIKHAO", "  1  ", "SE t ;  ", "DIKHAO 2;"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts == std::vector<std::string>{"DIKHAO\n  1  \nSE t ;  \n", "DIKHAO 2;\n"});
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main>       ...>       ...> ok\n\nmeradb:main> ok\n\nmeradb:main> \nPhir milenge!\n");
}

TEST_CASE("shell loop: a line starting with a dot is a command only when no statement is pending", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"  .tables  ", "DIKHAO", ".tables", ";"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts == std::vector<std::string>{"DIKHAO TABLES;", "DIKHAO\n.tables\n;\n"});
}

TEST_CASE("shell loop: a blank line starts a statement, so a later dot-command is statement text", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"", ".tables", ";"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts == std::vector<std::string>{"\n.tables\n;\n"});
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main>       ...>       ...> ok\n\nmeradb:main> \nPhir milenge!\n");
}

TEST_CASE("shell loop: Unicode whitespace after the semicolon still ends the statement", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"A;\xC2\xA0", "B;\xE3\x80\x80"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts.size() == 2);
}

TEST_CASE("shell loop: a lone semicolon is sent to the backend, which decides what it means", "[shell]") {
    FakeBackend backend;
    backend.onRun = [](const std::string&) { return std::vector<Result>{}; };
    std::ostringstream out;
    ScriptedSource source({";"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts == std::vector<std::string>{";\n"});
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main> meradb:main> \nPhir milenge!\n");
}

TEST_CASE("shell loop: .exit says goodbye without a leading newline and reads no more", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({".exit", "DIKHAO 1;"}, out);
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(source.reads == 1);
    CHECK(backend.scripts.empty());
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main> Phir milenge!\n");
}

TEST_CASE("shell loop: end of input inside a statement drops the statement", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"DIKHAO *"}, out);
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(backend.scripts.empty());
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main>       ...> \nPhir milenge!\n");
}

TEST_CASE("shell loop: Ctrl+C at a prompt says goodbye like end of input", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"DIKHAO"}, out);
    source.interruptAtEnd = true;
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main>       ...> \nPhir milenge!\n");
}

TEST_CASE("shell loop: Ctrl+C while a statement runs ends the shell with 130, silently", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"SLOW;", "DIKHAO 2;"}, out);
    backend.onRun = [&](const std::string&) {
        source.pending = true;  // Ctrl+C pressed during the statement
        return oneMessage("done");
    };
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 130);
    CHECK(backend.scripts == std::vector<std::string>{"SLOW;\n"});  // the statement finished; the next line was never read
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main> done\n\n");
}

TEST_CASE("shell loop: Ctrl+C that landed before the next read ends it with 130 without running or reading anything", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"DIKHAO 2;"}, out);
    source.pending = true;  // Ctrl+C arrived after the last poll, before the shell asked for a line
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 130);
    CHECK(source.reads == 0);
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell loop: Ctrl+C during a dot-command also ends it with 130", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({".tables", "DIKHAO 2;"}, out);
    backend.onRun = [&](const std::string&) {
        source.pending = true;
        return oneMessage("done");
    };
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 130);
}

TEST_CASE("shell loop: a dropped connection is reported and the shell keeps going", "[shell]") {
    FakeBackend backend;
    backend.onRun = [](const std::string&) -> std::vector<Result> { throw ConnectionFailed("Server ne connection band kar diya"); };
    std::ostringstream out;
    ScriptedSource source({"A;", "B;"}, out);
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(out.str() == bannerFor("fake:1") +
                           "meradb:main> [Connection Galti] Server ne connection band kar diya\n"
                           "meradb:main> [Connection Galti] Server ne connection band kar diya\n"
                           "meradb:main> \nPhir milenge!\n");
}

TEST_CASE("shell loop: the colour style reaches the banner, the prompts and the results", "[shell]") {
    FakeBackend backend;
    backend.where = "local (/data/x)";
    std::ostringstream out;
    ScriptedSource source({"X;"}, out);
    repl::run(backend, source, out, term::Style::colored(), "1.0.0", 0);
    CHECK(out.str() == golden_shell::get("banner", true) + "\n" + golden_shell::get("prompt_main", true) +
                           "\x1b[32mok\x1b[0m\n\n" + golden_shell::get("prompt_main", true) + "\nPhir milenge!\n");
}

// The real engine against what the Python shell printed for the same input.
TEST_CASE("shell loop: every recorded Python transcript is reproduced exactly", "[shell][transcript]") {
    int count = 0;
    const golden_transcripts::Transcript* all = golden_transcripts::all(count);
    REQUIRE(count > 20);
    for (int i = 0; i < count; ++i) {
        const auto& transcript = all[i];
#ifndef _WIN32
        // recorded on Windows, where Python's stdin turns a lone CR into a line break; on POSIX it does not
        // (shell_diff.py compares the live shells for this script on every platform)
        if (std::string(transcript.name) == "lone_cr") continue;
#endif
        DYNAMIC_SECTION(transcript.name) {
            TempDir dir;
            LocalBackend backend(dir.file("data"));
            std::istringstream in(transcript.input);
            std::ostringstream out;
            repl::StreamLineSource source(in, out);
            CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
            const std::string marker = "se khatam hote hain\n\n";
            const std::string text = out.str();
            const auto at = text.find(marker);
            REQUIRE(at != std::string::npos);
            CHECK(text.substr(at + marker.size()) == transcript.output);
        }
    }
}
