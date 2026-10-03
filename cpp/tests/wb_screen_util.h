// cpp/tests/wb_screen_util.h -- render FTXUI elements into a fixed-size screen and look at the text and colours.
#pragma once
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>
#include <vector>

namespace wbtest {

// Renders an element into a width x height screen and returns the text of each row (a wide glyph's second cell is empty).
inline std::vector<std::string> renderLines(ftxui::Element e, int width, int height, ftxui::Screen* keep = nullptr) {
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, e);
    std::vector<std::string> lines;
    for (int y = 0; y < height; ++y) {
        std::string row;
        for (int x = 0; x < width; ++x) row += screen.PixelAt(x, y).character;
        while (!row.empty() && row.back() == ' ') row.pop_back();   // trailing blanks are noise
        lines.push_back(row);
    }
    if (keep) *keep = std::move(screen);
    return lines;
}
inline std::string fgOf(ftxui::Screen& s, int x, int y) { return s.PixelAt(x, y).foreground_color.Print(false); }
inline std::string bgOf(ftxui::Screen& s, int x, int y) { return s.PixelAt(x, y).background_color.Print(true); }
inline std::string rgbFg(int rgb) {
    return ftxui::Color::RGB(static_cast<std::uint8_t>((rgb >> 16) & 255), static_cast<std::uint8_t>((rgb >> 8) & 255),
                             static_cast<std::uint8_t>(rgb & 255)).Print(false);
}
inline std::string rgbBg(int rgb) {
    return ftxui::Color::RGB(static_cast<std::uint8_t>((rgb >> 16) & 255), static_cast<std::uint8_t>((rgb >> 8) & 255),
                             static_cast<std::uint8_t>(rgb & 255)).Print(true);
}
inline std::string defaultFg() { return ftxui::Color().Print(false); }
inline std::string defaultBg() { return ftxui::Color().Print(true); }
// Finds the first row containing `needle`; -1 if none. Useful to avoid hard-coding layout rows.
inline int findRow(const std::vector<std::string>& lines, const std::string& needle) {
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find(needle) != std::string::npos) return static_cast<int>(i);
    return -1;
}
// Column (in cells) of the first occurrence of `needle` in a rendered row (-1 if absent). ASCII-safe: counts glyphs, so
// only use it on rows whose text before the needle is single-width.
inline int cellColumn(const std::string& row, const std::string& needle) {
    const std::size_t at = row.find(needle);
    if (at == std::string::npos) return -1;
    int cells = 0;
    for (std::size_t i = 0; i < at; ++i)
        if ((static_cast<unsigned char>(row[i]) & 0xC0) != 0x80) ++cells;
    return cells;
}

}  // namespace wbtest
