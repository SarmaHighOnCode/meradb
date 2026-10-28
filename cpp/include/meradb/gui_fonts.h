// cpp/include/meradb/gui_fonts.h -- the typefaces of the desktop window: Geist (Regular, Medium) and Geist Mono
// (Regular), SIL Open Font License 1.1, compiled into the program (see THIRD_PARTY_NOTICES.md).
#pragma once
#include "imgui.h"
#include <cstddef>
#include <string>
#include <vector>

namespace meradb::gui {

// Sizes in unscaled pixels; Dear ImGui multiplies them by style.FontScaleDpi (the monitor's content scale), and
// rasterises the glyphs again at the new size whenever that changes.
constexpr float kUiFontSize = 14.0f;
constexpr float kMonoFontSize = 13.0f;

struct FontSet {
    ImFont* ui = nullptr;        // Geist Regular: the interface
    ImFont* uiMedium = nullptr;  // Geist Medium: headings and buttons
    ImFont* mono = nullptr;      // Geist Mono Regular: the editor, the results grid and the log
};

struct FontBlob {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
};
FontBlob geistRegularData();
FontBlob geistMediumData();
FontBlob geistMonoRegularData();

// A system font merged behind Geist so that Devanagari, CJK, symbols and emoji show instead of '?'.
struct FallbackFont {
    std::string path;
    int index = 0;   // the face within a .ttc collection
};
// The fallback fonts that exist on this machine, in merge order (the first font that has a glyph wins).
//   Windows: %WINDIR%\Fonts: Nirmala.ttc (Devanagari and the other Indic scripts), msyh.ttc (CJK), seguisym.ttf, seguiemj.ttf.
//   macOS:   /System/Library/Fonts: Kohinoor (Devanagari), PingFang (CJK), Apple Symbols.
//   Linux:   Noto Sans Devanagari, Noto Sans CJK, Noto Sans Symbols 2 and Noto Emoji (monochrome) below /usr/share/fonts.
// Files that are not there are skipped silently. `root` (when not empty) replaces the system folder, for tests.
std::vector<FallbackFont> systemFallbackFonts(const std::string& root = std::string());

// Adds the three fonts to `io`'s atlas (the data stays in the program: the atlas does not own it) and makes Geist
// Regular at kUiFontSize the default. Every one of them gets the system fallback fonts merged in. Call once per ImGui
// context.
FontSet loadFonts(ImGuiIO& io);

}  // namespace meradb::gui
