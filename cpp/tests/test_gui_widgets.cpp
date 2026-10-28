// cpp/tests/test_gui_widgets.cpp -- the pure parts of the desktop window's widgets: the editor's undo stack and its
// position <-> pixel mapping (with a fake glyph width), the tree filter, grid helpers, the key table. No window.
#include <catch2/catch_test_macros.hpp>
#include "meradb/gui_editor.h"
#include "meradb/gui_grid.h"
#include "meradb/gui_fonts.h"
#include "meradb/gui_options.h"
#include "meradb/gui_platform.h"
#include "meradb/gui_tree.h"
#include "meradb/gui_ui.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <catch2/catch_approx.hpp>

using namespace meradb;
using namespace meradb::gui;

namespace {

Advance fixed(float width) {
    Advance a;
    a.ascii = width;
    a.other = [width](char32_t cp) { return cp >= 0x3000 ? 2.0f * width : width; };   // CJK is double width
    return a;
}

EditorSnapshot snap(const std::string& text, int row = 0, int col = 0) {
    EditorSnapshot s;
    s.text = text;
    s.cursor = wb::Pos{row, col};
    return s;
}

}  // namespace

// ---------------------------------------------------------------- UTF-8

TEST_CASE("gui editor: UTF-8 decoding and encoding round-trip, including beyond the BMP", "[gui][editor]") {
    for (char32_t cp : {U'a', U'\u00e9', U'\u0928', U'\U0001F600', U'\U0010FFFF'}) {
        const std::string bytes = encodeUtf8(cp);
        const char* p = bytes.data();
        CHECK(decodeUtf8(p, bytes.data() + bytes.size()) == cp);
        CHECK(p == bytes.data() + bytes.size());
    }
    CHECK(encodeUtf8(0xD800) == encodeUtf8(0xFFFD));
    CHECK(encodeUtf8(0x110000) == encodeUtf8(0xFFFD));
    const std::string cut = "\xF0\x9F";   // a 4-byte lead with two bytes missing: consumed, never read past the end
    const char* p = cut.data();
    decodeUtf8(p, cut.data() + cut.size());
    CHECK(p == cut.data() + cut.size());
    const std::string stray = "\x80z";
    p = stray.data();
    CHECK(decodeUtf8(p, stray.data() + stray.size()) == 0xFFFD);
    CHECK(p == stray.data() + 1);
}

// ---------------------------------------------------------------- columns and pixels

TEST_CASE("gui editor: columns map to pixels and back with a fixed glyph width", "[gui][editor]") {
    const Advance adv = fixed(8.0f);
    CHECK(columnX("hello", 0, adv) == 0.0f);
    CHECK(columnX("hello", 3, adv) == 24.0f);
    CHECK(columnX("hello", 99, adv) == 40.0f);   // past the end: the end
    CHECK(columnX("a\xE3\x81\x82z", 2, adv) == 24.0f);   // U+3042 is two cells wide
    CHECK(columnAt("hello", 0.0f, adv) == 0);
    CHECK(columnAt("hello", 3.9f, adv) == 0);    // nearest boundary: left of the half
    CHECK(columnAt("hello", 4.1f, adv) == 1);
    CHECK(columnAt("hello", 20.0f, adv) == 3);   // 20 is nearer to 24 than to 16
    CHECK(columnAt("hello", -5.0f, adv) == 0);
    CHECK(columnAt("hello", 1000.0f, adv) == 5);
    CHECK(columnAt("", 10.0f, adv) == 0);
    CHECK(columnAt("a\xE3\x81\x82z", 20.0f, adv) == 2);   // inside the wide glyph: nearer to its right edge (24) than to 8
    // every boundary maps back to its own column
    const std::string line = "x\xE3\x81\x82yz\xF0\x9F\x98\x80q";
    for (int col = 0; col <= 6; ++col) CHECK(columnAt(line, columnX(line, col, adv), adv) == col);
}

TEST_CASE("gui editor: the visible slice of a line covers exactly the columns in view", "[gui][editor]") {
    const Advance adv = fixed(10.0f);
    const std::string line = "0123456789ABCDEFGHIJ";
    Slice s = visibleSlice(line, 35.0f, 82.0f, adv);
    CHECK(s.firstCol == 3);          // column 3 spans 30..40: it reaches into view
    CHECK(s.firstX == 30.0f);
    CHECK(s.firstByte == 3);
    CHECK(s.endCol == 9);            // column 8 starts at 80 < 82 and is drawn; column 9 starts at 90 >= 82
    s = visibleSlice(line, 0.0f, 1000.0f, adv);
    CHECK(s.firstCol == 0);
    CHECK(s.endCol == 20);
    CHECK(s.endByte == line.size());
    s = visibleSlice(line, 500.0f, 600.0f, adv);   // scrolled beyond the end: an empty slice at the end
    CHECK(s.firstByte == line.size());
    CHECK(s.endByte == line.size());
    s = visibleSlice("", 0.0f, 100.0f, adv);
    CHECK(s.firstByte == 0);
    CHECK(s.endByte == 0);
    // non-ASCII: byte offsets follow the characters
    const std::string wide = "ab\xF0\x9F\x98\x80" "cd";   // a b U+1F600 c d
    s = visibleSlice(wide, 15.0f, 35.0f, adv);
    CHECK(s.firstCol == 1);
    CHECK(s.firstByte == 1);
    CHECK(s.endCol == 3);            // c starts at 40 >= 35
    CHECK(s.endByte == 6);
}

TEST_CASE("gui editor: hit testing picks the row and the nearest column", "[gui][editor]") {
    wb::TextBuffer buffer;
    buffer.setText("first line\nsecond\n\nlast");
    const Advance adv = fixed(8.0f);
    const float lh = 16.0f;
    CHECK(hitTest(buffer, 0.0f, 0.0f, lh, adv) == (wb::Pos{0, 0}));
    CHECK(hitTest(buffer, 20.0f, 8.0f, lh, adv) == (wb::Pos{0, 3}));       // 20 is nearer to 24
    CHECK(hitTest(buffer, 20.0f, 16.0f, lh, adv) == (wb::Pos{1, 3}));
    CHECK(hitTest(buffer, 500.0f, 20.0f, lh, adv) == (wb::Pos{1, 6}));     // beyond the end of "second"
    CHECK(hitTest(buffer, 30.0f, 40.0f, lh, adv) == (wb::Pos{2, 0}));      // the empty line
    CHECK(hitTest(buffer, 4.0f, 999.0f, lh, adv) == (wb::Pos{3, 1}));      // below the text: the last line
    CHECK(hitTest(buffer, 4.0f, -30.0f, lh, adv) == (wb::Pos{0, 1}));      // above it: the first
    CHECK(hitTest(buffer, 4.0f, 5.0f, 0.0f, adv).row == 0);                // a zero line height must not divide
}

TEST_CASE("gui editor: word bounds for double-click", "[gui][editor]") {
    CHECK(wordBounds("SE students WHERE", 5) == std::make_pair(3, 11));
    CHECK(wordBounds("SE students WHERE", 3) == std::make_pair(3, 11));      // at the first letter
    CHECK(wordBounds("SE students WHERE", 2) == std::make_pair(2, 3));      // on the blank
    CHECK(wordBounds("a_b1 + c", 1) == std::make_pair(0, 4));               // underscore and digits belong to words
    CHECK(wordBounds("a_b1 + c", 5) == std::make_pair(5, 6));               // a run of punctuation
    CHECK(wordBounds("naam", 4) == std::make_pair(0, 4));                    // at the end: the last word
    CHECK(wordBounds("", 0) == std::make_pair(0, 0));
    CHECK(wordBounds("caf\xC3\xA9 x", 3) == std::make_pair(0, 4));          // accented letters are letters
    CHECK(wordBounds("x  y", 2) == std::make_pair(1, 3));
}

TEST_CASE("gui editor: syntax colours come from the theme", "[gui][editor]") {
    const Theme& th = darkTheme();
    CHECK(colorForKind(th, highlight::Kind::Keyword) == th.keyword);
    CHECK(colorForKind(th, highlight::Kind::String) == th.string);
    CHECK(colorForKind(th, highlight::Kind::Number) == th.number);
    CHECK(colorForKind(th, highlight::Kind::Boolean) == th.number);
    CHECK(colorForKind(th, highlight::Kind::ConstantBuiltin) == th.number);
    CHECK(colorForKind(th, highlight::Kind::Comment) == th.comment);
    CHECK(colorForKind(th, highlight::Kind::Function) == th.function);
    CHECK(colorForKind(th, highlight::Kind::Type) == th.type);
    CHECK(colorForKind(th, highlight::Kind::Operator) == th.text);
    CHECK(colorForKind(th, highlight::Kind::Bracket) == th.text);
}

// ---------------------------------------------------------------- undo

TEST_CASE("gui editor: undo merges typing until a pause or another kind of edit", "[gui][editor][undo]") {
    UndoStack stack;
    int taken = 0;
    auto before = [&](const char* text) { return [&taken, text] { ++taken; return snap(text); }; };
    CHECK(stack.record(true, 0.0, before("")));            // first character: a step
    CHECK_FALSE(stack.record(true, 0.2, before("a")));     // joins it
    CHECK_FALSE(stack.record(true, 0.9, before("ab")));    // 0.7 s after the previous one: still joined
    CHECK(taken == 1);                                      // the snapshot is only taken for a new step
    CHECK(stack.undoSteps() == 1);
    CHECK(stack.record(true, 2.5, before("abc")));         // a pause: a new step
    CHECK(stack.undoSteps() == 2);
    CHECK(stack.record(false, 2.6, before("abcd")));       // an edit that is not typing: its own step
    CHECK(stack.record(true, 2.7, before("abcde")));       // and typing after it does not join it
    CHECK_FALSE(stack.record(true, 2.8, before("abcdef")));
    stack.breakMerge();                                     // a cursor movement
    CHECK(stack.record(true, 2.9, before("abcdefg")));
    CHECK(stack.undoSteps() == 5);
}

TEST_CASE("gui editor: undo and redo walk the steps", "[gui][editor][undo]") {
    UndoStack stack;
    stack.record(false, 0.0, [] { return snap("one", 0, 3); });
    stack.record(false, 1.0, [] { return snap("one two", 0, 7); });
    auto step = stack.undo(snap("one two three", 0, 13));
    REQUIRE(step);
    CHECK(step->text == "one two");
    CHECK(stack.redoSteps() == 1);
    step = stack.undo(snap("one two", 0, 7));
    REQUIRE(step);
    CHECK(step->text == "one");
    CHECK(step->cursor == (wb::Pos{0, 3}));
    CHECK_FALSE(stack.undo(snap("one", 0, 3)));            // nothing left
    step = stack.redo(snap("one", 0, 3));
    REQUIRE(step);
    CHECK(step->text == "one two");
    step = stack.redo(snap("one two", 0, 7));
    REQUIRE(step);
    CHECK(step->text == "one two three");
    CHECK_FALSE(stack.redo(snap("one two three", 0, 13)));
    // a new edit clears what could be redone
    stack.undo(snap("one two three", 0, 13));
    CHECK(stack.redoSteps() == 1);
    stack.record(false, 5.0, [] { return snap("one two", 0, 7); });
    CHECK(stack.redoSteps() == 0);
}

TEST_CASE("gui editor: an edit that changed nothing can be cancelled, and the stack stays bounded", "[gui][editor][undo]") {
    UndoStack stack;
    CHECK(stack.record(false, 0.0, [] { return snap("x"); }));
    stack.cancelLast();
    CHECK(stack.undoSteps() == 0);
    for (std::size_t i = 0; i < UndoStack::kMaxSteps + 50; ++i) stack.record(false, static_cast<double>(i), [] { return snap("x"); });
    CHECK(stack.undoSteps() == UndoStack::kMaxSteps);
    stack.clear();
    CHECK(stack.undoSteps() == 0);
    // the byte budget drops the oldest steps first but keeps at least one
    const std::string big(UndoStack::kMaxBytes / 3 + 1, 'z');
    for (int i = 0; i < 4; ++i) stack.record(false, static_cast<double>(i), [&] { return snap(big); });
    CHECK(stack.undoSteps() <= 3);
    CHECK(stack.undoSteps() >= 1);
}

TEST_CASE("gui editor: snapshots restore text, cursor and selection", "[gui][editor][undo]") {
    wb::TextBuffer buffer;
    buffer.setText("alpha\nbeta");
    buffer.selectRange(wb::Pos{0, 1}, wb::Pos{1, 2});
    const EditorSnapshot taken = takeSnapshot(buffer);
    CHECK(taken.text == "alpha\nbeta");
    CHECK(taken.cursor == (wb::Pos{1, 2}));
    REQUIRE(taken.anchor);
    CHECK(*taken.anchor == (wb::Pos{0, 1}));
    buffer.setText("other");
    restoreSnapshot(buffer, taken);
    CHECK(buffer.text() == "alpha\nbeta");
    CHECK(buffer.cursor() == (wb::Pos{1, 2}));
    CHECK(buffer.selectedText() == "lpha\nbe");
    buffer.moveTo(wb::Pos{0, 3}, false);
    const EditorSnapshot plain = takeSnapshot(buffer);
    CHECK_FALSE(plain.anchor);
    buffer.selectAll();
    restoreSnapshot(buffer, plain);
    CHECK_FALSE(buffer.hasSelection());
    CHECK(buffer.cursor() == (wb::Pos{0, 3}));
}

// ---------------------------------------------------------------- the tree

namespace {

std::vector<wb::TreeRow> sampleTree(wb::TreeModel& model) {
    nlohmann::ordered_json dbs = nlohmann::ordered_json::parse(R"([
        {"name":"college","current":true,"tables":[
            {"name":"students","columns":[
                {"name":"id","type_name":"INT","primary_key":true,"not_null":true},
                {"name":"naam","type_name":"TEXT","not_null":true},
                {"name":"umar","type_name":"INT"}]},
            {"name":"courses","columns":[
                {"name":"id","type_name":"INT","primary_key":true},
                {"name":"title","type_name":"TEXT","unique":true}]}]},
        {"name":"main","tables":[]}])");
    model.refresh(dbs);
    return model.allNodes();
}

std::vector<std::string> names(const std::vector<TreeEntry>& entries) {
    std::vector<std::string> out;
    for (const TreeEntry& e : entries) out.push_back(e.name);
    return out;
}

}  // namespace

TEST_CASE("gui tree: matching is case-insensitive and an empty filter matches everything", "[gui][tree]") {
    CHECK(matchesFilter("Students", "stud"));
    CHECK(matchesFilter("students", "DEN"));
    CHECK_FALSE(matchesFilter("students", "x"));
    CHECK(matchesFilter("anything", ""));
    CHECK(matchesFilter("", ""));
    CHECK_FALSE(matchesFilter("", "a"));
}

TEST_CASE("gui tree: without a filter the entries follow what is expanded", "[gui][tree]") {
    wb::TreeModel model;
    const std::vector<wb::TreeRow> all = sampleTree(model);
    // the current database is open, its tables are closed
    CHECK(names(treeEntries(all, "")) == std::vector<std::string>{"college", "students", "courses", "main"});
    const std::vector<TreeEntry> entries = treeEntries(all, "");
    CHECK(entries[0].current);
    CHECK_FALSE(entries[3].current);
    CHECK(entries[0].expandable);
    CHECK(entries[0].expanded);
    CHECK_FALSE(entries[1].expanded);
    CHECK(entries[0].depth == 1);
    CHECK(entries[1].depth == 2);
    // opening a table lists its columns, with type and tags
    model.toggle(2);   // rows: Databases, college, students, courses, main
    const std::vector<TreeEntry> opened = treeEntries(model.allNodes(), "");
    REQUIRE(opened.size() == 7);
    CHECK(opened[2].name == "id");
    CHECK(opened[2].key.kind == wb::NodeKey::Kind::Column);
    CHECK(opened[2].type == "int");
    CHECK(opened[2].tags == std::vector<std::string>{"PK", "NN"});
    CHECK(opened[3].tags == std::vector<std::string>{"NN"});
    CHECK(opened[4].tags.empty());
    // the visible rows (already pruned) give the same list
    CHECK(names(treeEntries(model.rows(), "")) == names(opened));
    // the root is never listed
    for (const TreeEntry& e : opened) CHECK(e.key.kind != wb::NodeKey::Kind::Root);
}

TEST_CASE("gui tree: a filter keeps matches and their parents, and shows them open", "[gui][tree]") {
    wb::TreeModel model;
    const std::vector<wb::TreeRow> all = sampleTree(model);   // no table is expanded in the model
    // a column name: the column, its table and its database; the other tables and columns are hidden
    std::vector<TreeEntry> found = treeEntries(all, "NAAM");
    CHECK(names(found) == std::vector<std::string>{"college", "students", "naam"});
    CHECK(found[0].expanded);
    CHECK(found[1].expanded);
    // "id" is in two tables
    found = treeEntries(all, "id");
    CHECK(names(found) == std::vector<std::string>{"college", "students", "id", "courses", "id"});
    // a table name: the table and its database; its columns do not match so they are hidden, and it shows closed
    found = treeEntries(all, "stud");
    CHECK(names(found) == std::vector<std::string>{"college", "students"});
    CHECK(found[0].expanded);
    CHECK_FALSE(found[1].expanded);
    // a database name
    found = treeEntries(all, "MAIN");
    CHECK(names(found) == std::vector<std::string>{"main"});
    // several matches at every level
    found = treeEntries(all, "e");
    CHECK(names(found) == std::vector<std::string>{"college", "students", "courses", "title"});
    CHECK(treeEntries(all, "zzz").empty());
    CHECK(treeEntries({}, "a").empty());
}

TEST_CASE("gui tree: the context menu statements and names", "[gui][tree]") {
    wb::NodeKey table;
    table.kind = wb::NodeKey::Kind::Table;
    table.db = "college";
    table.table = "students";
    CHECK(topRowsScripts(table, "college") == std::vector<std::string>{"DIKHAO * SE students SIRF 100;"});
    CHECK(topRowsScripts(table, "main") == std::vector<std::string>{"ISTEMAL college;", "DIKHAO * SE students SIRF 100;"});
    wb::NodeKey column = table;
    column.kind = wb::NodeKey::Kind::Column;
    column.column = "naam";
    CHECK(topRowsScripts(column, "college").empty());
    CHECK(nodeName(table) == "students");
    CHECK(nodeName(column) == "naam");
    wb::NodeKey database;
    database.kind = wb::NodeKey::Kind::Database;
    database.db = "college";
    CHECK(nodeName(database) == "college");
    CHECK(nodeName(wb::NodeKey{}).empty());
}

// ---------------------------------------------------------------- the grid

TEST_CASE("gui grid: clipboard text, summary line, cell colours, initial widths", "[gui][grid]") {
    wb::ResultTable table;
    table.columns = {"id", "naam", "bio"};
    table.rows.push_back({{"1", wb::CellKind::Number}, {"Asha", wb::CellKind::Plain}, {"KHALI", wb::CellKind::Null}});
    table.rows.push_back({{"2", wb::CellKind::Number}, {"Ravi Kumar", wb::CellKind::Plain}, {std::string(500, 'x'), wb::CellKind::Plain}});
    CHECK(cellClipboardText(table, 0, 1) == "Asha");
    CHECK(cellClipboardText(table, 1, 0) == "2");
    CHECK(cellClipboardText(table, 5, 0).empty());
    CHECK(cellClipboardText(table, 0, 9).empty());
    CHECK(cellClipboardText(table, -1, 0).empty());
    CHECK(rowClipboardText(table, 0) == "1\tAsha\tKHALI");
    CHECK(rowClipboardText(table, 7).empty());
    CHECK(rowsSummary(12, 3.14) == "12 row(s), 3.1 ms");
    CHECK(rowsSummary(0, 0.0) == "0 row(s), 0.0 ms");
    CHECK(rowsSummary(5, -1.0) == "5 row(s)");
    const Theme& th = lightTheme();
    CHECK(colorForCell(th, wb::CellKind::Null) == th.faint);
    CHECK(colorForCell(th, wb::CellKind::True) == th.success);
    CHECK(colorForCell(th, wb::CellKind::False) == th.error);
    CHECK(colorForCell(th, wb::CellKind::Number) == th.number);
    CHECK(colorForCell(th, wb::CellKind::Plain) == th.text);
    const std::vector<int> chars = initialColumnChars(table);
    REQUIRE(chars.size() == 3);
    CHECK(chars[0] == 4);     // at least 4
    CHECK(chars[1] == 10);
    CHECK(chars[2] == 60);    // at most 60
    CHECK(initialColumnChars(wb::ResultTable{}).empty());
}

// ---------------------------------------------------------------- the window

TEST_CASE("gui ui: the breadcrumb shows the last folder of a local data folder", "[gui][ui]") {
    CHECK(connectionLabel("local (C:\\Users\\x\\AppData\\Local\\MeraDB\\data)") == "local (data)");
    CHECK(connectionLabel("local (/home/x/meradb/data/)") == "local (data)");
    CHECK(connectionLabel("local (data)") == "local (data)");
    CHECK(connectionLabel("local ()") == "local");        // no folder name to show
    CHECK(connectionLabel("127.0.0.1:6372") == "127.0.0.1:6372");
    CHECK(connectionLabel("db.example.com:6372") == "db.example.com:6372");
    CHECK(connectionLabel("") == "");
}

TEST_CASE("gui ui: the key table has the keys of the plan", "[gui][ui]") {
    auto action = [](ImGuiKeyChord chord) -> int {
        for (const KeyBinding& b : keyBindings())
            if (b.chord == chord) return static_cast<int>(b.action);
        return -1;
    };
    CHECK(action(ImGuiMod_Ctrl | ImGuiKey_Enter) == static_cast<int>(Action::Run));
    CHECK(action(ImGuiKey_F5) == static_cast<int>(Action::Run));
    CHECK(action(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Enter) == static_cast<int>(Action::Explain));
    CHECK(action(ImGuiKey_F6) == static_cast<int>(Action::Explain));
    CHECK(action(ImGuiMod_Alt | ImGuiKey_UpArrow) == static_cast<int>(Action::Previous));
    CHECK(action(ImGuiMod_Ctrl | ImGuiKey_UpArrow) == static_cast<int>(Action::Previous));
    CHECK(action(ImGuiMod_Alt | ImGuiKey_DownArrow) == static_cast<int>(Action::Next));
    CHECK(action(ImGuiMod_Ctrl | ImGuiKey_DownArrow) == static_cast<int>(Action::Next));
    CHECK(action(ImGuiMod_Ctrl | ImGuiKey_S) == static_cast<int>(Action::ExportCsv));
    CHECK(action(ImGuiMod_Ctrl | ImGuiKey_L) == static_cast<int>(Action::ClearLog));
    CHECK(action(ImGuiMod_Ctrl | ImGuiKey_Q) == static_cast<int>(Action::Quit));
    CHECK(action(ImGuiKey_Q) == -1);
    for (std::size_t i = 0; i < keyBindings().size(); ++i)   // no chord twice
        for (std::size_t j = i + 1; j < keyBindings().size(); ++j) CHECK(keyBindings()[i].chord != keyBindings()[j].chord);
}

TEST_CASE("gui options: the hidden editor and expand-tables options", "[gui][options]") {
    GuiOptions o = parseGuiArgs({"--local", "--screenshot", "a.png", "--editor", "DIKHAO 1;", "--expand-tables"});
    CHECK(o.error.empty());
    REQUIRE(o.editorText);
    CHECK(*o.editorText == "DIKHAO 1;");
    CHECK(o.expandTables);
    o = parseGuiArgs({"--local"});
    CHECK_FALSE(o.editorText);
    CHECK_FALSE(o.expandTables);
    o = parseGuiArgs({"--editor"});
    CHECK_FALSE(o.error.empty());
}

// ---------------------------------------------------------------- very long lines: LineMetrics

namespace {

std::string randomLine(unsigned seed, std::size_t length, bool wide) {
    std::string line;
    unsigned x = seed;
    auto next = [&] {
        x = x * 1664525u + 1013904223u;
        return x >> 8;
    };
    const char* pieces[] = {"DIKHAO ", "naam", ", ", "'ab cd'", "'it''s'", "42 ", "3.5", "(", ")", " ", "= ", "--c", "x_1 ", "SE "};
    const char* multi[] = {"\xE3\x81\x82", "\xC3\xA9", "\xE0\xA4\xA8", "\xF0\x9F\x98\x80"};
    while (line.size() < length) {
        if (wide && next() % 7 == 0) line += multi[next() % 4];
        else line += pieces[next() % (sizeof pieces / sizeof pieces[0])];
    }
    return line;
}

}  // namespace

TEST_CASE("gui long lines: LineMetrics agrees with the plain functions (ASCII and mixed text)", "[gui][editor][longline]") {
    for (const bool wide : {false, true}) {
        const Advance adv = fixed(7.0f);
        const std::string line = randomLine(wide ? 11u : 5u, 20000, wide);
        LineMetrics m;
        m.build(line, adv);
        REQUIRE(m.matches(line, adv));
        CHECK(m.columns() == static_cast<int>(std::count_if(line.begin(), line.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; })));
        CHECK(m.totalWidth() == Catch::Approx(static_cast<double>(textWidth(line.data(), line.data() + line.size(), adv))).margin(0.5));
        unsigned seed = 99;
        auto rnd = [&](unsigned n) {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) % n;
        };
        for (int i = 0; i < 400; ++i) {
            const int col = static_cast<int>(rnd(static_cast<unsigned>(m.columns() + 40)));
            CHECK(m.xOfColumn(line, col, adv) == Catch::Approx(static_cast<double>(columnX(line, col, adv))).margin(0.5));
            const float x = static_cast<float>(rnd(static_cast<unsigned>(m.totalWidth()) + 200)) - 20.0f;
            CHECK(m.columnAtX(line, static_cast<double>(x), adv) == columnAt(line, x, adv));
            const float from = static_cast<float>(rnd(static_cast<unsigned>(m.totalWidth()) + 100)) - 30.0f;
            const float to = from + static_cast<float>(rnd(900));
            const Slice a = m.slice(line, from, to, adv);
            const Slice b = visibleSlice(line, from, to, adv);
            CHECK(a.firstCol == b.firstCol);
            CHECK(a.endCol == b.endCol);
            CHECK(a.firstByte == b.firstByte);
            CHECK(a.endByte == b.endByte);
            CHECK(a.firstX == Catch::Approx(static_cast<double>(b.firstX)).margin(0.5));
        }
    }
}

TEST_CASE("gui long lines: lexing from a sync point gives the same colours as lexing from the start", "[gui][editor][longline]") {
    const Advance adv = fixed(7.0f);
    // long strings and comments, so that sync points must skip them
    std::string line = randomLine(3u, 30000, true);
    line += " '" + std::string(9000, 'q') + " ''" + std::string(9000, 'w') + "' " + randomLine(4u, 12000, false) + " --" + randomLine(5u, 9000, false);
    LineMetrics m;
    m.build(line, adv);
    unsigned seed = 7;
    auto rnd = [&](std::size_t n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % n;
    };
    for (int i = 0; i < 300; ++i) {
        std::size_t from = rnd(line.size());
        while (from > 0 && (static_cast<unsigned char>(line[from]) & 0xC0) == 0x80) --from;
        std::size_t to = std::min(line.size(), from + 600);
        while (to < line.size() && (static_cast<unsigned char>(line[to]) & 0xC0) == 0x80) ++to;
        const std::size_t sync = m.syncBefore(from);
        CHECK(sync <= from);
        const auto full = highlight::spans(line, from, to);
        const auto fast = highlight::spans(line, sync, from, to);
        REQUIRE(full.size() == fast.size());
        for (std::size_t k = 0; k < full.size(); ++k) {
            CHECK(full[k].start == fast[k].start);
            CHECK(full[k].end == fast[k].end);
            CHECK(full[k].kind == fast[k].kind);
        }
    }
}

TEST_CASE("gui long lines: a line of 30 million columns keeps exact positions", "[gui][editor][longline]") {
    const Advance adv = fixed(6.0f);
    const std::string line(30000000, 'x');
    LineMetrics m;
    m.build(line, adv);
    // 6 px * 30M = 180M px: far beyond what a float holds exactly; the double-based answers stay exact
    CHECK(m.totalWidth() == 180000000.0);
    CHECK(m.xOfColumn(line, 29999999, adv) == 179999994.0);
    const double scroll = 179999000.0;
    const Slice s = m.slice(line, scroll - 6.0, scroll + 1000.0, adv);
    CHECK(s.firstCol == 29999832);
    CHECK(s.firstX - scroll == Catch::Approx(-8.0).margin(0.001));   // relative to the scroll the number is small again
    CHECK(m.columnAtX(line, 179999994.0 + 2.0, adv) == 29999999);
}

TEST_CASE("gui undo: an edit that changes nothing keeps the redo steps", "[gui][editor][undo]") {
    UndoStack u;
    u.record(false, 0.0, [] { return snap("a"); });
    u.record(false, 5.0, [] { return snap("ab"); });
    auto step = u.undo(snap("abc"));
    REQUIRE(step);
    REQUIRE(u.redoSteps() == 1);
    const bool started = u.record(false, 10.0, [] { return snap("ab"); });   // a no-op edit begins a step...
    CHECK(started);
    CHECK(u.redoSteps() == 0);
    u.cancelLast();                                                          // ...and is found to change nothing
    CHECK(u.redoSteps() == 1);
    CHECK(u.undoSteps() == 1);
    auto again = u.redo(snap("ab"));
    REQUIRE(again);
    CHECK(again->text == "abc");
}

// ---------------------------------------------------------------- fallback fonts, IME, window size

TEST_CASE("gui fonts: the fallback list only names files that exist", "[gui][fonts]") {
    for (const FallbackFont& f : systemFallbackFonts()) {
        std::FILE* file = std::fopen(f.path.c_str(), "rb");
        CHECK(file != nullptr);
        if (file != nullptr) std::fclose(file);
        CHECK(f.index >= 0);
    }
    CHECK(systemFallbackFonts("/this/folder/does/not/exist").empty());
}

TEST_CASE("gui ime: the composition starts at the caret and the candidate list hangs below it", "[gui][ime]") {
    const ImePlacement p = imePlacement(120.5f, 300.0f, 18.0f);
    CHECK(p.compositionX == 120);
    CHECK(p.compositionY == 300);
    CHECK(p.candidateX == 120);
    CHECK(p.candidateY > 318);
    CHECK(p.candidateY < 330);
}

TEST_CASE("gui window: the minimum size is 640 x 400 logical pixels at any scale", "[gui][window]") {
    CHECK(minimumWindowSize(1.0f).width == 640);
    CHECK(minimumWindowSize(1.0f).height == 400);
    CHECK(minimumWindowSize(2.0f).width == 1280);   // at 200% the top bar and the tabs keep the room they have at 100%
    CHECK(minimumWindowSize(2.0f).height == 800);
    CHECK(minimumWindowSize(1.5f).width == 960);
    CHECK(minimumWindowSize(0.0f).width == 640);    // nonsense scales fall back to 1
    CHECK(minimumWindowSize(100.0f).width == 3840);
}
