// cpp/include/meradb/gui_fonts.h -- the typefaces of the desktop window: Geist (Regular, Medium) and Geist Mono
// (Regular), SIL Open Font License 1.1, compiled into the program (see THIRD_PARTY_NOTICES.md).
#pragma once
#include "imgui.h"
#include <cstddef>

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

// Adds the three fonts to `io`'s atlas (the data stays in the program: the atlas does not own it) and makes Geist
// Regular at kUiFontSize the default. Call once per ImGui context.
FontSet loadFonts(ImGuiIO& io);

}  // namespace meradb::gui
