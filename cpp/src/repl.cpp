// cpp/src/repl.cpp -- see repl.h.
#include "meradb/repl.h"

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

}  // namespace meradb::repl
