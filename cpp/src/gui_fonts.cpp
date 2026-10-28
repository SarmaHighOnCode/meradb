// cpp/src/gui_fonts.cpp -- see gui_fonts.h.
#include "meradb/gui_fonts.h"
#include "font_geist_regular.inc"
#include "font_geist_medium.inc"
#include "font_geist_mono_regular.inc"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>

namespace meradb::gui {

FontBlob geistRegularData() { return {kFontGeistRegular, static_cast<std::size_t>(kFontGeistRegular_size)}; }
FontBlob geistMediumData() { return {kFontGeistMedium, static_cast<std::size_t>(kFontGeistMedium_size)}; }
FontBlob geistMonoRegularData() { return {kFontGeistMonoRegular, static_cast<std::size_t>(kFontGeistMonoRegular_size)}; }

namespace {

namespace fs = std::filesystem;

bool isFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

#if !defined(_WIN32) && !defined(__APPLE__)
// The first file below `dir` (searched to a modest depth) whose name starts with one of `prefixes`.
std::string findBelow(const fs::path& dir, const std::vector<std::string>& prefixes, const char* extension) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::string();
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    std::string best;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 4) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        const std::string ext = it->path().extension().string();
        if (ext != extension && !(std::string(extension) == ".ttf" && ext == ".otf")) continue;
        for (const std::string& prefix : prefixes) {
            if (name.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string full = it->path().string();
            if (best.empty() || full < best) best = full;   // the same pick every run
        }
    }
    return best;
}

#endif

// File contents, read once per process: the fallback fonts are big (a CJK font is about 20 MB) and every font of
// every context merges the same bytes.
const std::vector<unsigned char>* fontBytes(const std::string& path) {
    static std::map<std::string, std::vector<unsigned char>> cache;
    auto found = cache.find(path);
    if (found != cache.end()) return found->second.empty() ? nullptr : &found->second;
    std::vector<unsigned char> bytes;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0 && size < 256L * 1024L * 1024L) {
            bytes.resize(static_cast<std::size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        }
        std::fclose(f);
    }
    auto inserted = cache.emplace(path, std::move(bytes));
    return inserted.first->second.empty() ? nullptr : &inserted.first->second;
}

void mergeFallbacks(ImGuiIO& io, const char* base) {
    for (const FallbackFont& fallback : systemFallbackFonts()) {
        const std::vector<unsigned char>* bytes = fontBytes(fallback.path);
        if (bytes == nullptr) continue;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.MergeMode = true;
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.PixelSnapH = false;
        config.FontNo = static_cast<ImU32>(fallback.index);
        std::snprintf(config.Name, sizeof config.Name, "%s fallback", base);
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bytes->data()), static_cast<int>(bytes->size()), 0.0f, &config);
    }
}

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

std::vector<FallbackFont> systemFallbackFonts(const std::string& root) {
    std::vector<FallbackFont> found;
#if defined(_WIN32) || defined(__APPLE__)
    auto add = [&](const fs::path& path, int index = 0) {
        if (!path.empty() && isFile(path)) found.push_back({path.string(), index});
    };
#endif
#if defined(_WIN32)
    fs::path dir = root;
    if (root.empty()) {
        const char* windir = std::getenv("WINDIR");
        if (windir == nullptr) windir = std::getenv("SystemRoot");
        dir = fs::path(windir != nullptr ? windir : "C:/Windows") / "Fonts";
    }
    add(dir / "Nirmala.ttc");
    if (found.empty()) add(dir / "Nirmala.ttf");
    add(dir / "msyh.ttc");
    add(dir / "seguisym.ttf");
    add(dir / "seguiemj.ttf");
#elif defined(__APPLE__)
    const fs::path dir = root.empty() ? fs::path("/System/Library/Fonts") : fs::path(root);
    add(dir / "Kohinoor.ttc");
    add(dir / "Supplemental" / "DevanagariSangamMN.ttc");
    add(dir / "PingFang.ttc");
    add(dir / "Apple Symbols.ttf");
#else
    const fs::path dir = root.empty() ? fs::path("/usr/share/fonts") : fs::path(root);
    auto addFound = [&](const std::string& path, int index = 0) {
        if (!path.empty()) found.push_back({path, index});
    };
    addFound(findBelow(dir, {"NotoSansDevanagari-Regular", "NotoSansDevanagari"}, ".ttf"));
    addFound(findBelow(dir, {"NotoSansCJK-Regular", "NotoSansCJKsc-Regular", "NotoSansCJK"}, ".ttc"));
    addFound(findBelow(dir, {"NotoSansCJK-Regular", "NotoSansCJKsc-Regular"}, ".otf"));
    addFound(findBelow(dir, {"NotoSansSymbols2-Regular", "NotoSansSymbols2"}, ".ttf"));
    addFound(findBelow(dir, {"NotoEmoji-Regular", "NotoEmoji["}, ".ttf"));   // monochrome; the colour bitmap font is not usable
#endif
    return found;
}

FontSet loadFonts(ImGuiIO& io) {
    FontSet fonts;
    fonts.ui = addFont(io, geistRegularData(), "Geist");
    mergeFallbacks(io, "Geist");
    fonts.uiMedium = addFont(io, geistMediumData(), "Geist Medium");
    mergeFallbacks(io, "Geist Medium");
    fonts.mono = addFont(io, geistMonoRegularData(), "Geist Mono");
    mergeFallbacks(io, "Geist Mono");
    ImGui::GetStyle().FontSizeBase = kUiFontSize;
    return fonts;
}

}  // namespace meradb::gui
