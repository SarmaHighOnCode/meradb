// cpp/src/gui_editor.cpp -- see gui_editor.h.
#include "meradb/gui_editor.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>

namespace meradb::gui {

// ---------------------------------------------------------------- text geometry

char32_t decodeUtf8(const char*& p, const char* end) {
    const unsigned char lead = static_cast<unsigned char>(*p);
    std::size_t length = 1;
    if (lead >= 0xF0 && lead < 0xF8) length = 4;
    else if (lead >= 0xE0) length = lead < 0xF0 ? 3 : 1;
    else if (lead >= 0xC0) length = 2;
    length = std::min<std::size_t>(length, static_cast<std::size_t>(end - p));
    char32_t cp = lead;
    if (length == 1) {
        cp = lead < 0x80 ? lead : 0xFFFD;
    } else {
        cp = lead & (length == 2 ? 0x1F : (length == 3 ? 0x0F : 0x07));
        for (std::size_t i = 1; i < length; ++i) cp = (cp << 6) | (static_cast<unsigned char>(p[i]) & 0x3F);
    }
    p += length;
    return cp;
}

std::string encodeUtf8(char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

float textWidth(const char* begin, const char* end, const Advance& advance) {
    float width = 0.0f;
    const char* p = begin;
    while (p < end) width += advance.of(decodeUtf8(p, end));
    return width;
}

float columnX(const std::string& line, int col, const Advance& advance) {
    const char* p = line.data();
    const char* end = p + line.size();
    float x = 0.0f;
    for (int c = 0; c < col && p < end; ++c) x += advance.of(decodeUtf8(p, end));
    return x;
}

int columnAt(const std::string& line, float x, const Advance& advance) {
    const char* p = line.data();
    const char* end = p + line.size();
    float left = 0.0f;
    int col = 0;
    while (p < end) {
        const float width = advance.of(decodeUtf8(p, end));
        if (x < left + width * 0.5f) return col;
        left += width;
        ++col;
    }
    return col;
}

Slice visibleSlice(const std::string& line, float fromX, float toX, const Advance& advance) {
    Slice s;
    const char* base = line.data();
    const char* end = base + line.size();
    const char* p = base;
    float x = 0.0f;
    int col = 0;
    bool started = false;
    while (p < end) {
        const char* next = p;
        const float width = advance.of(decodeUtf8(next, end));
        if (!started && x + width > fromX) {
            started = true;
            s.firstCol = col;
            s.firstByte = static_cast<std::size_t>(p - base);
            s.firstX = x;
        }
        if (started && x >= toX) {
            s.endCol = col;
            s.endByte = static_cast<std::size_t>(p - base);
            return s;
        }
        x += width;
        p = next;
        ++col;
    }
    if (!started) {
        s.firstCol = col;
        s.firstByte = line.size();
        s.firstX = x;
    }
    s.endCol = col;
    s.endByte = line.size();
    return s;
}

namespace {

int charClass(char32_t c) {
    if (c == ' ' || c == '\t' || c == 0xA0) return 0;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80) return 1;
    return 2;
}

}  // namespace

std::pair<int, int> wordBounds(const std::string& line, int col) {
    std::vector<int> classes;
    const char* p = line.data();
    const char* end = p + line.size();
    while (p < end) classes.push_back(charClass(decodeUtf8(p, end)));
    const int n = static_cast<int>(classes.size());
    if (n == 0) return {0, 0};
    const int at = std::max(0, std::min(col, n - 1));
    int from = at, to = at + 1;
    while (from > 0 && classes[static_cast<std::size_t>(from - 1)] == classes[static_cast<std::size_t>(at)]) --from;
    while (to < n && classes[static_cast<std::size_t>(to)] == classes[static_cast<std::size_t>(at)]) ++to;
    return {from, to};
}

wb::Pos hitTest(const wb::TextBuffer& buffer, float x, float y, float lineHeight, const Advance& advance) {
    int row = lineHeight > 0.0f ? static_cast<int>(std::floor(y / lineHeight)) : 0;
    row = std::max(0, std::min(row, buffer.lineCount() - 1));
    return wb::Pos{row, columnAt(buffer.line(row), x, advance)};
}

std::uint32_t colorForKind(const Theme& theme, highlight::Kind kind) {
    using K = highlight::Kind;
    switch (kind) {
        case K::Keyword: return theme.keyword;
        case K::String: return theme.string;
        case K::Number:
        case K::Boolean:
        case K::ConstantBuiltin: return theme.number;
        case K::Comment: return theme.comment;
        case K::Function: return theme.function;
        case K::Type: return theme.type;
        case K::Operator:
        case K::Bracket: break;
    }
    return theme.text;
}

void drawHighlighted(ImDrawList* list, ImFont* font, float fontSize, ImVec2 pos, const std::string& line, std::size_t from,
                     std::size_t to, const Theme& theme, const Advance& advance) {
    to = std::min(to, line.size());
    if (from >= to) return;
    float x = pos.x;
    auto run = [&](std::size_t a, std::size_t b, std::uint32_t color) {
        if (a >= b) return;
        const char* begin = line.data() + a;
        const char* end = line.data() + b;
        list->AddText(font, fontSize, ImVec2(x, pos.y), toU32(color), begin, end);
        x += textWidth(begin, end, advance);
    };
    std::size_t at = from;
    for (const highlight::Span& span : highlight::spans(line, from, to)) {
        const std::size_t s = std::max(span.start, from), e = std::min(span.end, to);
        if (s >= e || s < at) continue;
        run(at, s, theme.text);
        run(s, e, colorForKind(theme, span.kind));
        at = e;
    }
    run(at, to, theme.text);
}

// ---------------------------------------------------------------- undo

EditorSnapshot takeSnapshot(const wb::TextBuffer& buffer) {
    EditorSnapshot s;
    s.text = buffer.text();
    s.cursor = buffer.cursor();
    if (buffer.hasSelection()) {
        const auto sel = buffer.selection();
        s.anchor = (s.cursor == sel.first) ? sel.second : sel.first;
    }
    return s;
}

void restoreSnapshot(wb::TextBuffer& buffer, const EditorSnapshot& snapshot) {
    buffer.setText(snapshot.text);
    if (snapshot.anchor) buffer.selectRange(*snapshot.anchor, snapshot.cursor);
    else buffer.moveTo(snapshot.cursor, false);
}

bool UndoStack::record(bool typing, double now, const std::function<EditorSnapshot()>& before) {
    const bool merge = typing && merging_ && now - lastTime_ <= kTypingPause;
    lastTime_ = now;
    if (merge) return false;
    undo_.push_back(before());
    redo_.clear();
    merging_ = typing;
    trim();
    return true;
}

void UndoStack::cancelLast() {
    if (!undo_.empty()) undo_.pop_back();
    merging_ = false;
}

std::optional<EditorSnapshot> UndoStack::undo(EditorSnapshot current) {
    if (undo_.empty()) return std::nullopt;
    EditorSnapshot step = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(std::move(current));
    merging_ = false;
    return step;
}

std::optional<EditorSnapshot> UndoStack::redo(EditorSnapshot current) {
    if (redo_.empty()) return std::nullopt;
    EditorSnapshot step = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(std::move(current));
    merging_ = false;
    return step;
}

void UndoStack::clear() {
    undo_.clear();
    redo_.clear();
    merging_ = false;
}

void UndoStack::trim() {
    if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(undo_.size() - kMaxSteps));
    std::size_t bytes = 0;
    for (const EditorSnapshot& s : undo_) bytes += s.text.size();
    std::size_t drop = 0;
    while (bytes > kMaxBytes && drop + 1 < undo_.size()) bytes -= undo_[drop++].text.size();
    if (drop > 0) undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(drop));
}

// ---------------------------------------------------------------- the widget

void EditorWidget::edit(bool typing, const std::function<void()>& change) {
    EditorSnapshot before;
    bool have = false;
    const bool started = undo_.record(typing, now_, [&] {
        before = takeSnapshot(buffer_);
        have = true;
        return before;
    });
    change();
    if (started && have && buffer_.text() == before.text) undo_.cancelLast();   // nothing happened
    ++textStamp_;
    caretMoved_ = true;
    settleFrames_ = 0;
    lastInput_ = now_;
}

void EditorWidget::external(const std::function<void()>& change) {
    now_ = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTime() : 0.0;
    edit(false, change);
}

void EditorWidget::performUndo(bool redo) {
    EditorSnapshot current = takeSnapshot(buffer_);
    std::optional<EditorSnapshot> step = redo ? undo_.redo(std::move(current)) : undo_.undo(std::move(current));
    if (!step) return;
    restoreSnapshot(buffer_, *step);
    ++textStamp_;
    caretMoved_ = true;
    settleFrames_ = 0;
    lastInput_ = now_;
}

void EditorWidget::handleKeys() {
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
    auto moved = [&] {
        undo_.breakMerge();
        caretMoved_ = true;
        settleFrames_ = 0;
        lastInput_ = now_;
    };

    if (ctrl && !alt) {
        if (pressed(ImGuiKey_A)) {
            buffer_.selectAll();
            moved();
        }
        if (pressed(ImGuiKey_C) && !shift && buffer_.hasSelection()) ImGui::SetClipboardText(buffer_.selectedText().c_str());
        if (pressed(ImGuiKey_X) && !shift && buffer_.hasSelection()) {
            ImGui::SetClipboardText(buffer_.selectedText().c_str());
            edit(false, [&] { buffer_.backspace(); });
        }
        if (pressed(ImGuiKey_V) && !shift) {
            const char* clip = ImGui::GetClipboardText();
            if (clip != nullptr && *clip != '\0') {
                const std::string text = clip;
                edit(false, [&] { buffer_.insert(text); });
            }
        }
        if (pressed(ImGuiKey_Z)) {
            performUndo(shift);
        } else if (pressed(ImGuiKey_Y)) {
            performUndo(true);
        }
    }

    if (!ctrl && !alt) {
        if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) edit(false, [&] { buffer_.newline(); });
        if (pressed(ImGuiKey_Tab) && !shift) edit(true, [&] { buffer_.insert("\t"); });
    }
    if (!ctrl && !alt) {
        if (pressed(ImGuiKey_Backspace)) edit(true, [&] { buffer_.backspace(); });
        if (pressed(ImGuiKey_Delete) && !shift) edit(true, [&] { buffer_.del(); });
    }

    const bool word = ctrl || alt;
    if (pressed(ImGuiKey_LeftArrow)) {
        buffer_.move(word ? wb::Move::WordLeft : wb::Move::Left, shift);
        moved();
    }
    if (pressed(ImGuiKey_RightArrow)) {
        buffer_.move(word ? wb::Move::WordRight : wb::Move::Right, shift);
        moved();
    }
    if (!ctrl && !alt) {   // Ctrl+Up / Ctrl+Down / Alt+Up / Alt+Down are the history keys of the window
        if (pressed(ImGuiKey_UpArrow)) {
            buffer_.move(wb::Move::Up, shift);
            moved();
        }
        if (pressed(ImGuiKey_DownArrow)) {
            buffer_.move(wb::Move::Down, shift);
            moved();
        }
    }
    if (pressed(ImGuiKey_Home)) {
        buffer_.move(ctrl ? wb::Move::DocStart : wb::Move::Home, shift);
        moved();
    }
    if (pressed(ImGuiKey_End)) {
        buffer_.move(ctrl ? wb::Move::DocEnd : wb::Move::End, shift);
        moved();
    }
    if (pressed(ImGuiKey_PageUp) || pressed(ImGuiKey_PageDown)) {
        const int delta = pressed(ImGuiKey_PageUp) ? -pageRows_ : pageRows_;
        const wb::Pos at = buffer_.cursor();
        buffer_.moveTo(wb::Pos{std::max(0, std::min(at.row + delta, buffer_.lineCount() - 1)), at.col}, shift);
        moved();
    }

    // text: characters the platform delivered, including those outside the BMP
    if (!(ctrl && !alt)) {
        std::string typed;
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            const char32_t c = static_cast<char32_t>(io.InputQueueCharacters[i]);
            if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) continue;
            typed += encodeUtf8(c);
        }
        if (!typed.empty()) edit(true, [&] { buffer_.insert(typed); });
    }
}

void EditorWidget::draw(const char* id, ImVec2 size, const Theme& th, ImFont* mono, float monoSize, float scale) {
    now_ = ImGui::GetTime();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec4(th.surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * scale);
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PushFont(mono, monoSize);

    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float lh = ImGui::GetTextLineHeight();
    const float cw = ImGui::CalcTextSize("0").x;
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    ImFontBaked* baked = ImGui::GetFontBaked();
    Advance adv;
    adv.ascii = cw;
    adv.other = [baked](char32_t cp) { return baked->GetCharAdvance(static_cast<ImWchar>(cp)); };

    const int lines = buffer_.lineCount();
    int digits = 2;
    for (int n = lines; n >= 100; n /= 10) ++digits;
    const float inset = 8.0f * scale;
    const float gutter = static_cast<float>(digits) * cw + 24.0f * scale;
    pageRows_ = std::max(1, static_cast<int>((win->InnerRect.Max.y - win->InnerRect.Min.y) / lh) - 1);

    // the widest line decides the scroll range; it is looked up again only when the text changed
    {
        const wb::Pos c = buffer_.cursor();
        const std::size_t rowSize = buffer_.line(c.row).size();
        if (widthStamp_ != textStamp_ || widthLineCount_ != lines || widthRow_ != c.row || widthRowSize_ != rowSize) {
            maxColumns_ = 0;
            for (int r = 0; r < lines; ++r) {
                int count = 0;
                for (unsigned char ch : buffer_.line(r)) count += (ch & 0xC0) != 0x80 ? 1 : 0;
                maxColumns_ = std::max(maxColumns_, count);
            }
            widthStamp_ = textStamp_;
            widthLineCount_ = lines;
            widthRow_ = c.row;
            widthRowSize_ = rowSize;
        }
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float contentW = std::max(avail.x, gutter + static_cast<float>(maxColumns_ + 3) * cw + 12.0f * scale);
    const float contentH = std::max(avail.y, static_cast<float>(lines) * lh + 2.0f * inset);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##surface", ImVec2(contentW, contentH), ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

    const ImVec2 viewMin = win->InnerRect.Min, viewMax = win->InnerRect.Max;
    const ImVec2 textOrigin(origin.x + gutter, origin.y + inset);
    layout_.origin = textOrigin;
    layout_.viewMin = viewMin;
    layout_.viewMax = viewMax;
    layout_.lineHeight = lh;
    layout_.charWidth = cw;
    layout_.gutter = gutter;
    layout_.inset = inset;

    // ---- mouse ----
    auto hit = [&](ImVec2 mouse) { return hitTest(buffer_, mouse.x - textOrigin.x, mouse.y - textOrigin.y, lh, adv); };
    if (clicked) {
        focused_ = true;
        dragging_ = true;
        undo_.breakMerge();
        const wb::Pos p = hit(io.MousePos);
        const int count = io.MouseClickedCount[0];
        if (count >= 3) {
            const int last = buffer_.lineCount() - 1;
            if (p.row < last) buffer_.selectRange(wb::Pos{p.row, 0}, wb::Pos{p.row + 1, 0});
            else buffer_.selectRange(wb::Pos{p.row, 0}, wb::Pos{p.row, buffer_.lineLength(p.row)});
        } else if (count == 2) {
            const auto word = wordBounds(buffer_.line(p.row), p.col);
            buffer_.selectRange(wb::Pos{p.row, word.first}, wb::Pos{p.row, word.second});
        } else {
            buffer_.moveTo(p, io.KeyShift);
        }
        caretMoved_ = true;
        settleFrames_ = 0;
        lastInput_ = now_;
    } else if ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) && !hovered &&
               !ImGui::IsWindowHovered()) {
        focused_ = false;
    }
    if (dragging_) {
        if (!io.MouseDown[0]) {
            dragging_ = false;
        } else if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f * scale)) {
            const wb::Pos p = hit(io.MousePos);
            if (p != buffer_.cursor()) {
                buffer_.moveTo(p, true);
                caretMoved_ = true;
                settleFrames_ = 0;
            }
        }
    }

    // ---- keyboard ----
    if (focused_) {
        handleKeys();
        ImGuiContext& g = *ImGui::GetCurrentContext();
        g.PlatformImeData.WantVisible = true;
        g.PlatformImeData.WantTextInput = true;
    }

    // ---- keep the caret in view ----
    if (caretMoved_) {
        const wb::Pos c = buffer_.cursor();
        const float cx = columnX(buffer_.line(c.row), c.col, adv);
        const float cy = inset + static_cast<float>(c.row) * lh;
        const float sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
        const float viewW = viewMax.x - viewMin.x - gutter, viewH = viewMax.y - viewMin.y;
        const float margin = 2.0f * cw;
        float tx = sx, ty = sy;
        if (cx < sx + margin) tx = std::max(0.0f, cx - margin);
        else if (cx + cw > sx + viewW - margin) tx = cx + cw - viewW + margin;
        if (cy - inset < sy) ty = std::max(0.0f, cy - inset);
        else if (cy + lh + inset > sy + viewH) ty = cy + lh + inset - viewH;
        const bool change = tx != sx || ty != sy;
        if (tx != sx) ImGui::SetScrollX(tx);
        if (ty != sy) ImGui::SetScrollY(ty);
        if (!change || ++settleFrames_ > 3) caretMoved_ = false;
    }
    scrollX_ = ImGui::GetScrollX();
    scrollY_ = ImGui::GetScrollY();

    // ---- drawing ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int first = std::max(0, static_cast<int>(std::floor((viewMin.y - textOrigin.y) / lh)));
    const int last = std::min(buffer_.lineCount() - 1, static_cast<int>(std::floor((viewMax.y - textOrigin.y) / lh)));
    layout_.firstRow = first;
    layout_.lastRow = last;
    const wb::Pos cursor = buffer_.cursor();
    const bool hasSelection = buffer_.hasSelection();
    std::pair<wb::Pos, wb::Pos> selection;
    if (hasSelection) selection = buffer_.selection();
    const float textLeftEdge = viewMin.x + gutter;

    if (cursor.row >= first && cursor.row <= last) {   // the current line
        const float y = textOrigin.y + static_cast<float>(cursor.row) * lh;
        dl->AddRectFilled(ImVec2(textLeftEdge, y), ImVec2(viewMax.x, y + lh), toU32(th.hover, 0.55f));
    }
    for (int r = first; r <= last; ++r) {
        const std::string& line = buffer_.line(r);
        const float y = textOrigin.y + static_cast<float>(r) * lh;
        if (hasSelection && r >= selection.first.row && r <= selection.second.row) {
            const int from = r == selection.first.row ? selection.first.col : 0;
            const int to = r == selection.second.row ? selection.second.col : -1;   // -1: through the end and the newline
            const float x0 = textOrigin.x + columnX(line, from, adv);
            float x1 = to >= 0 ? textOrigin.x + columnX(line, to, adv) : textOrigin.x + textWidth(line.data(), line.data() + line.size(), adv) + cw * 0.6f;
            if (x1 > x0) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + lh), toU32(th.focus, focused_ ? kSelectionAlpha : kSelectionAlpha * 0.5f));
        }
        const Slice slice = visibleSlice(line, scrollX_ - cw, scrollX_ + (viewMax.x - viewMin.x - gutter) + cw, adv);
        drawHighlighted(dl, font, fontSize, ImVec2(textOrigin.x + slice.firstX, y), line, slice.firstByte, slice.endByte, th, adv);
    }
    if (buffer_.empty()) {
        dl->AddText(font, fontSize, ImVec2(textOrigin.x, textOrigin.y), toU32(th.faint),
                    "Yahan query likho, jaise:  DIKHAO * SE students;   (Ctrl+Enter se chalao)");
    }
    if (focused_ && cursor.row >= first - 1 && cursor.row <= last + 1) {   // the caret
        const double phase = std::fmod(std::max(0.0, now_ - lastInput_), 1.0);
        const float x = textOrigin.x + columnX(buffer_.line(cursor.row), cursor.col, adv);
        const float y = textOrigin.y + static_cast<float>(cursor.row) * lh;
        if (phase < 0.5) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + std::max(1.0f, 1.5f * scale), y + lh), toU32(th.text));
        ImGuiContext& g = *ImGui::GetCurrentContext();
        if (focused_) {
            g.PlatformImeData.InputPos = ImVec2(x, y);
            g.PlatformImeData.InputLineHeight = lh;
            g.PlatformImeData.ViewportId = ImGui::GetWindowViewport()->ID;
        }
    }
    // the line numbers stay where they are when the text scrolls sideways
    dl->AddRectFilled(ImVec2(viewMin.x, viewMin.y), ImVec2(viewMin.x + gutter, viewMax.y), toU32(th.surface));
    dl->AddLine(ImVec2(viewMin.x + gutter - 1.0f, viewMin.y), ImVec2(viewMin.x + gutter - 1.0f, viewMax.y), toU32(th.border));
    for (int r = first; r <= last; ++r) {
        const std::string number = std::to_string(r + 1);
        const float y = textOrigin.y + static_cast<float>(r) * lh;
        const float x = viewMin.x + gutter - 12.0f * scale - static_cast<float>(number.size()) * cw;
        dl->AddText(font, fontSize, ImVec2(x, y), toU32(r == cursor.row ? th.muted : th.faint), number.c_str());
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

}  // namespace meradb::gui
