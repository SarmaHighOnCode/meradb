#include "meradb/cli_format.h"
#include "meradb/datatypes.h"
#include <algorithm>
#include <vector>

namespace meradb {

namespace {

// Python pads by code points (len()), not bytes: count UTF-8 lead bytes.
size_t displayLen(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string padRight(const std::string& s, size_t width) {
    size_t len = displayLen(s);
    return len >= width ? s : s + std::string(width - len, ' ');
}

std::string formatTable(const Result& r, const term::Style& style) {
    std::vector<std::vector<std::string>> body;
    for (const auto& row : r.rows) {
        std::vector<std::string> cells;
        for (const auto& v : row) cells.push_back(formatValue(v));
        body.push_back(std::move(cells));
    }
    std::vector<size_t> widths;
    for (const auto& h : r.columns) widths.push_back(displayLen(h));
    for (const auto& row : body)
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) widths[i] = std::max(widths[i], displayLen(row[i]));

    std::string plainSep = "+";
    for (size_t w : widths) plainSep += std::string(w + 2, '-') + "+";
    const std::string sep = style.c(plainSep, {2});  // dim

    auto line = [&](const std::vector<std::string>& cells) {
        std::string out = "|";
        for (size_t i = 0; i < widths.size(); ++i) {
            out += " " + padRight(i < cells.size() ? cells[i] : "", widths[i]) + " |";
        }
        return out;
    };

    std::string out = sep + "\n" + style.c(line(r.columns), {1}) + "\n" + sep;  // the header row is bold
    for (const auto& row : body) out += "\n" + line(row);
    out += "\n" + sep;
    return out;
}

}  // namespace

std::string formatResult(const Result& r, const term::Style& style) {
    if (!r.error.empty()) return style.c(r.error, {31});  // red
    std::string out;
    if (!r.columns.empty()) out = formatTable(r, style);
    if (!r.message.empty()) out += (out.empty() ? "" : "\n") + style.c(r.message, {32});  // green
    return out;
}

}  // namespace meradb
