// cpp/src/repl_text.cpp -- see repl_text.h.
#include "meradb/repl_text.h"
#include "meradb/pytext.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

namespace meradb::repl {

// helpReference() and examplesHelp(): generated, see cpp/tests/gen_shell_help.py.
#include "shell_help_data.inc"

std::string shellCommandsHelp(const term::Style& style) {
    return style.c("Shell commands", {1, 35}) +
           "\n"
           "  .help [khoj]     ye reference dikhao (poora, ya sirf matching keywords -- jaise: .help join)\n"
           "  .tables          saari tables dikhao\n"
           "  .schema <table>  table ka structure\n"
           "  .run <file>      ek .mdb script file chalao\n"
           "  .exit / .nikal   bahar niklo";
}

std::string renderReference(const term::Style& style, const std::string& rawQuery) {
    const std::string query = pytext::lower(pytext::strip(rawQuery));

    struct Matched {
        const HelpCategory* category;
        std::vector<const HelpRow*> rows;
    };
    std::vector<Matched> matched;
    for (const auto& category : helpReference()) {
        Matched m{&category, {}};
        for (const auto& row : category.rows) {
            const std::string haystack = pytext::lower(row.keyword + " " + row.sql + " " + row.description);
            if (query.empty() || haystack.find(query) != std::string::npos) m.rows.push_back(&row);
        }
        if (!m.rows.empty()) matched.push_back(std::move(m));
    }

    if (!query.empty() && matched.empty())
        return "'" + query + "' ke liye kuch nahi mila. Poora reference ke liye sirf .help likho.";

    std::size_t keywordWidth = 0, sqlWidth = 0;
    for (const auto& m : matched)
        for (const HelpRow* row : m.rows) {
            keywordWidth = std::max(keywordWidth, pytext::length(row->keyword));
            sqlWidth = std::max(sqlWidth, pytext::length(row->sql));
        }

    std::string out;
    for (const auto& m : matched) {
        out += style.c(m.category->name, {1, 35}) + "\n";
        for (const HelpRow* row : m.rows)
            out += "  " + style.c(pytext::ljust(row->keyword, keywordWidth), {1, 36}) + "  " +
                   style.c(pytext::ljust(row->sql, sqlWidth), {2}) + "  " + row->description + "\n";
        out += "\n";
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();  // "\n".join(out).rstrip("\n")
    return out;
}

std::string fullHelp(const term::Style& style) {
    return shellCommandsHelp(style) + "\n\n" + renderReference(style) + "\n\n" + examplesHelp();
}

// ---------------------------------------------------------------------------
// banner
// ---------------------------------------------------------------------------

namespace {

const char* const kBlock = "\xE2\x96\x88";  // U+2588 FULL BLOCK
const char* const kDot = "\xC2\xB7";         // U+00B7 MIDDLE DOT

// A hand-authored 5x7 dot-matrix font, just for the letters M E R A D B. 1 = filled pixel.
struct Glyph {
    char letter;
    const char* rows[7];
};

const Glyph kFont[] = {
    {'M', {"1...1", "11.11", "1.1.1", "1.1.1", "1...1", "1...1", "1...1"}},
    {'E', {"11111", "1....", "1....", "1111.", "1....", "1....", "11111"}},
    {'R', {"1111.", "1...1", "1...1", "1111.", "1.1..", "1..1.", "1...1"}},
    {'A', {".111.", "1...1", "1...1", "11111", "1...1", "1...1", "1...1"}},
    {'D', {"1111.", "1...1", "1...1", "1...1", "1...1", "1...1", "1111."}},
    {'B', {"1111.", "1...1", "1...1", "1111.", "1...1", "1...1", "1111."}},
};

const Glyph& glyphFor(char letter) {
    for (const Glyph& glyph : kFont)
        if (glyph.letter == letter) return glyph;
    return kFont[0];  // unreachable: only the letters of "MERADB" are asked for
}

const std::vector<std::string> kWordmark = {
    "  __  __                 ____  ____",
    " |  \\/  | ___ _ __ __ _|  _ \\| __ )",
    " | |\\/| |/ _ \\ '__/ _` | | | |  _ \\",
    " | |  | |  __/ | | (_| | |_| | |_) |",
    " |_|  |_|\\___|_|  \\__,_|____/|____/",
};

struct Logo {
    std::vector<std::string> rows;
    int width;  // on-screen width: escape codes are invisible on screen but not to size()
};

// "MERADB" as 7 lines of block letters, split and coloured as MERA (bold cyan) + DB (bold magenta). Each
// pixel is two blocks wide, because a terminal cell is taller than it is wide.
Logo renderLogo(const term::Style& style) {
    const char* const halves[2] = {"MERA", "DB"};
    std::vector<std::string> rows(7);
    int width = 0;
    for (int half = 0; half < 2; ++half) {
        const std::string letters = halves[half];
        for (std::size_t i = 0; i < letters.size(); ++i) {
            const Glyph& glyph = glyphFor(letters[i]);
            for (int r = 0; r < 7; ++r) {
                std::string pixels;
                for (const char* px = glyph.rows[r]; *px != '\0'; ++px)
                    pixels += *px == '1' ? std::string(kBlock) + kBlock : std::string("  ");
                rows[static_cast<std::size_t>(r)] += half == 0 ? style.c(pixels, {1, 36}) : style.c(pixels, {1, 35});
            }
            width += 10;  // each glyph is 5 pixels, drawn 2 characters wide
            if (i + 1 != letters.size()) {
                for (auto& row : rows) row += " ";  // gap between letters of the same half
                width += 1;
            }
        }
        if (half == 0) {
            for (auto& row : rows) row += "   ";  // wider gap between MERA and DB
            width += 3;
        }
    }
    return {rows, width};
}

// Indents every line by the SAME amount, so the block is centred under `width` without disturbing its own
// internal alignment (centring each line separately would ruin the slanted wordmark).
std::vector<std::string> centerBlock(const std::vector<std::string>& lines, int width,
                                     const std::function<std::string(const std::string&)>& paint) {
    std::size_t inner = 0;
    for (const auto& line : lines) inner = std::max(inner, pytext::length(line));
    const int pad = std::max(0, (width - static_cast<int>(inner)) / 2);
    std::vector<std::string> out;
    for (const auto& line : lines) out.push_back(std::string(static_cast<std::size_t>(pad), ' ') + paint(line));
    return out;
}

}  // namespace

void printBanner(std::ostream& out, const term::Style& style, const std::string& version, const std::string& where,
                 int pauseMs) {
    const Logo logo = renderLogo(style);
    const auto wordmark = centerBlock(kWordmark, logo.width, [&](const std::string& s) { return style.c(s, {2}); });  // dim
    const auto taglineBlock = centerBlock({"meraDB " + version + " -- apna database, apni bhasha."}, logo.width,
                                          [&](const std::string& s) { return style.c(s, {1}); });
    const auto reveal = [&](const std::vector<std::string>& lines) {
        for (const auto& line : lines) {
            out << line << "\n";
            if (pauseMs > 0) {
                out.flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(pauseMs));
            }
        }
    };

    out << "\n";
    reveal(logo.rows);
    out << "\n";
    reveal(wordmark);
    out << "\n";
    out << taglineBlock[0] << "\n";
    out << "  connected: " << where << "\n";
    const std::string hint = "  " + style.c(".help", {1, 33}) + " commands  " + style.c(kDot, {2}) + "  " +
                             style.c(".exit", {1, 33}) + " bahar niklo  " + style.c(kDot, {2}) + "  statements " +
                             style.c(";", {1, 33}) + " se khatam hote hain\n";
    out << hint;
}

std::string promptFor(const term::Style& style, const std::string& db, bool inTransaction) {
    const std::string marker = inTransaction ? style.c("*", {1, 33}) : std::string();
    return "meradb:" + style.c(db, {1, 36}) + marker + "> ";
}

}  // namespace meradb::repl
