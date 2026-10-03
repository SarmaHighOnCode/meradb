// cpp/include/meradb/wb_panels.h -- the four panels of the workbench window (FTXUI drawing and key handling).
#pragma once
#include "meradb/wb_session.h"
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <functional>
#include <memory>

namespace meradb::wb {

ftxui::Element lineToElement(const Line& line);   // one styled line: an hbox of coloured text pieces

// A bordered panel with a title; focused panels get the yellow heavy border (tui.py: `border: heavy $warning`).
// Only the border and title take the border colour: the content keeps its own colours.
ftxui::Element panelFrame(const std::string& title, ftxui::Element content, bool focused, int accentRgb);

// The same frame, but the content is built once the frame knows its final size: `body(innerWidth, innerHeight)` is
// called during layout (with both at least 1) and its element is placed inside. `box`, when given, receives the
// frame's box (borders included) so the owner can hit-test the mouse. `heavy` forces the heavy border (dialogs).
ftxui::Element sizedFrame(const std::string& title, int borderRgb, bool heavy, ftxui::Box* box,
                          std::function<ftxui::Element(int, int)> body);

// A flexible element without a border whose content is built from the final size (width, height >= 1).
ftxui::Element sizedElement(std::function<ftxui::Element(int, int)> body);

class PanelView : public ftxui::ComponentBase {
public:
    PanelView();
    void setFocused(bool focused) { focused_ = focused; }
    bool focused() const { return focused_; }
    const ftxui::Box& box() const { return box_; }   // where the panel was drawn last frame (mouse hit-tests)
    bool Focusable() const override { return true; }
    // Mouse support: a wheel step (-1 up, +1 down) or a left click at a screen position inside box().
    virtual void scrollWheel(int /*direction*/) {}
    virtual void click(int /*x*/, int /*y*/) {}
protected:
    bool focused_ = false;
    ftxui::Box box_;
    int innerHeight() const;                         // rows inside the frame, from the previous frame's box_
    int innerWidth() const;
};

std::shared_ptr<PanelView> makeTreePanel(Session& session);      // "Schema"
std::shared_ptr<PanelView> makeResultsPanel(Session& session);   // title from resultsTitle()
std::shared_ptr<PanelView> makeLogPanel(Session& session);       // "Log"
std::shared_ptr<PanelView> makeEditorPanel(Session& session);    // "Query  [F5 = chalao, F6 = samjhao]"

}  // namespace meradb::wb
