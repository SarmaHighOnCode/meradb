// cpp/src/gui_theme.cpp -- see gui_theme.h.
#include "meradb/gui_theme.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace meradb::gui {

namespace {

Theme makeDark() {
    Theme t;
    t.mode = ThemeMode::Dark;
    t.background = 0x000000; t.surface = 0x0A0A0A; t.border = 0x262626; t.hover = 0x1A1A1A;
    t.text = 0xEDEDED; t.muted = 0xA1A1A1; t.faint = 0x5E5E5E;
    t.primary = 0xEDEDED; t.primaryText = 0x0A0A0A;
    t.focus = 0x0070F3;
    t.error = 0xFF6166; t.success = 0x62C073; t.warning = 0xF2A65A;
    t.keyword = 0xFF4D8D; t.string = 0x62C073; t.number = 0x52A8FF; t.comment = 0xA1A1A1;
    t.function = 0xBF7AF0; t.type = 0xF2A65A;
    return t;
}

Theme makeLight() {
    Theme t;
    t.mode = ThemeMode::Light;
    t.background = 0xFFFFFF; t.surface = 0xFAFAFA; t.border = 0xEAEAEA; t.hover = 0xF2F2F2;
    t.text = 0x171717; t.muted = 0x666666; t.faint = 0xA8A8A8;
    t.primary = 0x171717; t.primaryText = 0xFFFFFF;
    t.focus = 0x0070F3;
    t.error = 0xE5484D; t.success = 0x297A3A; t.warning = 0xAD5700;
    t.keyword = 0xC41562; t.string = 0x297A3A; t.number = 0x0068D6; t.comment = 0x666666;
    t.function = 0x7820BC; t.type = 0xAD5700;
    return t;
}

}  // namespace

const Theme& darkTheme() {
    static const Theme theme = makeDark();
    return theme;
}

const Theme& lightTheme() {
    static const Theme theme = makeLight();
    return theme;
}

const Theme& themeFor(ThemeMode mode) { return mode == ThemeMode::Light ? lightTheme() : darkTheme(); }

ThemeMode otherMode(ThemeMode mode) { return mode == ThemeMode::Light ? ThemeMode::Dark : ThemeMode::Light; }

const char* themeName(ThemeMode mode) { return mode == ThemeMode::Light ? "light" : "dark"; }

std::optional<ThemeMode> parseThemeName(const std::string& name) {
    std::string lower;
    for (unsigned char c : name) lower += static_cast<char>(std::tolower(c));
    if (lower == "dark") return ThemeMode::Dark;
    if (lower == "light") return ThemeMode::Light;
    return std::nullopt;
}

ImVec4 toVec4(std::uint32_t rgb, float alpha) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

ImU32 toU32(std::uint32_t rgb, float alpha) { return ImGui::ColorConvertFloat4ToU32(toVec4(rgb, alpha)); }

void applyTheme(ImGuiStyle& style, const Theme& theme, float scale) {
    const float keepFontBase = style.FontSizeBase;
    const float keepNextFontBase = style._NextFrameFontSizeBase;

    ImGuiStyle fresh;   // the library defaults, then our shapes
    fresh.Alpha = 1.0f;
    fresh.WindowPadding = ImVec2(12, 12);
    fresh.WindowRounding = 0.0f;
    fresh.WindowBorderSize = 0.0f;
    fresh.ChildRounding = 6.0f;
    fresh.ChildBorderSize = 1.0f;
    fresh.PopupRounding = 8.0f;
    fresh.PopupBorderSize = 1.0f;
    fresh.FramePadding = ImVec2(12, 6);
    fresh.FrameRounding = 6.0f;
    fresh.FrameBorderSize = 1.0f;
    fresh.ItemSpacing = ImVec2(8, 8);
    fresh.ItemInnerSpacing = ImVec2(8, 4);
    fresh.CellPadding = ImVec2(8, 4);
    fresh.IndentSpacing = 16.0f;
    fresh.ScrollbarSize = 10.0f;
    fresh.ScrollbarRounding = 6.0f;
    fresh.GrabMinSize = 12.0f;
    fresh.GrabRounding = 6.0f;
    fresh.TabRounding = 6.0f;
    fresh.TabBorderSize = 0.0f;
    fresh.WindowMinSize = ImVec2(1, 1);   // the panels are laid out by hand and may be thin (the status bar is 24px)
    fresh.ScaleAllSizes(scale);   // sizes only: the font scale is set next
    fresh.FontScaleDpi = scale;

    ImVec4* c = fresh.Colors;
    const ImVec4 clear(0, 0, 0, 0);
    c[ImGuiCol_Text] = toVec4(theme.text);
    c[ImGuiCol_TextDisabled] = toVec4(theme.faint);
    c[ImGuiCol_WindowBg] = toVec4(theme.background);
    c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_PopupBg] = toVec4(theme.surface);
    c[ImGuiCol_Border] = toVec4(theme.border);
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_FrameBg] = toVec4(theme.surface);
    c[ImGuiCol_FrameBgHovered] = toVec4(theme.hover);
    c[ImGuiCol_FrameBgActive] = toVec4(theme.hover);
    c[ImGuiCol_TitleBg] = toVec4(theme.background);
    c[ImGuiCol_TitleBgActive] = toVec4(theme.background);
    c[ImGuiCol_TitleBgCollapsed] = toVec4(theme.background);
    c[ImGuiCol_MenuBarBg] = toVec4(theme.background);
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = toVec4(theme.faint, 0.6f);
    c[ImGuiCol_ScrollbarGrabHovered] = toVec4(theme.faint, 0.85f);
    c[ImGuiCol_ScrollbarGrabActive] = toVec4(theme.muted);
    c[ImGuiCol_CheckMark] = toVec4(theme.text);
    c[ImGuiCol_SliderGrab] = toVec4(theme.muted);
    c[ImGuiCol_SliderGrabActive] = toVec4(theme.text);
    c[ImGuiCol_Button] = toVec4(theme.surface);
    c[ImGuiCol_ButtonHovered] = toVec4(theme.hover);
    c[ImGuiCol_ButtonActive] = toVec4(theme.border);
    c[ImGuiCol_Header] = toVec4(theme.hover);
    c[ImGuiCol_HeaderHovered] = toVec4(theme.hover);
    c[ImGuiCol_HeaderActive] = toVec4(theme.border);
    c[ImGuiCol_Separator] = toVec4(theme.border);
    c[ImGuiCol_SeparatorHovered] = toVec4(theme.focus);
    c[ImGuiCol_SeparatorActive] = toVec4(theme.focus);
    c[ImGuiCol_ResizeGrip] = clear;
    c[ImGuiCol_ResizeGripHovered] = toVec4(theme.focus, 0.6f);
    c[ImGuiCol_ResizeGripActive] = toVec4(theme.focus);
    c[ImGuiCol_Tab] = clear;
    c[ImGuiCol_TabHovered] = toVec4(theme.hover);
    c[ImGuiCol_TabSelected] = toVec4(theme.hover);
    c[ImGuiCol_TabSelectedOverline] = toVec4(theme.text);
    c[ImGuiCol_TabDimmed] = clear;
    c[ImGuiCol_TabDimmedSelected] = toVec4(theme.hover);
    c[ImGuiCol_TabDimmedSelectedOverline] = toVec4(theme.muted);
    c[ImGuiCol_TableHeaderBg] = toVec4(theme.surface);
    c[ImGuiCol_TableBorderStrong] = toVec4(theme.border);
    c[ImGuiCol_TableBorderLight] = toVec4(theme.border);
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = clear;
    c[ImGuiCol_TextLink] = toVec4(theme.number);
    c[ImGuiCol_TextSelectedBg] = toVec4(theme.focus, kSelectionAlpha);
    c[ImGuiCol_DragDropTarget] = toVec4(theme.focus);
    c[ImGuiCol_NavCursor] = toVec4(theme.focus);
    c[ImGuiCol_NavWindowingHighlight] = toVec4(theme.text, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = toVec4(theme.background, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = toVec4(0x000000, theme.mode == ThemeMode::Dark ? 0.6f : 0.25f);
    c[ImGuiCol_PlotLines] = toVec4(theme.muted);
    c[ImGuiCol_PlotLinesHovered] = toVec4(theme.text);
    c[ImGuiCol_PlotHistogram] = toVec4(theme.muted);
    c[ImGuiCol_PlotHistogramHovered] = toVec4(theme.text);

    style = fresh;
    style.FontSizeBase = keepFontBase;
    style._NextFrameFontSizeBase = keepNextFontBase;
}

ThemeMode themeFromAppsUseLightTheme(std::optional<std::uint32_t> value) {
    return value && *value != 0 ? ThemeMode::Light : ThemeMode::Dark;
}

ThemeMode themeFromAppleInterfaceStyle(bool commandSucceeded, const std::string& output) {
    if (commandSucceeded) {
        std::string lower;
        for (unsigned char ch : output) lower += static_cast<char>(std::tolower(ch));
        return lower.find("dark") != std::string::npos ? ThemeMode::Dark : ThemeMode::Light;
    }
    return ThemeMode::Light;   // the key does not exist while the system is in light mode
}

ThemeMode detectSystemTheme() {
#if defined(_WIN32)
    DWORD value = 0;
    DWORD size = sizeof value;
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status != ERROR_SUCCESS) return themeFromAppsUseLightTheme(std::nullopt);
    return themeFromAppsUseLightTheme(static_cast<std::uint32_t>(value));
#elif defined(__APPLE__)
    FILE* pipe = popen("defaults read -g AppleInterfaceStyle 2>/dev/null", "r");
    if (pipe == nullptr) return ThemeMode::Dark;
    std::string output;
    char buffer[64];
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) output += buffer;
    const int status = pclose(pipe);
    return themeFromAppleInterfaceStyle(status == 0, output);
#else
    return ThemeMode::Dark;
#endif
}

}  // namespace meradb::gui
