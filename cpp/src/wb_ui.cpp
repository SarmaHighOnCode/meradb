// cpp/src/wb_ui.cpp
#include "meradb/wb_ui.h"
#include "meradb/wb_dialogs.h"
#include "meradb/wb_keys.h"
#include <algorithm>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/string.hpp>

namespace meradb::wb {

using namespace ftxui;

namespace {

constexpr int kBand = 0x3b3e52;
constexpr const char* kTooSmall = "Terminal bahut chhota hai (kam se kam 60 x 24)";

Color rgb(int value) {
    return Color::RGB(static_cast<std::uint8_t>((value >> 16) & 255), static_cast<std::uint8_t>((value >> 8) & 255),
                      static_cast<std::uint8_t>(value & 255));
}

}  // namespace

struct WorkbenchUi::Impl {
    Impl(Session& s, WorkbenchUi& o);

    Session& session;
    WorkbenchUi& owner;
    std::shared_ptr<PanelView> tree, results, log, editor;
    Panel focus = Panel::Editor;
    Component root;
    std::unique_ptr<ConnectForm> form;   // exists while the connect dialog is open
    HelpDialog help;
    bool helpOpen = false;
    bool pasting = false;   // between the bracketed-paste markers: a Tab is text, not "next panel"

    PanelView& panelOf(Panel p) {
        switch (p) {
            case Panel::Tree: return *tree;
            case Panel::Results: return *results;
            case Panel::Log: return *log;
            case Panel::Editor: break;
        }
        return *editor;
    }

    Element render();
    Element layout(int w, int h);
    Element headerRow(int w);
    Element footerRow(int w);
    void syncModal();
    bool handleMouse(const Mouse& mouse);
    bool handle(const Event& e);
    bool dispatch(const Event& e);
    void cycleFocus(int step);
};

namespace {

class RootComponent : public ComponentBase {
public:
    explicit RootComponent(WorkbenchUi::Impl& impl) : impl_(impl) {}
    Element Render() override;
    bool OnEvent(Event event) override;

private:
    WorkbenchUi::Impl& impl_;
};

}  // namespace

WorkbenchUi::Impl::Impl(Session& s, WorkbenchUi& o)
    : session(s), owner(o), tree(makeTreePanel(s)), results(makeResultsPanel(s)), log(makeLogPanel(s)),
      editor(makeEditorPanel(s)) {
    root = std::make_shared<RootComponent>(*this);
}

Element RootComponent::Render() { return impl_.render(); }
bool RootComponent::OnEvent(Event event) { return impl_.handle(event); }

Element WorkbenchUi::Impl::render() {
    tree->setFocused(focus == Panel::Tree);
    results->setFocused(focus == Panel::Results);
    log->setFocused(focus == Panel::Log);
    editor->setFocused(focus == Panel::Editor);
    syncModal();
    Element base = sizedElement([this](int w, int h) { return layout(w, h); });
    if (session.modal() == Modal::Connect && form) return modalOverlay(base, renderConnectDialog(*form));
    if (session.modal() == Modal::Help) return modalOverlay(base, help.render());
    return base;
}

// The dialogs' state lives only while they are open: a fresh form (with the backend's host and port) for every
// Ctrl+O, the help text back at the top for every F1.
void WorkbenchUi::Impl::syncModal() {
    const Modal modal = session.modal();
    if (modal == Modal::Connect) {
        if (!form) {
            const ConnectDefaults defaults = session.connectDefaults();
            form.reset(new ConnectForm(defaults.host, defaults.port));
        }
    } else {
        form.reset();
    }
    if (modal == Modal::Help) {
        if (!helpOpen) help.reset();
        helpOpen = true;
    } else {
        helpOpen = false;
    }
}

bool WorkbenchUi::Impl::handleMouse(const Mouse& mouse) {
    PanelView* hit = nullptr;
    Panel hitPanel = Panel::Editor;
    const Panel order[] = {Panel::Tree, Panel::Results, Panel::Log, Panel::Editor};
    for (Panel p : order) {
        const Box& b = panelOf(p).box();
        if (b.x_max >= b.x_min && mouse.x >= b.x_min && mouse.x <= b.x_max && mouse.y >= b.y_min && mouse.y <= b.y_max) {
            hit = &panelOf(p);
            hitPanel = p;
            break;
        }
    }
    if (!hit) return false;
    if (mouse.motion != Mouse::Pressed) return true;
    if (mouse.button == Mouse::Left) {
        focus = hitPanel;
        hit->click(mouse.x, mouse.y);
    } else if (mouse.button == Mouse::WheelUp) {
        hit->scrollWheel(-1);
    } else if (mouse.button == Mouse::WheelDown) {
        hit->scrollWheel(1);
    }
    return true;
}

Element WorkbenchUi::Impl::headerRow(int w) {
    Line left;
    appendSegment(left, " " + session.title() + " \xE2\x80\x94 " + session.subtitle(), fgStyle(palette::kText, true));
    const std::string busy = session.busyLabel();
    int busyWidth = 0;
    Line right;
    if (!busy.empty()) {
        appendSegment(right, busy + " ", fgStyle(palette::kYellow, true));
        busyWidth = string_width(busy) + 1;
    }
    Elements parts;
    parts.push_back(lineToElement(clipLine(left, 0, std::max(0, w - busyWidth))));
    parts.push_back(filler());
    if (!busy.empty()) parts.push_back(lineToElement(right));
    Element row = hbox(std::move(parts));
    row = bgcolor(rgb(kBand), row);
    return row | size(HEIGHT, EQUAL, 1);
}

Element WorkbenchUi::Impl::footerRow(int w) {
    // Like Textual's footer, what does not fit is left out; the least useful labels go first so that F1 and ^Q (how to
    // get help and how to leave) always show. `drop` is the level at which a label gives way (0 = never).
    struct Item { const char* key; const char* what; int drop; };
    static const Item items[] = {{"F5", "Chalao", 0},      {"F6", "Samjhao", 0},  {"^\xE2\x86\x91", "Pichli", 0},
                                 {"^\xE2\x86\x93", "Agli", 2}, {"^S", "CSV", 4},    {"^O", "Connect", 3},
                                 {"^L", "Log saaf", 1}, {"F1", "Madad", 0},    {"^Q", "Bahar", 0}};
    auto widthOf = [&](int dropLevel) {
        int total = 1;   // the leading blank
        bool firstItem = true;
        for (const Item& item : items) {
            if (item.drop != 0 && item.drop <= dropLevel) continue;
            if (!firstItem) total += 2;
            firstItem = false;
            total += string_width(item.key) + 1 + string_width(item.what);
        }
        return total;
    };
    int dropLevel = 0;
    while (dropLevel < 4 && widthOf(dropLevel) > w) ++dropLevel;
    Line line;
    bool first = true;
    for (const Item& item : items) {
        if (item.drop != 0 && item.drop <= dropLevel) continue;
        if (!first) appendSegment(line, "  ", Style());
        first = false;
        appendSegment(line, item.key, fgStyle(palette::kCyan, true));
        appendSegment(line, std::string(" ") + item.what, fgStyle(palette::kText));
    }
    Line shown;
    appendSegment(shown, " ", Style());
    for (const Segment& seg : line) shown.push_back(seg);
    Element row = lineToElement(clipLine(shown, 0, w));
    row = bgcolor(rgb(kBand), row);
    return row | size(HEIGHT, EQUAL, 1);
}

Element WorkbenchUi::Impl::layout(int w, int h) {
    if (w < WorkbenchUi::kMinWidth || h < WorkbenchUi::kMinHeight) {
        tree->forgetBox();   // nothing is drawn, so a click must not reach an invisible panel
        results->forgetBox();
        log->forgetBox();
        editor->forgetBox();
        Element message = text(kTooSmall);
        return center(message);
    }
    // Children are built into named locals, in screen order.
    Element head = headerRow(w);
    Element treeEl = tree->Render();
    treeEl = treeEl | size(WIDTH, EQUAL, 32);
    Element resultsEl = results->Render();
    resultsEl = resultsEl | flex;
    Element logEl = log->Render();
    logEl = logEl | size(HEIGHT, EQUAL, 10);
    Element editorEl = editor->Render();
    editorEl = editorEl | size(HEIGHT, EQUAL, 9);
    Elements mainColumn;
    mainColumn.push_back(resultsEl);
    mainColumn.push_back(logEl);
    mainColumn.push_back(editorEl);
    Element mainEl = vbox(std::move(mainColumn));
    mainEl = mainEl | flex;
    Elements bodyParts;
    bodyParts.push_back(treeEl);
    bodyParts.push_back(mainEl);
    Element body = hbox(std::move(bodyParts));
    body = body | flex;
    Element foot = footerRow(w);
    Elements rows;
    rows.push_back(head);
    rows.push_back(body);
    rows.push_back(foot);
    return vbox(std::move(rows));
}

void WorkbenchUi::Impl::cycleFocus(int step) {
    const int order[] = {static_cast<int>(Panel::Tree), static_cast<int>(Panel::Results), static_cast<int>(Panel::Log),
                         static_cast<int>(Panel::Editor)};
    int index = 0;
    for (int i = 0; i < 4; ++i)
        if (order[i] == static_cast<int>(focus)) index = i;
    index = (index + step + 4) % 4;
    focus = static_cast<Panel>(order[index]);
}

bool WorkbenchUi::Impl::dispatch(const Event& e) {
    if (keys::isQuit(e)) {
        session.requestQuit();
        return true;
    }
    if (e == Event::Special(keys::kPasteStart)) { pasting = true; return true; }
    if (e == Event::Special(keys::kPasteEnd)) { pasting = false; return true; }
    if (session.quitting()) return true;
    syncModal();
    const Modal modal = session.modal();
    if (modal == Modal::Help) return help.onEvent(e, session);   // keys outside its own list do nothing
    if (modal == Modal::Connect) {
        if (e.is_mouse() || keys::isInterrupt(e) || keys::isRun(e) || keys::isExplain(e) || keys::isHistoryPrev(e) ||
            keys::isHistoryNext(e) || keys::isExport(e) || keys::isConnect(e) || keys::isClearLog(e) || keys::isHelp(e))
            return true;
        if (!form) return true;
        return handleConnectEvent(*form, e, session);
    }
    if (e.is_mouse()) {
        Event copy = e;   // Event::mouse() is not const
        return handleMouse(copy.mouse());
    }
    if (keys::isInterrupt(e)) {
        session.logLine(LogKind::Warn, "Bahar niklne ke liye Ctrl+Q dabao.");
        return true;
    }
    if (keys::isRun(e)) { session.runEditorText(); return true; }
    if (keys::isExplain(e)) { session.explainEditorText(); return true; }
    if (keys::isHistoryPrev(e)) { session.historyStep(-1); return true; }
    if (keys::isHistoryNext(e)) { session.historyStep(1); return true; }
    if (keys::isExport(e)) { session.exportCsv(); return true; }
    if (keys::isConnect(e)) { session.openConnectDialog(); return true; }
    if (keys::isClearLog(e)) { session.clearLog(); return true; }
    if (keys::isHelp(e)) { session.showHelp(); return true; }
    if (e == Event::Tab && pasting) {
        // Pasted text goes into the focused text box (Textual: a Paste event); a pasted Tab never moves the focus.
        if (focus == Panel::Editor) session.editor().insert("\t");
        return true;
    }
    if (e == Event::Tab) { cycleFocus(1); return true; }
    if (e == Event::TabReverse) { cycleFocus(-1); return true; }
    return panelOf(focus).OnEvent(e);
}

bool WorkbenchUi::Impl::handle(const Event& e) {
    const bool handled = dispatch(keys::normalize(e));
    if (const std::optional<Panel> request = session.takeFocusRequest()) focus = *request;
    return handled;
}

WorkbenchUi::WorkbenchUi(Session& session) : impl_(new Impl(session, *this)) {}
WorkbenchUi::~WorkbenchUi() = default;
Component WorkbenchUi::component() const { return impl_->root; }
Element WorkbenchUi::render() { return impl_->root->Render(); }
bool WorkbenchUi::onEvent(const Event& e) { return impl_->root->OnEvent(e); }
Panel WorkbenchUi::focus() const { return impl_->focus; }
void WorkbenchUi::setFocus(Panel panel) { impl_->focus = panel; }

}  // namespace meradb::wb
