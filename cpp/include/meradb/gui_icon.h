// cpp/include/meradb/gui_icon.h -- the window icon: the pixels of the images in resources/meradb_gui.ico (the same file
// the Windows executable carries as its application icon), embedded in the program.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace meradb::gui {

struct IconImage {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;   // width * height * 4, rows top to bottom
};

// Reads the uncompressed 32-bit bitmap images (up to 64 pixels) out of an .ico file image; anything else in it
// (the 256 pixel PNG) or anything malformed is skipped. Never reads outside `data`.
std::vector<IconImage> parseIcoImages(const unsigned char* data, std::size_t size);

// The embedded icon's images.
std::vector<IconImage> windowIconImages();

}  // namespace meradb::gui
