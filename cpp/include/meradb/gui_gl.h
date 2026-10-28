// cpp/include/meradb/gui_gl.h -- the few OpenGL calls the window itself makes (clear, and drawing into an off-screen
// image for screenshots). Dear ImGui's own renderer loads what it needs by itself; these are loaded through GLFW so
// no OpenGL header or import library is needed.
#pragma once
#include <string>
#include <vector>

namespace meradb::gui {

#ifdef _WIN32
#define MERADB_GL_CALL __stdcall
#else
#define MERADB_GL_CALL
#endif

struct GlApi {
    void(MERADB_GL_CALL* Viewport)(int, int, int, int) = nullptr;
    void(MERADB_GL_CALL* ClearColor)(float, float, float, float) = nullptr;
    void(MERADB_GL_CALL* Clear)(unsigned) = nullptr;
    void(MERADB_GL_CALL* ReadPixels)(int, int, int, int, unsigned, unsigned, void*) = nullptr;
    void(MERADB_GL_CALL* PixelStorei)(unsigned, int) = nullptr;
    void(MERADB_GL_CALL* GenTextures)(int, unsigned*) = nullptr;
    void(MERADB_GL_CALL* BindTexture)(unsigned, unsigned) = nullptr;
    void(MERADB_GL_CALL* TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = nullptr;
    void(MERADB_GL_CALL* TexParameteri)(unsigned, unsigned, int) = nullptr;
    void(MERADB_GL_CALL* DeleteTextures)(int, const unsigned*) = nullptr;
    void(MERADB_GL_CALL* GenFramebuffers)(int, unsigned*) = nullptr;
    void(MERADB_GL_CALL* BindFramebuffer)(unsigned, unsigned) = nullptr;
    void(MERADB_GL_CALL* FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = nullptr;
    unsigned(MERADB_GL_CALL* CheckFramebufferStatus)(unsigned) = nullptr;
    void(MERADB_GL_CALL* DeleteFramebuffers)(int, const unsigned*) = nullptr;
    void(MERADB_GL_CALL* Finish)() = nullptr;

    // Needs a current OpenGL context (glfwMakeContextCurrent). False if a function is missing.
    bool load();
};

// An RGBA image the size of the screenshot that Dear ImGui can draw into instead of the window, so the picture does
// not depend on whether the (hidden) window's own back buffer can be read.
class OffscreenTarget {
public:
    OffscreenTarget() = default;
    ~OffscreenTarget();
    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    bool create(const GlApi& gl, int width, int height);   // false: not supported, draw into the window instead
    void bind();
    void unbind();
    // RGBA, top row first.
    bool read(std::vector<unsigned char>& rgba);
    bool valid() const { return framebuffer_ != 0; }

private:
    const GlApi* gl_ = nullptr;
    unsigned framebuffer_ = 0, texture_ = 0;
    int width_ = 0, height_ = 0;
};

// Writes an RGBA image (rows top to bottom, alpha ignored: the PNG is opaque RGB) to `path` (UTF-8). False with
// `error` set on failure.
bool writePng(const std::string& path, int width, int height, const unsigned char* rgba, std::string& error);

}  // namespace meradb::gui
