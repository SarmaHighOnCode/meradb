// cpp/src/wb_dialogs.cpp
#include "meradb/wb_dialogs.h"
#include "meradb/wb_editor.h"
#include "meradb/wb_help.h"
#include "meradb/wb_keys.h"
#include "meradb/wb_panels.h"
#include <algorithm>

namespace meradb::wb {

using namespace ftxui;

namespace {

constexpr int kConnectWidth = 64;
constexpr int kConnectHeight = 15;

const char* const kLabels[] = {"Host", "Port", "Password (agar server par hai)", "Database (optional)"};
const char* const kButtonLabels[] = {"[ Connect ]", "[ Local mode ]", "[ Cancel ]"};

// A text field of `width` cells; the active one shows the cursor as an inverse cell and scrolls to keep it in view.
Line fieldLine(const LineEdit& edit, bool active, int width) {
    const std::string shown = edit.shown();
    const std::size_t at = [&] {
        std::size_t i = 0;
        for (int cp = 0; cp < edit.cursor() && i < shown.size(); ++cp) {
            ++i;
            while (i < shown.size() && (static_cast<unsigned char>(shown[i]) & 0xC0) == 0x80) ++i;
        }
        return i;
    }();
    Line line;
    appendSegment(line, shown.substr(0, at), fgStyle(palette::kText));
    int skip = 0;
    if (active) {
        std::size_t next = at;
        if (next < shown.size()) {
            ++next;
            while (next < shown.size() && (static_cast<unsigned char>(shown[next]) & 0xC0) == 0x80) ++next;
        }
        Style cursor = fgStyle(palette::kText);
        cursor.inverse = true;
        appendSegment(line, at < shown.size() ? shown.substr(at, next - at) : std::string(" "), cursor);
        appendSegment(line, shown.substr(next), fgStyle(palette::kText));
        const int column = displayColumn(shown, edit.cursor());
        skip = std::max(0, column + 1 - width);
    } else {
        appendSegment(line, shown.substr(at), fgStyle(palette::kText));
    }
    return clipLine(line, skip, width);
}

Element connectBody(const ConnectForm& form, int w) {
    const int fieldWidth = std::max(1, w - 4);
    Elements rows;
    rows.push_back(text(""));
    {
        Line title;
        appendSegment(title, "MeraDB server se connect karo", fgStyle(palette::kText, true));
        Elements parts;
        parts.push_back(text("  "));
        parts.push_back(lineToElement(title));
        rows.push_back(hbox(std::move(parts)));
    }
    for (int i = 0; i < ConnectForm::kFields; ++i) {
        Line label;
        appendSegment(label, kLabels[i], fgStyle(palette::kText));
        Elements labelParts;
        labelParts.push_back(text("  "));
        labelParts.push_back(lineToElement(label));
        rows.push_back(hbox(std::move(labelParts)));

        const bool active = form.active() == i;
        Element field = lineToElement(fieldLine(form.field(i), active, fieldWidth));
        field = field | size(WIDTH, EQUAL, fieldWidth);
        field = color(Color::RGB(0xf8, 0xf8, 0xf2), field);   // light text on the dark tint, whatever the terminal's own colours
        field = bgcolor(Color::RGB(active ? 0x44 : 0x2f, active ? 0x47 : 0x31, active ? 0x5a : 0x42), field);
        Elements fieldParts;
        fieldParts.push_back(text("  "));
        fieldParts.push_back(field);
        rows.push_back(hbox(std::move(fieldParts)));
    }
    rows.push_back(text(""));
    {
        Line buttons;
        for (int b = 0; b < ConnectForm::kButtons; ++b) {
            if (b) appendSegment(buttons, "  ", Style());
            Style style = fgStyle(palette::kText, true);
            if (form.active() == ConnectForm::kFields + b) style.inverse = true;
            appendSegment(buttons, kButtonLabels[b], style);
        }
        Elements parts;
        parts.push_back(text("  "));
        parts.push_back(lineToElement(buttons));
        rows.push_back(hbox(std::move(parts)));
    }
    return vbox(std::move(rows));
}

}  // namespace

Element modalOverlay(Element base, Element dialog) {
    Elements layers;
    layers.push_back(std::move(base));
    layers.push_back(std::move(dialog));
    return dbox(std::move(layers));
}

Element renderConnectDialog(const ConnectForm& form) {
    const ConnectForm* f = &form;
    return sizedElement([f](int w, int h) {
        const int width = std::max(4, std::min(kConnectWidth, w - 2));
        const int height = std::max(3, std::min(kConnectHeight, h));
        Element frame = sizedFrame("", palette::kPurple, true, nullptr,
                                   [f](int innerW, int /*innerH*/) { return connectBody(*f, innerW); });
        frame = frame | size(WIDTH, EQUAL, width);
        frame = frame | size(HEIGHT, EQUAL, height);
        frame = clear_under(frame);
        return center(frame);
    });
}

bool handleConnectEvent(ConnectForm& form, const Event& e, Session& session) {
    auto apply = [&](FormAction action) {
        switch (action) {
            case FormAction::Connect: session.connect(form.request(false)); break;
            case FormAction::Local: session.connect(form.request(true)); break;
            case FormAction::Cancel: session.closeModal(); break;
            case FormAction::None: break;
        }
    };
    if (e == Event::Escape) {
        apply(form.escape());
        return true;
    }
    if (e == Event::Return) {
        apply(form.enter());
        return true;
    }
    if (e == Event::Tab || e == Event::ArrowDown) {
        form.next();
        return true;
    }
    if (e == Event::TabReverse || e == Event::ArrowUp) {
        form.previous();
        return true;
    }
    const bool onField = form.active() < ConnectForm::kFields;
    if (!onField) {
        if (e == Event::ArrowLeft && form.active() > ConnectForm::kFields) form.previous();
        else if (e == Event::ArrowRight && form.active() < ConnectForm::kFields + ConnectForm::kButtons - 1) form.next();
        else if (e == Event::Character(" ")) apply(form.enter());
        return true;
    }
    LineEdit& edit = form.field(form.active());
    if (e == Event::Backspace) edit.backspace();
    else if (e == Event::Delete) edit.del();
    else if (e == Event::ArrowLeft) edit.left();
    else if (e == Event::ArrowRight) edit.right();
    else if (e == Event::Home) edit.home();
    else if (e == Event::End) edit.end();
    else if (e.is_character()) edit.insert(e.character());
    return true;   // an open dialog takes every key
}

// ---- help ----

Element HelpDialog::render() {
    return sizedElement([this](int w, int h) {
        const int width = std::max(10, w * 9 / 10);
        const int height = std::max(5, h * 9 / 10);
        Element frame = sizedFrame("Madad", palette::kPurple, true, nullptr, [this](int innerW, int innerH) {
            const int textWidth = std::max(1, innerW - 4);
            if (textWidth != width_) {
                lines_ = renderHelp(textWidth);
                width_ = textWidth;
            }
            scroll_.setCount(static_cast<int>(lines_.size()));
            scroll_.setHeight(innerH);
            Elements rows;
            const int last = std::min(scroll_.count(), scroll_.top() + innerH);
            for (int i = scroll_.top(); i < last; ++i) {
                Elements parts;
                parts.push_back(text("  "));
                parts.push_back(lineToElement(clipLine(lines_[static_cast<std::size_t>(i)], 0, textWidth)));
                rows.push_back(hbox(std::move(parts)));
            }
            return vbox(std::move(rows));
        });
        frame = frame | size(WIDTH, EQUAL, std::min(width, w));
        frame = frame | size(HEIGHT, EQUAL, std::min(height, h));
        frame = clear_under(frame);
        return center(frame);
    });
}

bool HelpDialog::onEvent(const Event& e, Session& session) {
    if (e.is_mouse()) {
        Event copy = e;   // Event::mouse() is not const
        const Mouse& mouse = copy.mouse();
        if (mouse.motion == Mouse::Pressed && mouse.button == Mouse::WheelUp) scrollWheel(-1);
        else if (mouse.motion == Mouse::Pressed && mouse.button == Mouse::WheelDown) scrollWheel(1);
        return true;
    }
    if (e == Event::Escape || keys::isHelp(e) || e == Event::Character("q")) {
        session.closeModal();
        return true;
    }
    const int page = std::max(1, scroll_.height() - 1);
    if (e == Event::ArrowDown) scroll_.scrollBy(1);
    else if (e == Event::ArrowUp) scroll_.scrollBy(-1);
    else if (e == Event::PageDown) scroll_.scrollBy(page);
    else if (e == Event::PageUp) scroll_.scrollBy(-page);
    else if (e == Event::Home) scroll_.scrollBy(-scroll_.count());
    else if (e == Event::End) scroll_.scrollBy(scroll_.count());
    return true;
}

void HelpDialog::scrollWheel(int direction) { scroll_.scrollBy(direction * 3); }

void HelpDialog::reset() {
    scroll_.scrollBy(-scroll_.count());
}

}  // namespace meradb::wb
