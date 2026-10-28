// cpp/tests/test_gui_theme.cpp -- theme tokens, their mapping to Dear ImGui colours, the system setting, the embedded
// fonts and the middle-ellipsis helper. No window and no context are needed.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "meradb/gui_fonts.h"
#include "meradb/gui_theme.h"
#include "meradb/gui_ui.h"

using namespace meradb::gui;

TEST_CASE("gui theme dark tokens match the design table", "[gui][theme]") {
    const Theme& t = darkTheme();
    CHECK(t.mode == ThemeMode::Dark);
    CHECK(t.background == 0x000000);
    CHECK(t.surface == 0x0A0A0A);
    CHECK(t.border == 0x262626);
    CHECK(t.hover == 0x1A1A1A);
    CHECK(t.text == 0xEDEDED);
    CHECK(t.muted == 0xA1A1A1);
    CHECK(t.faint == 0x5E5E5E);
    CHECK(t.primary == 0xEDEDED);
    CHECK(t.primaryText == 0x0A0A0A);
    CHECK(t.focus == 0x0070F3);
    CHECK(t.error == 0xFF6166);
    CHECK(t.success == 0x62C073);
    CHECK(t.warning == 0xF2A65A);
    CHECK(t.keyword == 0xFF4D8D);
    CHECK(t.string == 0x62C073);
    CHECK(t.number == 0x52A8FF);
    CHECK(t.comment == 0xA1A1A1);
    CHECK(t.function == 0xBF7AF0);
    CHECK(t.type == 0xF2A65A);
}

TEST_CASE("gui theme light tokens match the design table", "[gui][theme]") {
    const Theme& t = lightTheme();
    CHECK(t.mode == ThemeMode::Light);
    CHECK(t.background == 0xFFFFFF);
    CHECK(t.surface == 0xFAFAFA);
    CHECK(t.border == 0xEAEAEA);
    CHECK(t.hover == 0xF2F2F2);
    CHECK(t.text == 0x171717);
    CHECK(t.muted == 0x666666);
    CHECK(t.faint == 0xA8A8A8);
    CHECK(t.primary == 0x171717);
    CHECK(t.primaryText == 0xFFFFFF);
    CHECK(t.focus == 0x0070F3);
    CHECK(t.error == 0xE5484D);
    CHECK(t.success == 0x297A3A);
    CHECK(t.warning == 0xAD5700);
    CHECK(t.keyword == 0xC41562);
    CHECK(t.string == 0x297A3A);
    CHECK(t.number == 0x0068D6);
    CHECK(t.comment == 0x666666);
    CHECK(t.function == 0x7820BC);
    CHECK(t.type == 0xAD5700);
}

TEST_CASE("gui theme names and switching", "[gui][theme]") {
    CHECK(&themeFor(ThemeMode::Dark) == &darkTheme());
    CHECK(&themeFor(ThemeMode::Light) == &lightTheme());
    CHECK(otherMode(ThemeMode::Dark) == ThemeMode::Light);
    CHECK(otherMode(ThemeMode::Light) == ThemeMode::Dark);
    CHECK(std::string(themeName(ThemeMode::Dark)) == "dark");
    CHECK(std::string(themeName(ThemeMode::Light)) == "light");
    CHECK(parseThemeName("dark") == ThemeMode::Dark);
    CHECK(parseThemeName("LIGHT") == ThemeMode::Light);
    CHECK_FALSE(parseThemeName("").has_value());
    CHECK_FALSE(parseThemeName("purple").has_value());
}

TEST_CASE("gui theme colours convert to ImGui colours", "[gui][theme]") {
    const ImVec4 c = toVec4(0xFF8000, 0.5f);
    CHECK(c.x == 1.0f);
    CHECK_THAT(c.y, Catch::Matchers::WithinAbs(128.0 / 255.0, 1e-6));
    CHECK(c.z == 0.0f);
    CHECK(c.w == 0.5f);
    CHECK(toU32(0x0070F3) == IM_COL32(0x00, 0x70, 0xF3, 255));
}

namespace {
void checkColour(const ImVec4& actual, std::uint32_t rgb, float alpha = 1.0f) {
    const ImVec4 expected = toVec4(rgb, alpha);
    CHECK(actual.x == expected.x);
    CHECK(actual.y == expected.y);
    CHECK(actual.z == expected.z);
    CHECK(actual.w == expected.w);
}
}  // namespace

TEST_CASE("gui applyTheme maps tokens onto the ImGui style", "[gui][theme]") {
    for (const Theme* theme : {&darkTheme(), &lightTheme()}) {
        INFO(themeName(theme->mode));
        ImGuiStyle style;
        applyTheme(style, *theme, 1.0f);
        checkColour(style.Colors[ImGuiCol_Text], theme->text);
        checkColour(style.Colors[ImGuiCol_TextDisabled], theme->faint);
        checkColour(style.Colors[ImGuiCol_WindowBg], theme->background);
        checkColour(style.Colors[ImGuiCol_PopupBg], theme->surface);
        checkColour(style.Colors[ImGuiCol_FrameBg], theme->surface);
        checkColour(style.Colors[ImGuiCol_Border], theme->border);
        checkColour(style.Colors[ImGuiCol_Button], theme->surface);
        checkColour(style.Colors[ImGuiCol_ButtonHovered], theme->hover);
        checkColour(style.Colors[ImGuiCol_HeaderHovered], theme->hover);
        checkColour(style.Colors[ImGuiCol_NavCursor], theme->focus);
        checkColour(style.Colors[ImGuiCol_TextSelectedBg], theme->focus, kSelectionAlpha);
        CHECK(kSelectionAlpha == 0.35f);
    }
}

TEST_CASE("gui applyTheme scales sizes and fonts with the monitor", "[gui][theme]") {
    ImGuiStyle one;
    applyTheme(one, darkTheme(), 1.0f);
    ImGuiStyle two;
    applyTheme(two, darkTheme(), 2.0f);
    CHECK(one.FontScaleDpi == 1.0f);
    CHECK(two.FontScaleDpi == 2.0f);
    CHECK(one.FrameRounding == 6.0f);
    CHECK(two.FrameRounding == 12.0f);
    CHECK(one.PopupRounding == 8.0f);
    CHECK(two.FramePadding.x == 2.0f * one.FramePadding.x);
    CHECK(two.ItemSpacing.y == 2.0f * one.ItemSpacing.y);
    // applying again starts from the same base: sizes do not accumulate
    applyTheme(two, darkTheme(), 2.0f);
    CHECK(two.FramePadding.x == 2.0f * one.FramePadding.x);
    // the font size already chosen is kept
    two.FontSizeBase = 14.0f;
    applyTheme(two, lightTheme(), 1.5f);
    CHECK(two.FontSizeBase == 14.0f);
}

TEST_CASE("gui system theme from the platform settings", "[gui][theme]") {
    CHECK(themeFromAppsUseLightTheme(std::nullopt) == ThemeMode::Dark);
    CHECK(themeFromAppsUseLightTheme(0u) == ThemeMode::Dark);
    CHECK(themeFromAppsUseLightTheme(1u) == ThemeMode::Light);
    CHECK(themeFromAppleInterfaceStyle(true, "Dark\n") == ThemeMode::Dark);
    CHECK(themeFromAppleInterfaceStyle(true, "Light\n") == ThemeMode::Light);
    CHECK(themeFromAppleInterfaceStyle(false, "") == ThemeMode::Light);   // the key is absent in light mode
    // whatever the machine says, it is one of the two
    const ThemeMode now = detectSystemTheme();
    CHECK((now == ThemeMode::Dark || now == ThemeMode::Light));
}

TEST_CASE("gui embedded fonts are TrueType files", "[gui][fonts]") {
    for (const FontBlob& blob : {geistRegularData(), geistMediumData(), geistMonoRegularData()}) {
        REQUIRE(blob.data != nullptr);
        CHECK(blob.size > 50000);
        CHECK(blob.size < 400000);
        // sfnt version 1.0 (0x00010000) or "true"
        const bool sfnt = blob.data[0] == 0x00 && blob.data[1] == 0x01 && blob.data[2] == 0x00 && blob.data[3] == 0x00;
        const bool truetype = blob.data[0] == 't' && blob.data[1] == 'r' && blob.data[2] == 'u' && blob.data[3] == 'e';
        CHECK((sfnt || truetype));
    }
    CHECK(kUiFontSize == 14.0f);
    CHECK(kMonoFontSize == 13.0f);
}

TEST_CASE("gui ellipsizeMiddle shortens in the middle only when needed", "[gui][ui]") {
    const auto measure = [](const std::string& s) { return static_cast<float>(s.size()) * 8.0f; };
    CHECK(ellipsizeMiddle("short", 100.0f, measure) == "short");
    const std::string path = "C:\\Users\\someone\\AppData\\Local\\MeraDB\\data";
    const std::string cut = ellipsizeMiddle(path, 200.0f, measure);
    CHECK(measure(cut) <= 200.0f);
    CHECK(cut.find("...") != std::string::npos);
    CHECK(cut.rfind("C:", 0) == 0);                       // the start survives
    CHECK(cut.substr(cut.size() - 4) == "data");          // and the end
    CHECK(ellipsizeMiddle(path, 1.0f, measure) == "...");
    // never inside a UTF-8 sequence: every byte of the result is valid
    const std::string deva = "\xE0\xA4\xAE\xE0\xA5\x87\xE0\xA4\xB0\xE0\xA4\xBE-database-\xE0\xA4\xA8\xE0\xA4\xBE\xE0\xA4\xAE";
    const std::string cutDeva = ellipsizeMiddle(deva, 80.0f, measure);
    for (std::size_t i = 0; i < cutDeva.size();) {
        const unsigned char c = static_cast<unsigned char>(cutDeva[i]);
        const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        REQUIRE(i + len <= cutDeva.size());
        for (std::size_t k = 1; k < len; ++k) CHECK((static_cast<unsigned char>(cutDeva[i + k]) & 0xC0) == 0x80);
        i += len;
    }
}
