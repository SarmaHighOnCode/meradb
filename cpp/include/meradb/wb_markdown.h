// cpp/include/meradb/wb_markdown.h
#pragma once
#include "meradb/wb_text.h"
#include <string>
#include <vector>

namespace meradb::wb {
// The small Markdown subset docs/LANGUAGE.md and the key table use, as styled terminal lines for a given width.
// Blocks: # .. ###### headings (bold; level 1 pink, 2 cyan, 3+ plain bold), paragraphs (word-wrapped, lines of a
// paragraph joined by one space), "- " / "* " bullets (shown "• ", hanging indent), "1. " numbered items (kept),
// fenced ``` code blocks (verbatim, never wrapped, on the current-line background), tables (| a | b | with a
// |---| divider: aligned columns joined by " │ ", header bold, a "─┼─" divider line), "> " quotes (dim, "▌ "),
// horizontal rules (--- or ***). One blank line between blocks. Inline: **bold**, `code` (cyan), *italic*.
// CRLF and LF both work. Never throws; a width below 1 counts as 1.
std::vector<Line> renderMarkdown(const std::string& markdown, int width);
}  // namespace meradb::wb
