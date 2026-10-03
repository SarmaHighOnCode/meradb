// cpp/include/meradb/wb_text.h
//
// The pure text model of the workbench (no terminal): styles, the log, result-table cells, CSV, clipping and
// wrapping by terminal cells, query history and scroll state. Mirrors the helpers of meradb/tui.py.
#pragma once
#include "meradb/engine.h"
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace meradb::wb {

// ---- styles ----
struct Style {
    int fg = -1;  // 0xRRGGBB, -1 = the terminal's default
    int bg = -1;
    bool bold = false, dim = false, italic = false, underline = false, inverse = false;
    bool operator==(const Style& o) const {
        return fg == o.fg && bg == o.bg && bold == o.bold && dim == o.dim && italic == o.italic &&
               underline == o.underline && inverse == o.inverse;
    }
    bool operator!=(const Style& o) const { return !(*this == o); }
};
namespace palette {  // tui.py / highlight.py (Dracula)
constexpr int kPink = 0xff79c6, kString = 0xf1fa8c, kPurple = 0xbd93f9, kComment = 0x6272a4, kCyan = 0x8be9fd,
              kGreen = 0x50fa7b, kOrange = 0xffb86c, kRed = 0xff5555, kYellow = 0xf1fa8c, kText = 0xf8f8f2,
              kBackground = 0x282a36, kCurrentLine = 0x44475a, kStripe = 0x2f3142;
}
Style fgStyle(int rgb, bool bold = false, bool dim = false, bool italic = false);

struct Segment { std::string text; Style style; };
using Line = std::vector<Segment>;
std::string plainText(const Line& line);
void appendSegment(Line& line, const std::string& text, const Style& style);  // merges equal neighbours, skips ""

// ---- the log ----
enum class LogKind { Plain, Bold, Dim, Error, Message, Connected, Warn, Echo };
const char* logKindName(LogKind kind);  // plain bold dim error message connected warn echo
Style styleForLogKind(LogKind kind);
struct LogEntry { LogKind kind = LogKind::Plain; std::vector<Line> lines; };
class LogBuffer {
public:
    static constexpr std::size_t kMaxLines = 20000;
    void add(LogEntry entry);                                  // drops the oldest entries past kMaxLines lines
    void addText(LogKind kind, const std::string& text);       // text split at '\n', all lines in styleForLogKind
    void clear();
    const std::deque<LogEntry>& entries() const { return entries_; }
    std::size_t lineCount() const { return lines_; }
    std::size_t revision() const { return revision_; }      // bumps on every add / clear (view caches key on it)
    std::size_t generation() const { return generation_; }  // bumps when old entries disappear (clear, trimming)
private:
    std::deque<LogEntry> entries_;
    std::size_t lines_ = 0, revision_ = 0, generation_ = 0;
};

// Python's str.splitlines(): \n \r\n \r \v \f \x1c \x1d \x1e U+0085 U+2028 U+2029; no empty last line; "" -> {}.
std::vector<std::string> splitLines(const std::string& text);
Line highlightLine(const std::string& line);               // highlight::spans -> styled segments (RICH_STYLES)
// The part of highlightLine(line) that lies in bytes [from, to) (both on character boundaries), highlighted as
// part of the whole line.
Line highlightRange(const std::string& line, std::size_t from, std::size_t to);
// tui.py's `Text(f"{db}> ", dim) + highlighted(text)`: lines of text.strip().splitlines(), prefix on the first.
LogEntry echoEntry(const std::string& db, const std::string& text);

// ---- the results table ----
enum class CellKind { Null, True, False, Number, Plain };
const char* cellKindName(CellKind kind);  // null true false number plain
struct Cell { std::string text; CellKind kind = CellKind::Plain; };
Cell makeCell(const Value& value);        // tui.py cell(): KHALI / SACH / JHOOTH / number / text (R16 for newlines)
Style cellStyle(CellKind kind);
inline bool cellRightAligned(CellKind kind) { return kind == CellKind::Number; }
struct ResultTable { std::vector<std::string> columns; std::vector<std::vector<Cell>> rows; };
ResultTable makeTable(const Result& result);
std::vector<int> columnWidths(const ResultTable& table);        // display width of the widest of header and cells
std::string resultsTitle(const ResultTable* table);             // "Results" or "Results -- N row(s)"

// ---- cell geometry (terminal cells, wide characters count 2) ----
struct Glyph { std::string text; int width; };                  // one character with its combining marks
std::vector<Glyph> glyphs(const std::string& utf8);
Line clipLine(const Line& line, int skipCells, int takeCells);  // the part in [skip, skip+take); a cut wide glyph becomes ' '
std::vector<Line> wrapLine(const Line& line, int width);        // character wrap, at least one (maybe empty) line

// ---- CSV (Python csv.writer defaults) ----
std::string csvRow(const std::vector<std::string>& fields);     // quoted as needed, ends "\r\n"
std::string csvDocument(const Result& result);                  // header row, then rows (KHALI -> "", else formatValue)
std::string exportFileStamp();                                  // "20260929-140307" (local time)
struct ExportOutcome { bool ok = false; std::string path; std::string error; };
// Writes <baseDir>/exports/meradb-<stamp>.csv ("" = current directory), UTF-8, bytes exactly as csvDocument.
ExportOutcome exportCsv(const Result& result, const std::string& baseDir, const std::string& stamp);

// ---- history and scrolling ----
class History {
public:
    void add(const std::string& text);                  // appended unless equal to the last entry; position = size
    std::optional<std::string> step(int delta);         // nullopt when empty; pos = clamp(pos+delta, 0, size); "" at size
    std::size_t size() const { return items_.size(); }
    const std::vector<std::string>& items() const { return items_; }
private:
    std::vector<std::string> items_;
    std::size_t pos_ = 0;
};

class ScrollState {  // a window of `height` rows over `count` rows, plus an optional cursor row
public:
    void setCount(int n);
    void setHeight(int h);
    int count() const { return count_; }
    int height() const { return height_; }
    int top() const { return top_; }
    int cursor() const { return cursor_; }
    void setCursor(int row);            // clamped; scrolls just enough to keep it visible
    void moveCursor(int delta);
    void pageCursor(int pages);         // pages * (height - 1) rows
    void scrollBy(int delta);           // moves the window only (the cursor may leave it)
    void ensureVisible(int row);
    void followEnd();                   // top = last full window
    bool atBottom() const;
private:
    int maxTop() const;
    void clamp();
    int count_ = 0, height_ = 1, top_ = 0, cursor_ = 0;
};

}  // namespace meradb::wb
