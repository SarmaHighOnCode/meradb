// cpp/src/repl_text.cpp -- see repl_text.h.
#include "meradb/repl_text.h"
#include "meradb/pytext.h"
#include <algorithm>

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
    const std::string query = pytext::lowerAscii(pytext::strip(rawQuery));

    struct Matched {
        const HelpCategory* category;
        std::vector<const HelpRow*> rows;
    };
    std::vector<Matched> matched;
    for (const auto& category : helpReference()) {
        Matched m{&category, {}};
        for (const auto& row : category.rows) {
            const std::string haystack = pytext::lowerAscii(row.keyword + " " + row.sql + " " + row.description);
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

}  // namespace meradb::repl
