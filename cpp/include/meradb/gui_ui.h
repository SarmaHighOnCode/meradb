// cpp/include/meradb/gui_ui.h -- the window's layout: a view over wb::Session (the same controller the terminal
// workbench uses). It only calls Dear ImGui, so the tests can run real frames of it without a window.
//
// One frame:   ui.prepare(scale);  ImGui::NewFrame();  ui.draw();  ImGui::Render();
#pragma once
#include "meradb/gui_actions.h"
#include "meradb/gui_dialogs.h"
#include "meradb/gui_editor.h"
#include "meradb/gui_fonts.h"
#include "meradb/gui_grid.h"
#include "meradb/gui_platform.h"
#include "meradb/gui_theme.h"
#include "meradb/gui_tree.h"
#include "meradb/wb_session.h"
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace meradb::gui {

// Shortens `text` in the middle ("C:\Users\...\data") until measure(result) <= maxWidth. "..." is ASCII; code points
// are never cut in half. Returns `text` unchanged when it fits and "..." when even that does not fit.
std::string ellipsizeMiddle(const std::string& text, float maxWidth, const std::function<float(const std::string&)>& measure);

// "local (C:\Users\x\data)" -> "local (data)": the connection as the top bar shows it. A server ("host:port") stays as it is.
std::string connectionLabel(const std::string& description);

// What a key does. The table is keyBindings(); the Madad window will list it.
struct KeyBinding {
    ImGuiKeyChord chord;   // ImGuiMod_Ctrl is the Cmd key on macOS (Dear ImGui swaps them there)
    Action action;
};
const std::vector<KeyBinding>& keyBindings();

class GuiUi {
public:
    GuiUi(wb::Session& session, const FontSet& fonts, ThemeMode mode);

    ThemeMode mode() const { return mode_; }
    void setMode(ThemeMode mode) { mode_ = mode; }     // takes effect at the next prepare()
    const Theme& theme() const { return themeFor(mode_); }
    float scale() const { return scale_; }

    enum class Tab { Results, Log };
    Tab tab() const { return activeTab_; }
    void showTab(Tab tab) { requestTab_ = tab; }
    double lastRunMs() const { return lastMs_; }              // the time of the run that made the table; < 0 = unknown
    float schemaWidth() const { return schemaWidth_; }        // unscaled pixels
    float queryFraction() const { return queryFraction_; }    // the editor's share of the height between the bars
    EditorWidget& editor() { return editor_; }
    const EditorWidget::Layout& editorLayout() const { return editor_.layout(); }
    SchemaTree& schemaTree() { return tree_; }
    ResultsGrid& grid() { return grid_; }
    void perform(Action action);
    ConnectDialog& connectDialog() { return connectDialog_; }
    HelpWindow& helpWindow() { return helpWindow_; }
    CommandMenu& commandMenu() { return palette_; }
    void runCommand(const CommandItem& item);
    void openPalette(const std::string& query = "");

    // The CSV toast: after an export a small card in the bottom-right corner with the log message and a Folder kholo
    // button; it hides itself after kToastSeconds (hovering it keeps it) and the message stays in the Log.
    static constexpr double kToastSeconds = 6.0;
    bool quitOverlayShown() const { return quitOverlayShown_; }   // "band ho raha hai..." was drawn in the last frame
    bool toastVisible() const { return toastVisible_; }
    const std::string& toastMessage() const { return toastMessage_; }
    void setFolderOpener(std::function<void(const std::string&)> opener) { openFolder_ = std::move(opener); }
    bool wantsFastTick() const { return toastVisible_; }   // the main loop wakes often enough to hide the toast on time
    // a toast button: where the last frame drew it (screen pixels); empty rect when no toast is shown
    ImVec2 toastButtonCentre() const { return toastButton_; }
    // A run that made a table and also failed (DIKHAO ok; DIKHAO nahi;) shows the table: Results stays in front, and
    // the Log tab carries a small error-coloured dot until it is opened (or the log is cleared). The error text is in
    // the Log; a run that makes no table and fails still goes to the Log tab itself.
    bool logErrorDot() const { return logErrorDot_; }
    // Where the last frame drew a toolbar button (screen pixels; (-1, -1) before the first frame): Run, Explain,
    // Previous or Next. Clicking one gives the keyboard back to the editor.
    ImVec2 toolbarButtonCentre(Action action) const;
    bool transactionBadgeShown() const { return badgeShown_; }   // the TRANSACTION badge was drawn in the last frame

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
    void drawTabBarExtras(float tabY, float rightX);
    void rebuildLogCache();
    bool overlayOpen() const;
    void handleShortcuts();
    void followRuns();
    void drawSplitters(float topH, float bodyH, float schemaW, float queryH, const ImVec2& display);
    float px(float value) const { return value * scale_; }

    wb::Session& session_;
    FontSet fonts_;
    ThemeMode mode_;
    ThemeMode appliedMode_;
    float scale_ = 1.0f;
    float appliedScale_ = 0.0f;   // 0: nothing applied yet
    EditorWidget editor_;
    SchemaTree tree_;
    ResultsGrid grid_;
    ConnectDialog connectDialog_;
    HelpWindow helpWindow_;
    CommandMenu palette_;
    bool overlayWasOpen_ = false;
    float schemaWidth_ = kSchemaWidth;
    float queryFraction_ = 0.42f;
    Tab activeTab_ = Tab::Results;
    std::optional<Tab> requestTab_;
    double lastMs_ = -1.0;
    int seenVersion_ = 0;
    std::size_t seenRevision_ = 0, seenGeneration_ = 0;
    bool logFollow_ = true;
    bool logErrorDot_ = false;
    ImVec2 toolbar_[4] = {ImVec2(-1, -1), ImVec2(-1, -1), ImVec2(-1, -1), ImVec2(-1, -1)};
    void focusEditor();
    bool badgeShown_ = false;
    void drawToast(const ImVec2& display, float statusHeight);
    void drawQuitOverlay(const ImVec2& display);
    bool quitOverlayShown_ = false;
    bool toastVisible_ = false;
    std::string toastMessage_, toastPath_;
    double toastShownAt_ = 0.0;
    int seenExport_ = 0;
    ImVec2 toastButton_ = ImVec2(-1, -1);
    std::function<void(const std::string&)> openFolder_ = [](const std::string& folder) { (void)openFolder(folder); };
    int logMaxChars_ = 0;

    struct LogLine {
        wb::LogKind kind;
        std::string text;
        std::size_t prefix = 0;
        mutable LineMetrics metrics;   // built when the line is very long, so drawing it costs what is visible
    };
    std::vector<LogLine> logLines_;
    std::size_t logRevision_ = static_cast<std::size_t>(-1);
    std::size_t logGeneration_ = static_cast<std::size_t>(-1);
};

}  // namespace meradb::gui
