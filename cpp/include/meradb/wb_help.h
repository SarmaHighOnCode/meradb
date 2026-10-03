// cpp/include/meradb/wb_help.h
#pragma once
#include "meradb/wb_text.h"
#include <string>
#include <vector>

namespace meradb::wb {
const char* keysHelpMarkdown();                         // tui.py KEYS_HELP, verbatim, plus the C++ extra keys section
bool languageDocEmbedded();                             // false when docs/LANGUAGE.md was missing at build time
std::string languageDoc();                              // the embedded text, or "*(docs/LANGUAGE.md nahi mila)*"
std::string helpMarkdownWith(const std::string& language);   // keysHelpMarkdown() + "\n" + language
std::string helpMarkdown();                             // helpMarkdownWith(languageDoc())
std::vector<Line> renderHelp(int width);                // renderMarkdown(helpMarkdown(), width)
}  // namespace meradb::wb
