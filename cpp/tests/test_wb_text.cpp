#include <catch2/catch_test_macros.hpp>
#include "meradb/wb_text.h"
#include "test_util.h"
#include <fstream>

using namespace meradb;
using namespace meradb::wb;

TEST_CASE("wbtext cells follow tui.py cell()", "[wbtext]") {
    CHECK(makeCell(Value()).text == "KHALI");
    CHECK(makeCell(Value()).kind == CellKind::Null);
    CHECK(makeCell(Value(true)).text == "SACH");
    CHECK(makeCell(Value(true)).kind == CellKind::True);
    CHECK(makeCell(Value(false)).kind == CellKind::False);
    CHECK(makeCell(Value(int64_t(42))).kind == CellKind::Number);
    CHECK(makeCell(Value(8.5)).text == "8.5");
    CHECK(makeCell(Value(std::string("hi"))).kind == CellKind::Plain);
    CHECK(makeCell(Value(std::string("a\nb\tc"))).text == "a\xE2\x86\xB5" "b c");  // R16
    CHECK(cellRightAligned(CellKind::Number));
    CHECK_FALSE(cellRightAligned(CellKind::Plain));
    CHECK(cellStyle(CellKind::True).fg == palette::kGreen);
    CHECK(cellStyle(CellKind::False).fg == palette::kRed);
    CHECK(cellStyle(CellKind::Null).dim);
    CHECK(cellStyle(CellKind::Null).italic);
    CHECK(cellStyle(CellKind::Number).fg == palette::kPurple);
}

TEST_CASE("wbtext table, widths and title", "[wbtext]") {
    Result r;
    r.columns = {"id", "naam"};
    r.rows.push_back({Value(int64_t(1)), Value(std::string("Asha"))});
    r.rows.push_back({Value(int64_t(22)), Value()});
    ResultTable t = makeTable(r);
    CHECK(t.rows.size() == 2);
    CHECK(columnWidths(t) == std::vector<int>({2, 5}));  // "22", "KHALI"
    CHECK(resultsTitle(&t) == "Results -- 2 row(s)");
    CHECK(resultsTitle(nullptr) == "Results");
}

TEST_CASE("wbtext splitLines matches str.splitlines", "[wbtext]") {
    CHECK(splitLines("").empty());
    CHECK(splitLines("a\n") == std::vector<std::string>({"a"}));
    CHECK(splitLines("a\n\nb") == std::vector<std::string>({"a", "", "b"}));
    CHECK(splitLines("a\r\nb\rc") == std::vector<std::string>({"a", "b", "c"}));
    CHECK(splitLines("a\x0b" "b\x0c" "c\x1c" "d") == std::vector<std::string>({"a", "b", "c", "d"}));
    CHECK(splitLines("a\xC2\x85" "b\xE2\x80\xA8" "c\xE2\x80\xA9" "d") == std::vector<std::string>({"a", "b", "c", "d"}));
    CHECK(splitLines("\n") == std::vector<std::string>({""}));
}

TEST_CASE("wbtext echo entry is a dim prompt plus the highlighted statement", "[wbtext]") {
    LogEntry e = echoEntry("main", "  DIKHAO * SE t;\nx  ");
    CHECK(e.kind == LogKind::Echo);
    REQUIRE(e.lines.size() == 2);
    CHECK(plainText(e.lines[0]) == "main> DIKHAO * SE t;");
    CHECK(e.lines[0][0].style.dim);
    CHECK(plainText(e.lines[1]) == "x");
    // the keyword is bold pink, the operator pink
    bool sawKeyword = false;
    for (const Segment& s : e.lines[0])
        if (s.text == "DIKHAO") sawKeyword = s.style.bold && s.style.fg == 0xff79c6;
    CHECK(sawKeyword);
}

TEST_CASE("wbtext log keeps at most kMaxLines lines and clears", "[wbtext]") {
    LogBuffer log;
    for (std::size_t i = 0; i < LogBuffer::kMaxLines + 5; ++i) log.addText(LogKind::Plain, "x");
    CHECK(log.lineCount() == LogBuffer::kMaxLines);
    log.addText(LogKind::Error, "a\nb");
    CHECK(log.entries().back().lines.size() == 2);
    CHECK(log.entries().back().lines[0][0].style.fg == palette::kRed);
    log.clear();
    CHECK(log.lineCount() == 0);
}

TEST_CASE("wbtext clip and wrap respect wide characters", "[wbtext]") {
    Line l;
    appendSegment(l, "ab\xE6\x97\xA5\xE6\x9C\xAC" "cd", Style{});  // ab日本cd : cells a b 日(2) 本(2) c d
    CHECK(plainText(clipLine(l, 0, 3)) == "ab ");                     // 日 does not fit in the last cell
    CHECK(plainText(clipLine(l, 3, 3)) == " \xE6\x9C\xAC");          // starts inside 日: blank, then 本
    CHECK(plainText(clipLine(l, 6, 10)) == "cd");
    CHECK(plainText(clipLine(l, 20, 5)) == "");
    auto w = wrapLine(l, 4);
    REQUIRE(w.size() == 2);
    CHECK(plainText(w[0]) == "ab\xE6\x97\xA5");
    CHECK(plainText(w[1]) == "\xE6\x9C\xAC" "cd");
    CHECK(wrapLine(Line{}, 5).size() == 1);
    Line a; appendSegment(a, "abcdef", fgStyle(1));
    auto w2 = wrapLine(a, 4);
    CHECK(plainText(w2[0]) == "abcd");
    CHECK(plainText(w2[1]) == "ef");
    CHECK(w2[1][0].style.fg == 1);
}

TEST_CASE("wbtext csv follows Python csv.writer", "[wbtext]") {
    CHECK(csvRow({"a", "b"}) == "a,b\r\n");
    CHECK(csvRow({"a,b", "c\"d", "e\nf"}) == "\"a,b\",\"c\"\"d\",\"e\nf\"\r\n");
    CHECK(csvRow({""}) == "\"\"\r\n");        // a lone empty field is quoted
    CHECK(csvRow({"", ""}) == ",\r\n");
    CHECK(csvRow({" x ", "\xC3\xA9"}) == " x ,\xC3\xA9\r\n");
    Result r;
    r.columns = {"id", "naam"};
    r.rows.push_back({Value(int64_t(1)), Value()});
    r.rows.push_back({Value(true), Value(std::string("a,b"))});
    CHECK(csvDocument(r) == "id,naam\r\n1,\r\nSACH,\"a,b\"\r\n");
    CHECK(exportFileStamp().size() == 15);
}

TEST_CASE("wbtext exportCsv writes exports/meradb-<stamp>.csv byte for byte", "[wbtext]") {
    meradb_test::TempDir dir;
    Result r;
    r.columns = {"x"};
    r.rows.push_back({Value(std::string("a"))});
    ExportOutcome out = exportCsv(r, dir.str(), "20260101-000000");
    REQUIRE(out.ok);
    CHECK(out.path.find("meradb-20260101-000000.csv") != std::string::npos);
    std::ifstream in(out.path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(bytes == "x\r\na\r\n");
}

TEST_CASE("wbtext exportCsv reports a failure instead of throwing", "[wbtext]") {
    meradb_test::TempDir dir;
    { std::ofstream blocker(dir.file("exports")); blocker << "a file where the folder should be"; }
    Result r;
    r.columns = {"x"};
    ExportOutcome out = exportCsv(r, dir.str(), "20260101-000000");
    CHECK_FALSE(out.ok);
    CHECK_FALSE(out.error.empty());
}

TEST_CASE("wbtext history follows tui.py", "[wbtext]") {
    History h;
    CHECK_FALSE(h.step(-1).has_value());
    h.add("a"); h.add("b"); h.add("b");  // consecutive duplicate ignored
    CHECK(h.size() == 2);
    CHECK(*h.step(-1) == "b");
    CHECK(*h.step(-1) == "a");
    CHECK(*h.step(-1) == "a");   // clamped at the oldest
    CHECK(*h.step(1) == "b");
    CHECK(*h.step(1) == "");     // past the newest: an empty editor
    CHECK(*h.step(1) == "");
    h.add("c");                  // position returns to the end
    CHECK(*h.step(-1) == "c");
}

TEST_CASE("wbtext scroll state keeps the cursor visible", "[wbtext]") {
    ScrollState s;
    s.setCount(100);
    s.setHeight(10);
    s.moveCursor(15);
    CHECK(s.cursor() == 15);
    CHECK(s.top() == 6);
    s.setCursor(0);
    CHECK(s.top() == 0);
    s.pageCursor(1);
    CHECK(s.cursor() == 9);
    s.scrollBy(-5);
    CHECK(s.top() == 0);
    s.followEnd();
    CHECK(s.top() == 90);
    CHECK(s.atBottom());
    s.setCount(5);   // shrinking clamps everything
    CHECK(s.top() == 0);
    CHECK(s.cursor() <= 4);
}
