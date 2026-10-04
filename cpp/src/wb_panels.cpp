// cpp/src/wb_panels.cpp
#include "meradb/wb_panels.h"
#include "meradb/wb_keys.h"
#include <algorithm>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <utility>

namespace meradb::wb {

using namespace ftxui;

namespace {

Color rgbColor(int rgb) {
    return Color::RGB(static_cast<std::uint8_t>((rgb >> 16) & 255), static_cast<std::uint8_t>((rgb >> 8) & 255),
                      static_cast<std::uint8_t>(rgb & 255));
}

// Control characters would reach the terminal as commands (ESC, and the C1 set U+0080..U+009F, whose UTF-8 form is
// C2 80..C2 9F: U+009B is a CSI) or break the column arithmetic (tab): show a space (one for each C1 character).
bool isC1At(const std::string& s, std::size_t i) {
    return static_cast<unsigned char>(s[i]) == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 &&
           static_cast<unsigned char>(s[i + 1]) <= 0x9F;
}
std::string printable(const std::string& s) {
    bool clean = true;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char u = static_cast<unsigned char>(s[i]);
        if (u < 0x20 || u == 0x7f || isC1At(s, i)) {
            clean = false;
            break;
        }
    }
    if (clean) return s;
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char u = static_cast<unsigned char>(s[i]);
        if (u < 0x20 || u == 0x7f) {
            out += ' ';
        } else if (isC1At(s, i)) {
            out += ' ';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---- the frame ----

const char* const kRound[6] = {"╭", "╮", "╰", "╯", "─", "│"};
const char* const kHeavy[6] = {"┏", "┓", "┗", "┛", "━", "┃"};

class FrameNode : public Node {
public:
    FrameNode(std::string title, int rgb, bool heavy, Element fixed, std::function<Element(int, int)> builder,
              Box* out)
        : title_(std::move(title)), rgb_(rgb), heavy_(heavy), fixed_(std::move(fixed)), builder_(std::move(builder)),
          out_(out) {}

    void ComputeRequirement() override {
        requirement_ = Requirement();
        if (fixed_) {
            fixed_->ComputeRequirement();
            requirement_ = fixed_->requirement();
            requirement_.selection = Requirement::NORMAL;
        } else {
            requirement_.flex_grow_x = 1;
            requirement_.flex_grow_y = 1;
        }
        requirement_.min_x = std::max(requirement_.min_x, 0) + 2;
        requirement_.min_y = std::max(requirement_.min_y, 0) + 2;
    }

    void SetBox(Box box) override {
        box_ = box;
        if (out_) *out_ = box;
        inner_ = Box{box.x_min + 1, box.x_max - 1, box.y_min + 1, box.y_max - 1};
        const int w = inner_.x_max - inner_.x_min + 1;
        const int h = inner_.y_max - inner_.y_min + 1;
        body_.reset();
        if (w <= 0 || h <= 0) return;
        if (builder_) body_ = builder_(w, h);
        else body_ = fixed_;
        if (body_) {
            body_->ComputeRequirement();
            body_->SetBox(inner_);
        }
    }

    void Render(Screen& screen) override {
        if (box_.x_max < box_.x_min || box_.y_max < box_.y_min) return;
        if (body_) {
            const Box saved = screen.stencil;
            screen.stencil = Box::Intersection(saved, inner_);
            body_->Render(screen);
            screen.stencil = saved;
        }
        const char* const* chars = heavy_ ? kHeavy : kRound;
        const Color color = rgbColor(rgb_);
        auto put = [&](int x, int y, const char* glyph) {
            Pixel& p = screen.PixelAt(x, y);
            p.character = glyph;
            p.foreground_color = color;
            p.bold = heavy_;
        };
        for (int x = box_.x_min + 1; x < box_.x_max; ++x) {
            put(x, box_.y_min, chars[4]);
            put(x, box_.y_max, chars[4]);
        }
        for (int y = box_.y_min + 1; y < box_.y_max; ++y) {
            put(box_.x_min, y, chars[5]);
            put(box_.x_max, y, chars[5]);
        }
        if (box_.x_max > box_.x_min && box_.y_max > box_.y_min) {
            put(box_.x_min, box_.y_min, chars[0]);
            put(box_.x_max, box_.y_min, chars[1]);
            put(box_.x_min, box_.y_max, chars[2]);
            put(box_.x_max, box_.y_max, chars[3]);
        }
        if (!title_.empty() && box_.x_max - box_.x_min >= 3) {
            Element t = text(" " + printable(title_) + " ");
            t->ComputeRequirement();
            t->SetBox(Box{box_.x_min + 1, box_.x_max - 1, box_.y_min, box_.y_min});
            t->Render(screen);
            for (int x = box_.x_min + 1; x < box_.x_max; ++x) {
                Pixel& p = screen.PixelAt(x, box_.y_min);
                p.foreground_color = color;
                p.bold = heavy_;
            }
        }
    }

private:
    std::string title_;
    int rgb_;
    bool heavy_;
    Element fixed_;
    std::function<Element(int, int)> builder_;
    Box* out_;
    Box inner_;
    Element body_;
};

// An element whose content is built when its final size is known.
class SizedNode : public Node {
public:
    explicit SizedNode(std::function<Element(int, int)> builder) : builder_(std::move(builder)) {}
    void ComputeRequirement() override {
        requirement_ = Requirement();
        requirement_.flex_grow_x = 1;
        requirement_.flex_grow_y = 1;
    }
    void SetBox(Box box) override {
        box_ = box;
        body_.reset();
        const int w = box.x_max - box.x_min + 1;
        const int h = box.y_max - box.y_min + 1;
        if (w <= 0 || h <= 0) return;
        body_ = builder_(w, h);
        if (body_) {
            body_->ComputeRequirement();
            body_->SetBox(box);
        }
    }
    void Render(Screen& screen) override {
        if (!body_) return;
        const Box saved = screen.stencil;
        screen.stencil = Box::Intersection(saved, box_);
        body_->Render(screen);
        screen.stencil = saved;
    }

private:
    std::function<Element(int, int)> builder_;
    Element body_;
};

int boxWidth(const Box& b) { return b.x_max - b.x_min + 1; }
int boxHeight(const Box& b) { return b.y_max - b.y_min + 1; }

Element rowWithBackground(Element row, int rgb) { return bgcolor(rgbColor(rgb), std::move(row)); }

}  // namespace

// ---- public helpers ----

Element lineToElement(const Line& line) {
    if (line.empty()) return text("");
    Elements parts;
    parts.reserve(line.size());
    for (const Segment& seg : line) {
        Element e = text(printable(seg.text));
        const Style& s = seg.style;
        if (s.fg >= 0) e = color(rgbColor(s.fg), e);
        if (s.bg >= 0) e = bgcolor(rgbColor(s.bg), e);
        if (s.bold) e = bold(e);
        if (s.dim) e = dim(e);
        // FTXUI 5.0.0 has no italic attribute: an italic style is drawn with its other attributes only.
        if (s.underline) e = underlined(e);
        if (s.inverse) e = inverted(e);
        parts.push_back(e);
    }
    if (parts.size() == 1) return parts[0];
    return hbox(std::move(parts));
}

Element panelFrame(const std::string& title, Element content, bool focused, int accentRgb) {
    return std::make_shared<FrameNode>(title, focused ? palette::kYellow : accentRgb, focused, std::move(content),
                                       std::function<Element(int, int)>(), nullptr);
}

Element sizedFrame(const std::string& title, int borderRgb, bool heavy, Box* box,
                   std::function<Element(int, int)> body) {
    return std::make_shared<FrameNode>(title, borderRgb, heavy, Element(), std::move(body), box);
}

Element sizedElement(std::function<Element(int, int)> body) { return std::make_shared<SizedNode>(std::move(body)); }

PanelView::PanelView() {
    box_.x_max = -1;  // "not laid out yet"
    box_.y_max = -1;
}

int PanelView::innerHeight() const {
    if (box_.x_max < box_.x_min) return 10;
    return std::max(1, boxHeight(box_) - 2);
}

int PanelView::innerWidth() const {
    if (box_.x_max < box_.x_min) return 40;
    return std::max(1, boxWidth(box_) - 2);
}

// ---- the schema tree ----

namespace {

class TreePanel : public PanelView {
public:
    explicit TreePanel(Session& session) : session_(session) {}

    Element Render() override {
        const bool focused = focused_;
        return sizedFrame("Schema", focused ? palette::kYellow : palette::kPurple, focused, &box_,
                          [this, focused](int w, int h) { return body(w, h, focused); });
    }

    bool OnEvent(Event e) override {
        TreeModel& tree = session_.tree();
        sync();
        if (e == Event::ArrowDown) tree.moveSelection(1);
        else if (e == Event::ArrowUp) tree.moveSelection(-1);
        else if (e == Event::PageDown) tree.scroll().pageCursor(1);
        else if (e == Event::PageUp) tree.scroll().pageCursor(-1);
        else if (e == Event::Home) tree.select(0);
        else if (e == Event::End) tree.select(static_cast<int>(tree.rows().size()) - 1);
        else if (e == Event::ArrowRight) tree.expandOrDescend();
        else if (e == Event::ArrowLeft) tree.collapseOrAscend();
        else if (e == Event::Return) session_.activateTreeRow(tree.selected());
        else if (e == Event::Character(" ")) tree.toggle(tree.selected());
        else return false;
        return true;
    }

    void scrollWheel(int direction) override {
        sync();
        session_.tree().scroll().scrollBy(direction * 3);
    }

    void click(int /*x*/, int y) override {
        TreeModel& tree = session_.tree();
        sync();
        const int row = tree.scroll().top() + (y - box_.y_min - 1);
        if (row >= 0 && row < static_cast<int>(tree.rows().size())) tree.select(row);
    }

private:
    void sync() {
        ScrollState& scroll = session_.tree().scroll();
        scroll.setCount(static_cast<int>(session_.tree().rows().size()));
        scroll.setHeight(innerHeight());
        scroll.ensureVisible(scroll.cursor());
    }

    Element body(int w, int h, bool focused) {
        TreeModel& tree = session_.tree();
        ScrollState& scroll = tree.scroll();
        const std::vector<TreeRow>& rows = tree.rows();
        scroll.setCount(static_cast<int>(rows.size()));
        scroll.setHeight(h);
        scroll.ensureVisible(scroll.cursor());
        Elements lines;
        const int last = std::min(static_cast<int>(rows.size()), scroll.top() + h);
        for (int i = scroll.top(); i < last; ++i) {
            const TreeRow& r = rows[static_cast<std::size_t>(i)];
            std::string lead(static_cast<std::size_t>(2 * r.depth), ' ');
            lead += r.expandable ? (r.expanded ? "▼ " : "▶ ") : "  ";
            const int room = std::max(0, w - 2 * r.depth - 2);
            Elements parts;
            parts.push_back(text(lead));
            parts.push_back(lineToElement(clipLine(r.label, 0, room)));
            Element row = hbox(std::move(parts));
            if (i == scroll.cursor()) row = rowWithBackground(row, focused ? palette::kCurrentLine : palette::kStripe);
            lines.push_back(row);
        }
        return vbox(std::move(lines));
    }

    Session& session_;
};

// ---- the results table ----

class ResultsPanel : public PanelView {
public:
    explicit ResultsPanel(Session& session) : session_(session) {}

    Element Render() override {
        const bool focused = focused_;
        const std::string title = resultsTitle(session_.table());
        return sizedFrame(title, focused ? palette::kYellow : palette::kCyan, focused, &box_,
                          [this, focused](int w, int h) { return body(w, h, focused); });
    }

    bool OnEvent(Event e) override {
        refresh();
        if (e == Event::ArrowDown) scroll_.moveCursor(1);
        else if (e == Event::ArrowUp) scroll_.moveCursor(-1);
        else if (e == Event::PageDown) scroll_.pageCursor(1);
        else if (e == Event::PageUp) scroll_.pageCursor(-1);
        else if (e == Event::Home) scroll_.setCursor(0);
        else if (e == Event::End) scroll_.setCursor(scroll_.count() - 1);
        else if (e == Event::ArrowRight) xOffset_ = std::min(std::max(0, totalWidth_ - innerWidth()), xOffset_ + 4);
        else if (e == Event::ArrowLeft) xOffset_ = std::max(0, xOffset_ - 4);
        else return false;
        return true;
    }

    void scrollWheel(int direction) override {
        refresh();
        scroll_.scrollBy(direction * 3);
    }

    void click(int /*x*/, int y) override {
        refresh();
        const int row = scroll_.top() + (y - box_.y_min - 2);   // frame line, then the header line
        if (row >= 0 && row < scroll_.count()) scroll_.setCursor(row);
    }

private:
    // New table: forget the cursor and the sideways offset, recompute the column widths.
    void refresh() {
        const ResultTable* table = session_.table();
        if (session_.resultVersion() != version_ || table != table_) {
            version_ = session_.resultVersion();
            table_ = table;
            xOffset_ = 0;
            widths_.clear();
            totalWidth_ = 0;
            scroll_ = ScrollState();
            if (table) {
                widths_ = columnWidths(*table);
                for (int w : widths_) totalWidth_ += w + 2;
                scroll_.setCount(static_cast<int>(table->rows.size()));
            }
        }
        scroll_.setHeight(std::max(1, innerHeight() - 1));
        scroll_.ensureVisible(scroll_.cursor());
    }

    Line renderCells(const std::vector<std::string>& texts, const std::vector<Cell>* cells, bool header) const {
        Line line;
        for (std::size_t c = 0; c < widths_.size(); ++c) {
            const std::string& t = texts.empty() ? (*cells)[c].text : texts[c];
            const int width = widths_[c];
            const int pad = std::max(0, width - string_width(t));
            const bool right = !header && cells && cellRightAligned((*cells)[c].kind);
            Style style;
            if (header) style.bold = true;
            else if (cells) style = cellStyle((*cells)[c].kind);
            Style plain;
            if (header) plain.bold = true;
            appendSegment(line, right ? " " + std::string(static_cast<std::size_t>(pad), ' ') : " ", plain);
            appendSegment(line, t, style);
            appendSegment(line, right ? " " : std::string(static_cast<std::size_t>(pad), ' ') + " ", plain);
        }
        return line;
    }

    Element body(int w, int h, bool focused) {
        refresh();
        const ResultTable* table = session_.table();
        if (!table) return vbox(Elements());
        xOffset_ = std::max(0, std::min(xOffset_, totalWidth_ - w));
        scroll_.setHeight(std::max(1, h - 1));
        scroll_.ensureVisible(scroll_.cursor());
        Elements lines;
        {
            Line head = renderCells(table->columns, nullptr, true);
            Element row = lineToElement(clipLine(head, xOffset_, w));
            lines.push_back(rowWithBackground(row, 0x3b3e52));
        }
        const int last = std::min(scroll_.count(), scroll_.top() + std::max(0, h - 1));
        for (int i = scroll_.top(); i < last; ++i) {
            const std::vector<Cell>& cells = table->rows[static_cast<std::size_t>(i)];
            Line line = renderCells(std::vector<std::string>(), &cells, false);
            Element row = lineToElement(clipLine(line, xOffset_, w));
            if (i == scroll_.cursor() && focused) row = rowWithBackground(row, palette::kCurrentLine);
            else if (i % 2 == 1) row = rowWithBackground(row, palette::kStripe);
            lines.push_back(row);
        }
        return vbox(std::move(lines));
    }

    Session& session_;
    int version_ = -1;
    const ResultTable* table_ = nullptr;
    std::vector<int> widths_;
    int totalWidth_ = 0;
    int xOffset_ = 0;
    ScrollState scroll_;
};

// ---- the log ----

class LogPanel : public PanelView {
public:
    explicit LogPanel(Session& session) : session_(session) {}

    Element Render() override {
        const bool focused = focused_;
        return sizedFrame("Log", focused ? palette::kYellow : palette::kPink, focused, &box_,
                          [this](int w, int h) { return body(w, h); });
    }

    bool OnEvent(Event e) override {
        sync(innerWidth(), innerHeight());
        if (e == Event::ArrowUp) scrollBy(-1);
        else if (e == Event::ArrowDown) scrollBy(1);
        else if (e == Event::PageUp) scrollBy(-std::max(1, innerHeight() - 1));
        else if (e == Event::PageDown) scrollBy(std::max(1, innerHeight() - 1));
        else if (e == Event::Home) {
            follow_ = false;
            scroll_.scrollBy(-scroll_.count());
        } else if (e == Event::End) {
            follow_ = true;
            scroll_.followEnd();
        } else return false;
        return true;
    }

    void scrollWheel(int direction) override {
        sync(innerWidth(), innerHeight());
        scrollBy(direction * 3);
    }

private:
    void scrollBy(int delta) {
        scroll_.scrollBy(delta);
        follow_ = scroll_.atBottom();
    }

    // Keeps wrapped_ in step with the session's log: rebuilt when the log was cleared or the width changed, extended
    // when entries were added, and shortened at the front (by the wrapped height of each dropped entry) when the log
    // trimmed its oldest entries, so a full log (20,000 lines) costs only the new lines per statement.
    void sync(int w, int h) {
        const LogBuffer& log = session_.log();
        w = std::max(1, w);
        if (log.generation() != generation_ || w != width_) {
            wrapped_.clear();
            counts_.clear();
            generation_ = log.generation();
            trimmedSeen_ = log.trimmedEntries();
            width_ = w;
            revision_ = static_cast<std::size_t>(-1);
        }
        if (log.trimmedEntries() != trimmedSeen_) {
            std::size_t drop = log.trimmedEntries() - trimmedSeen_;
            trimmedSeen_ = log.trimmedEntries();
            std::size_t droppedLines = 0;
            for (; drop > 0 && !counts_.empty(); --drop) {   // entries never wrapped have nothing to remove
                droppedLines += counts_.front();
                wrapped_.erase(wrapped_.begin(), wrapped_.begin() + static_cast<std::ptrdiff_t>(counts_.front()));
                counts_.pop_front();
            }
            if (!follow_) scroll_.scrollBy(-static_cast<int>(droppedLines));   // keep the same text in view
        }
        if (log.revision() != revision_) {
            const std::deque<LogEntry>& entries = log.entries();
            for (std::size_t i = counts_.size(); i < entries.size(); ++i) {
                std::size_t count = 0;
                for (const Line& line : entries[i].lines) {
                    std::vector<Line> pieces = wrapLine(line, width_);
                    count += pieces.size();
                    for (Line& piece : pieces) wrapped_.push_back(std::move(piece));
                }
                counts_.push_back(count);
            }
            revision_ = log.revision();
        }
        scroll_.setCount(static_cast<int>(wrapped_.size()));
        scroll_.setHeight(h);
        if (follow_) scroll_.followEnd();
    }

    Element body(int w, int h) {
        sync(w, h);
        Elements lines;
        const int last = std::min(scroll_.count(), scroll_.top() + h);
        for (int i = scroll_.top(); i < last; ++i) lines.push_back(lineToElement(wrapped_[static_cast<std::size_t>(i)]));
        return vbox(std::move(lines));
    }

    Session& session_;
    std::deque<Line> wrapped_;
    std::deque<std::size_t> counts_;   // wrapped lines of each entry already in wrapped_, oldest first
    std::size_t trimmedSeen_ = 0;
    std::size_t generation_ = static_cast<std::size_t>(-1);
    std::size_t revision_ = static_cast<std::size_t>(-1);
    int width_ = -1;
    bool follow_ = true;
    ScrollState scroll_;
};

// ---- the query editor ----

constexpr const char* kPlaceholder = "Yahan query likho, jaise:  DIKHAO * SE students;   (F5 se chalao)";

class EditorPanel : public PanelView {
public:
    explicit EditorPanel(Session& session) : session_(session) {}

    Element Render() override {
        const bool focused = focused_;
        return sizedFrame("Query  [F5 = chalao, F6 = samjhao]", focused ? palette::kYellow : palette::kGreen, focused,
                          &box_, [this, focused](int w, int h) { return body(w, h, focused); });
    }

    bool OnEvent(Event e) override {
        TextBuffer& buffer = session_.editor();
        if (e == Event::Return) buffer.newline();
        else if (e == Event::Backspace) buffer.backspace();
        else if (e == Event::Delete) buffer.del();
        else if (keys::isSelectAll(e)) buffer.selectAll();
        else if (const keys::EditorKey* k = keys::findEditorKey(e)) buffer.move(k->move, k->extendSelection);
        else if (e.is_character()) buffer.insert(e.character());
        else return false;
        return true;
    }

private:
    Element body(int w, int h, bool focused) {
        TextBuffer& buffer = session_.editor();
        const int lineCount = buffer.lineCount();
        const int gutter = std::min(gutterWidth(lineCount), std::max(0, w - 1));
        const int bodyWidth = std::max(1, w - gutter);

        // Keep the cursor in view.
        const Pos cursor = buffer.cursor();
        top_ = std::max(0, std::min(top_, lineCount - h));
        if (cursor.row < top_) top_ = cursor.row;
        if (cursor.row >= top_ + h) top_ = cursor.row - h + 1;
        const std::string& cursorLine = buffer.line(cursor.row);
        const int column = displayColumn(cursorLine, cursor.col);
        int cursorCells = 1;
        if (cursor.col < buffer.lineLength(cursor.row)) {
            const std::size_t at = buffer.byteOffset(cursor.row, cursor.col);
            if (at < cursorLine.size() && static_cast<unsigned char>(cursorLine[at]) >= 0x80) cursorCells = 2;
        }
        if (column < left_) left_ = column;
        if (column + cursorCells > left_ + bodyWidth) left_ = column + cursorCells - bodyWidth;
        left_ = std::max(0, left_);

        Elements lines;
        const int last = std::min(lineCount, top_ + h);
        for (int row = top_; row < last; ++row) {
            Elements parts;
            if (gutter > 0) {
                Style style = fgStyle(palette::kText);
                if (row != cursor.row) style = fgStyle(palette::kText, false, true);
                Line number;
                appendSegment(number, gutterText(row, lineCount), style);
                parts.push_back(lineToElement(clipLine(number, 0, gutter)));
            }
            Line rowLine = editorRowWindow(buffer, row, focused, left_, bodyWidth);
            if (buffer.empty() && row == 0) {
                const int used = static_cast<int>(plainText(rowLine).size());   // the cursor cell, when focused
                Line hint;
                appendSegment(hint, kPlaceholder, fgStyle(palette::kComment, false, true));
                const Line clipped = clipLine(hint, 0, std::max(0, bodyWidth - used));
                for (const Segment& seg : clipped) rowLine.push_back(seg);
            }
            parts.push_back(lineToElement(rowLine));
            lines.push_back(hbox(std::move(parts)));
        }
        return vbox(std::move(lines));
    }

    Session& session_;
    int top_ = 0;
    int left_ = 0;
};

}  // namespace

std::shared_ptr<PanelView> makeTreePanel(Session& session) { return std::make_shared<TreePanel>(session); }
std::shared_ptr<PanelView> makeResultsPanel(Session& session) { return std::make_shared<ResultsPanel>(session); }
std::shared_ptr<PanelView> makeLogPanel(Session& session) { return std::make_shared<LogPanel>(session); }
std::shared_ptr<PanelView> makeEditorPanel(Session& session) { return std::make_shared<EditorPanel>(session); }

}  // namespace meradb::wb
