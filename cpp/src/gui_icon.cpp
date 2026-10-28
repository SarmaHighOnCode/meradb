// cpp/src/gui_icon.cpp -- see gui_icon.h.
#include "meradb/gui_icon.h"
#include "gui_icon_data.inc"

namespace meradb::gui {

namespace {

std::uint32_t le16(const unsigned char* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }
std::uint32_t le32(const unsigned char* p) { return le16(p) | (le16(p + 2) << 16); }

}  // namespace

std::vector<IconImage> parseIcoImages(const unsigned char* data, std::size_t size) {
    std::vector<IconImage> images;
    if (data == nullptr || size < 6 || le16(data) != 0 || le16(data + 2) != 1) return images;
    const std::size_t count = le16(data + 4);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t entry = 6 + 16 * i;
        if (entry + 16 > size) break;
        const int w = data[entry] == 0 ? 256 : data[entry];
        const int h = data[entry + 1] == 0 ? 256 : data[entry + 1];
        const std::size_t length = le32(data + entry + 8), offset = le32(data + entry + 12);
        if (w > 64 || h > 64 || offset > size || length > size - offset) continue;
        const unsigned char* blob = data + offset;
        if (length < 40 || le32(blob) != 40 || le16(blob + 14) != 32) continue;   // not a 32-bit bitmap (the PNG)
        if (static_cast<int>(le32(blob + 4)) != w || static_cast<int>(le32(blob + 8)) != h * 2) continue;
        const std::size_t pixelBytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
        if (40 + pixelBytes > length) continue;
        IconImage image;
        image.width = w;
        image.height = h;
        image.rgba.resize(pixelBytes);
        for (int y = 0; y < h; ++y) {
            const unsigned char* src = blob + 40 + static_cast<std::size_t>(h - 1 - y) * static_cast<std::size_t>(w) * 4;   // bottom-up
            unsigned char* dst = &image.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4];
            for (int x = 0; x < w; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
        images.push_back(std::move(image));
    }
    return images;
}

std::vector<IconImage> windowIconImages() { return parseIcoImages(kGuiIconIco, static_cast<std::size_t>(kGuiIconIco_size)); }

}  // namespace meradb::gui
