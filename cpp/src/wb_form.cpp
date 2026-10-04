// cpp/src/wb_form.cpp
#include "meradb/wb_form.h"
#include <algorithm>

namespace meradb::wb {

namespace {

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

int codePoints(const std::string& s) {
    int n = 0;
    for (char c : s)
        if (!isContinuation(c)) ++n;
    return n;
}

// Byte offset of code point index `cp` (clamped to the end).
std::size_t byteOf(const std::string& s, int cp) {
    std::size_t i = 0;
    int seen = 0;
    while (i < s.size() && seen < cp) {
        ++i;
        while (i < s.size() && isContinuation(s[i])) ++i;
        ++seen;
    }
    return i;
}

}  // namespace

int LineEdit::length() const { return codePoints(text_); }

std::string LineEdit::shown() const {
    if (!password_) return text_;
    return std::string(static_cast<std::size_t>(length()), '*');
}

void LineEdit::insert(const std::string& utf8) {
    std::string kept;
    for (char c : utf8) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) continue;
        if (digitsOnly && (u < '0' || u > '9')) continue;
        kept += c;
    }
    if (kept.empty()) return;
    if (selected_) {
        text_.clear();
        cursor_ = 0;
        selected_ = false;
    }
    text_.insert(byteOf(text_, cursor_), kept);
    cursor_ += codePoints(kept);
}

void LineEdit::backspace() {
    if (selected_) {
        text_.clear();
        cursor_ = 0;
        selected_ = false;
        return;
    }
    if (cursor_ <= 0) return;
    const std::size_t from = byteOf(text_, cursor_ - 1);
    const std::size_t to = byteOf(text_, cursor_);
    text_.erase(from, to - from);
    --cursor_;
}

void LineEdit::del() {
    if (selected_) {
        text_.clear();
        cursor_ = 0;
        selected_ = false;
        return;
    }
    if (cursor_ >= length()) return;
    const std::size_t from = byteOf(text_, cursor_);
    const std::size_t to = byteOf(text_, cursor_ + 1);
    text_.erase(from, to - from);
}

void LineEdit::left() {
    if (selected_) cursor_ = 1;   // the selection ends at the start; the step below makes it 0
    selected_ = false;
    cursor_ = std::max(0, cursor_ - 1);
}
void LineEdit::right() {
    if (selected_) cursor_ = length() - 1;   // ... and at the end
    selected_ = false;
    cursor_ = std::min(length(), cursor_ + 1);
}
void LineEdit::home() {
    selected_ = false;
    cursor_ = 0;
}
void LineEdit::end() {
    selected_ = false;
    cursor_ = length();
}

void LineEdit::setText(const std::string& text) {
    text_.clear();
    cursor_ = 0;
    selected_ = false;
    insert(text);
}

void LineEdit::selectAll() {
    selected_ = !text_.empty();
    cursor_ = length();
}

ConnectForm::ConnectForm(const std::string& host, const std::string& port) {
    fields_[0] = LineEdit(host);
    fields_[1] = LineEdit();
    fields_[1].digitsOnly = true;
    fields_[1].setText(port);
    fields_[2] = LineEdit("", true);
    fields_[3] = LineEdit();
    fields_[0].selectAll();   // the first field starts with the focus
}

// A field that gets the focus selects its whole text, as Textual's Input does (select_on_focus).
void ConnectForm::focusChanged() {
    if (active_ < kFields) fields_[static_cast<std::size_t>(active_)].selectAll();
}
void ConnectForm::next() {
    active_ = (active_ + 1) % (kFields + kButtons);
    focusChanged();
}
void ConnectForm::previous() {
    active_ = (active_ + kFields + kButtons - 1) % (kFields + kButtons);
    focusChanged();
}
void ConnectForm::setActive(int index) {
    const int target = std::max(0, std::min(kFields + kButtons - 1, index));
    const bool changed = target != active_;
    active_ = target;
    if (changed) focusChanged();
}

FormAction ConnectForm::enter() const {
    if (active_ < kFields) return FormAction::Connect;
    switch (active_ - kFields) {
        case 0: return FormAction::Connect;
        case 1: return FormAction::Local;
        default: return FormAction::Cancel;
    }
}

ConnectRequest ConnectForm::request(bool local) const {
    return makeConnectRequest(local, fields_[0].text(), fields_[1].text(), fields_[2].text(), fields_[3].text());
}

}  // namespace meradb::wb
