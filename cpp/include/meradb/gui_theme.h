// cpp/include/meradb/gui_theme.h -- the colours of the desktop window (a minimal monochrome look in the style of
// Vercel's Geist design system) and how they become Dear ImGui style colours.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "imgui.h"

namespace meradb::gui {

enum class ThemeMode { Dark, Light };

// Every colour is 0xRRGGBB. Names follow the design table in docs/cpp-port/plans/2026-10-28-desktop-workbench.md.
struct Theme {
    ThemeMode mode = ThemeMode::Dark;
    std::uint32_t background = 0, surface = 0, border = 0, hover = 0;
    std::uint32_t text = 0, muted = 0, faint = 0;
    std::uint32_t primary = 0, primaryText = 0;   // the primary button
    std::uint32_t focus = 0;                      // focus ring; the selection is this colour at kSelectionAlpha
    std::uint32_t error = 0, success = 0, warning = 0;
    // syntax colours (Geist code style); operators and brackets use `text`
    std::uint32_t keyword = 0, string = 0, number = 0, comment = 0, function = 0, type = 0;
};

constexpr float kSelectionAlpha = 0.35f;

const Theme& darkTheme();
const Theme& lightTheme();
const Theme& themeFor(ThemeMode mode);
ThemeMode otherMode(ThemeMode mode);
const char* themeName(ThemeMode mode);                              // "dark" / "light"
std::optional<ThemeMode> parseThemeName(const std::string& name);   // "dark", "light" (any case); else nullopt

ImVec4 toVec4(std::uint32_t rgb, float alpha = 1.0f);
ImU32 toU32(std::uint32_t rgb, float alpha = 1.0f);

// Fills `style` with the theme's colours and the shapes of the look (radius 6, no shadows, hairline borders, a 4px
// grid), every size multiplied by `scale` (the monitor's content scale), and sets style.FontScaleDpi to `scale`.
// Call it between frames only. Keeps the style's current font size.
void applyTheme(ImGuiStyle& style, const Theme& theme, float scale);

// ---- the system setting ----
// Windows: the AppsUseLightTheme registry value (nullopt = not readable). 0 = dark, anything else = light.
ThemeMode themeFromAppsUseLightTheme(std::optional<std::uint32_t> value);
// macOS: `defaults read -g AppleInterfaceStyle` prints "Dark" in dark mode and fails (the key is absent) in light mode.
ThemeMode themeFromAppleInterfaceStyle(bool commandSucceeded, const std::string& output);
// Windows and macOS read the system setting; everywhere else (and when it cannot be read) dark.
ThemeMode detectSystemTheme();

}  // namespace meradb::gui
