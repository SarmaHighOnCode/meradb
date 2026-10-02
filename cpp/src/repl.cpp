// cpp/src/repl.cpp -- see repl.h.
#include "meradb/repl.h"
#include "meradb/cli_format.h"
#include "meradb/errors.h"
#include "meradb/pytext.h"
#include "meradb/repl_text.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace meradb::repl {

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

ReadStatus StreamLineSource::read(const std::string& prompt, std::string& line) {
    out_ << prompt << std::flush;
    line.clear();
    bool any = false;
    for (;;) {
        const int c = in_.get();
        if (c == std::istream::traits_type::eof()) break;
        any = true;
        if (c == '\n') return ReadStatus::Line;
        if (c == '\r') {
            if (in_.peek() == '\n') in_.get();
            return ReadStatus::Line;
        }
        line.push_back(static_cast<char>(c));
    }
    return any ? ReadStatus::Line : ReadStatus::Eof;
}

ReadStatus ConsoleLineSource::read(const std::string& prompt, std::string& line) {
    out_ << prompt << std::flush;
    return sys::readTerminalLine(line);
}

// ---------------------------------------------------------------------------
// running text and files
// ---------------------------------------------------------------------------

namespace {

// Python's text mode (universal newlines): "\r\n" and a lone "\r" both become "\n".
std::string normalizeNewlines(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') {
            out += '\n';
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
        } else {
            out += in[i];
        }
    }
    return out;
}

}  // namespace

bool runText(Backend& backend, const std::string& text, std::ostream& out, const term::Style& style) {
    std::vector<Result> results;
    try {
        results = backend.runScript(text);
    } catch (const MeraDBError& e) {  // e.g. the server connection dropped
        out << e.what() << "\n";
        return false;
    }
    bool ok = true;
    for (const auto& result : results) {
        const std::string rendered = formatResult(result, style);
        if (!rendered.empty()) out << rendered << "\n";
        out << "\n";
        if (!result.error.empty()) ok = false;
    }
    return ok;
}

bool runFile(Backend& backend, const std::string& path, std::ostream& out, const term::Style& style) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        // Same wording as Python's OSError text: "[Errno 2] No such file or directory: 'x'"
        std::string quoted;
        for (char c : path) {
            if (c == '\\' || c == '\'') quoted += '\\';
            quoted += c;
        }
        std::error_code ec;
        const bool missing = !std::filesystem::exists(std::filesystem::u8path(path), ec);
        out << "File nahi khuli: " << (missing ? "[Errno 2] No such file or directory: '" : "[Errno 13] Permission denied: '")
            << quoted << "'\n";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // utf-8-sig, like Python
    return runText(backend, normalizeNewlines(text), out, style);
}

// ---------------------------------------------------------------------------
// the shell
// ---------------------------------------------------------------------------

bool endsStatement(const std::string& buffer) { return pytext::endsWith(pytext::rstrip(buffer), ";"); }

bool handleDotCommand(Backend& backend, const std::string& line, std::ostream& out, const term::Style& style) {
    const std::vector<std::string> parts = pytext::split(line);
    if (parts.empty()) return true;  // cannot happen: the caller passes a stripped line that starts with "."
    const std::string cmd = pytext::lowerAscii(parts[0]);
    if (cmd == ".exit" || cmd == ".quit" || cmd == ".nikal") return false;
    if (cmd == ".help") {
        const std::string topic = pytext::strip(line.substr(parts[0].size()));  // everything after ".help"
        if (!topic.empty())
            out << renderReference(style, topic) << "\n";
        else
            out << fullHelp(style) << "\n";
    } else if (cmd == ".tables") {
        runText(backend, "DIKHAO TABLES;", out, style);
    } else if (cmd == ".schema" && parts.size() > 1) {
        runText(backend, "BATAO " + parts[1] + ";", out, style);
    } else if (cmd == ".run" && parts.size() > 1) {
        runFile(backend, parts[1], out, style);
    } else {
        out << "Ye shell command nahi pata: " << line << "  (.help dekho)\n";
    }
    return true;
}

}  // namespace meradb::repl
