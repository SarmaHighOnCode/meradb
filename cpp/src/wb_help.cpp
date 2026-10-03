// cpp/src/wb_help.cpp
#include "meradb/wb_help.h"
#include "meradb/wb_markdown.h"
#include "language_doc_data.inc"

namespace meradb::wb {

const char* keysHelpMarkdown() {
    // tui.py KEYS_HELP, verbatim; the second table lists the keys this version adds.
    return R"MD(# MeraDB Workbench: Madad

| Key | Kaam |
|-----|------|
| **F5** / **Ctrl+R** | Query chalao (selected text only, if something is selected) |
| **F6** | SAMJHAO: query plan dikhao (index / scan / join), bina chalaye |
| **Ctrl+Up / Ctrl+Down** | Pichli / agli query (history) |
| **Ctrl+S** | Results ko CSV file mein save karo (`exports/` folder) |
| **Ctrl+O** | Doosre server se connect karo, ya local mode |
| **Tab / Shift+Tab** | Editor, results aur schema tree ke beech jao |
| **Enter** on a table in the tree | Uske pehle 100 rows dikhao |
| **Enter** on a database | Us database ko ISTEMAL karo |
| **Enter** on a column | Column ka naam editor mein daalo |
| **Ctrl+L** | Log saaf karo |
| **F1** / **Esc** | Ye madad kholo / band karo |
| **Ctrl+Q** | Bahar niklo |

## Extra keys (C++ version)

| Key | Kaam |
|-----|------|
| **Ctrl+P / Ctrl+N** | History: pichli / agli (jab Ctrl+Up / Ctrl+Down terminal se na aaye) |
| **Shift+Arrows / Home / End** | Text select karo; F5 / F6 sirf select kiya hua hissa chalate hain |
| **Ctrl+A** | Editor ka poora text select karo |
| **PageUp / PageDown** | Results, log, tree aur madad mein scroll karo |
| **Mouse** | Panel par click karo (focus), wheel se scroll |
)MD";
}

bool languageDocEmbedded() { return kLanguageDocPresent; }

std::string languageDoc() {
    if (!kLanguageDocPresent) return "*(docs/LANGUAGE.md nahi mila)*";
    std::string text;
    const std::size_t n = sizeof(kLanguageDocBytes) - 1;
    text.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const char c = static_cast<char>(kLanguageDocBytes[i]);
        if (c != '\r') text += c;
    }
    return text;
}

std::string helpMarkdownWith(const std::string& language) { return std::string(keysHelpMarkdown()) + "\n" + language; }

std::string helpMarkdown() { return helpMarkdownWith(languageDoc()); }

std::vector<Line> renderHelp(int width) { return renderMarkdown(helpMarkdown(), width); }

}  // namespace meradb::wb
