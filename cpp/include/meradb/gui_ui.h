// cpp/include/meradb/gui_ui.h -- the window's layout: a view over wb::Session (the same controller the terminal
// workbench uses). It only calls Dear ImGui, so the tests can run real frames of it without a window.
//
// One frame:   ui.prepare(scale);  ImGui::NewFrame();  ui.draw();  ImGui::Render();
#pragma once
#include "meradb/gui_fonts.h"
#include "meradb/gui_theme.h"
#include "meradb/wb_session.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace meradb::gui {

// Shortens `text` in the middle ("C:\Users\...\data") until measure(result) <= maxWidth. "..." is ASCII; code points
// are never cut in half. Returns `text` unchanged when it fits and "..." when even that does not fit.
std::string ellipsizeMiddle(const std::string& text, float maxWidth, const std::function<float(const std::string&)>& measure);

class GuiUi {
public:
    GuiUi(wb::Session& session, const FontSet& fonts, ThemeMode mode);

    ThemeMode mode() const { return mode_; }
    void setMode(ThemeMode mode) { mode_ = mode; }     // takes effect at the next prepare()
    const Theme& theme() const { return themeFor(mode_); }
    float scale() const { return scale_; }

    // Before ImGui::NewFrame(): applies the theme and the monitor's content scale if either changed.
    void prepare(float scale);
    // Between NewFrame() and Render(): the whole window.
    void draw();

    // ---- layout numbers (unscaled pixels), for the tests ----
    static constexpr float kTopBarHeight = 48.0f;
    static constexpr float kStatusBarHeight = 24.0f;
    static constexpr float kSchemaWidth = 280.0f;

private:
    struct Rect { ImVec2 pos, size; };

    bool beginPanel(const char* id, const Rect& rect, std::uint32_t background, ImVec2 padding);
    void drawTopBar(const Rect& rect);
    void drawSchema(const Rect& rect);
    void drawQuery(const Rect& rect);
    void drawResults(const Rect& rect);
    void drawStatusBar(const Rect& rect);
    void drawLogTab();
    void drawResultsTab();
    void rebuildLogCache();
    void handleShortcuts();
    float px(float value) const { return value * scale_; }

    wb::Session& session_;
    FontSet fonts_;
    ThemeMode mode_;
    ThemeMode appliedMode_;
    float scale_ = 1.0f;
    float appliedScale_ = 0.0f;   // 0: nothing applied yet
    char schemaFilter_[128] = {0};

    struct LogLine { wb::LogKind kind; std::string text; };
    std::vector<LogLine> logLines_;
    std::size_t logRevision_ = static_cast<std::size_t>(-1);
    std::size_t logGeneration_ = static_cast<std::size_t>(-1);
};

}  // namespace meradb::gui
