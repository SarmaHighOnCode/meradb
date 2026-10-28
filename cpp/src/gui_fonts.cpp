// cpp/src/gui_fonts.cpp -- see gui_fonts.h.
#include "meradb/gui_fonts.h"
#include "font_geist_regular.inc"
#include "font_geist_medium.inc"
#include "font_geist_mono_regular.inc"

namespace meradb::gui {

FontBlob geistRegularData() { return {kFontGeistRegular, static_cast<std::size_t>(kFontGeistRegular_size)}; }
FontBlob geistMediumData() { return {kFontGeistMedium, static_cast<std::size_t>(kFontGeistMedium_size)}; }
FontBlob geistMonoRegularData() { return {kFontGeistMonoRegular, static_cast<std::size_t>(kFontGeistMonoRegular_size)}; }

namespace {

ImFont* addFont(ImGuiIO& io, const FontBlob& blob, const char* name) {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;   // the bytes live in the program image
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = false;
    for (int i = 0; name[i] != '\0' && i < static_cast<int>(sizeof config.Name) - 1; ++i) config.Name[i] = name[i];
    return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(blob.data), static_cast<int>(blob.size), 0.0f, &config);
}

}  // namespace

FontSet loadFonts(ImGuiIO& io) {
    FontSet fonts;
    fonts.ui = addFont(io, geistRegularData(), "Geist");
    fonts.uiMedium = addFont(io, geistMediumData(), "Geist Medium");
    fonts.mono = addFont(io, geistMonoRegularData(), "Geist Mono");
    ImGui::GetStyle().FontSizeBase = kUiFontSize;
    return fonts;
}

}  // namespace meradb::gui
