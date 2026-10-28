// cpp/src/gui_gl.cpp -- see gui_gl.h.
#include "meradb/gui_gl.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <filesystem>
#include <fstream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace meradb::gui {

namespace {

constexpr unsigned kTexture2D = 0x0DE1, kRgba = 0x1908, kUnsignedByte = 0x1401, kTextureMinFilter = 0x2801,
                   kTextureMagFilter = 0x2800, kNearest = 0x2600, kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0,
                   kFramebufferComplete = 0x8CD5, kPackAlignment = 0x0D05;

template <typename F>
bool fetch(F& slot, const char* name) {
    slot = reinterpret_cast<F>(glfwGetProcAddress(name));
    return slot != nullptr;
}

}  // namespace

bool GlApi::load() {
    bool ok = true;
    ok &= fetch(Viewport, "glViewport");
    ok &= fetch(ClearColor, "glClearColor");
    ok &= fetch(Clear, "glClear");
    ok &= fetch(ReadPixels, "glReadPixels");
    ok &= fetch(PixelStorei, "glPixelStorei");
    ok &= fetch(GenTextures, "glGenTextures");
    ok &= fetch(BindTexture, "glBindTexture");
    ok &= fetch(TexImage2D, "glTexImage2D");
    ok &= fetch(TexParameteri, "glTexParameteri");
    ok &= fetch(DeleteTextures, "glDeleteTextures");
    ok &= fetch(GenFramebuffers, "glGenFramebuffers");
    ok &= fetch(BindFramebuffer, "glBindFramebuffer");
    ok &= fetch(FramebufferTexture2D, "glFramebufferTexture2D");
    ok &= fetch(CheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= fetch(DeleteFramebuffers, "glDeleteFramebuffers");
    ok &= fetch(Finish, "glFinish");
    return ok;
}

OffscreenTarget::~OffscreenTarget() {
    if (gl_ == nullptr) return;
    if (framebuffer_ != 0) gl_->DeleteFramebuffers(1, &framebuffer_);
    if (texture_ != 0) gl_->DeleteTextures(1, &texture_);
}

bool OffscreenTarget::create(const GlApi& gl, int width, int height) {
    gl_ = &gl;
    width_ = width;
    height_ = height;
    gl.GenTextures(1, &texture_);
    gl.BindTexture(kTexture2D, texture_);
    gl.TexParameteri(kTexture2D, kTextureMinFilter, static_cast<int>(kNearest));
    gl.TexParameteri(kTexture2D, kTextureMagFilter, static_cast<int>(kNearest));
    gl.TexImage2D(kTexture2D, 0, static_cast<int>(kRgba), width, height, 0, kRgba, kUnsignedByte, nullptr);
    gl.GenFramebuffers(1, &framebuffer_);
    gl.BindFramebuffer(kFramebuffer, framebuffer_);
    gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, texture_, 0);
    const bool complete = gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    gl.BindFramebuffer(kFramebuffer, 0);
    if (!complete) {
        gl.DeleteFramebuffers(1, &framebuffer_);
        gl.DeleteTextures(1, &texture_);
        framebuffer_ = 0;
        texture_ = 0;
    }
    return complete;
}

void OffscreenTarget::bind() { gl_->BindFramebuffer(kFramebuffer, framebuffer_); }
void OffscreenTarget::unbind() { gl_->BindFramebuffer(kFramebuffer, 0); }

bool OffscreenTarget::read(std::vector<unsigned char>& rgba) {
    if (!valid()) return false;
    std::vector<unsigned char> bottomUp(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4);
    gl_->BindFramebuffer(kFramebuffer, framebuffer_);
    gl_->PixelStorei(kPackAlignment, 1);
    gl_->Finish();
    gl_->ReadPixels(0, 0, width_, height_, kRgba, kUnsignedByte, bottomUp.data());
    gl_->BindFramebuffer(kFramebuffer, 0);
    rgba.assign(bottomUp.size(), 0);
    const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4;
    for (int y = 0; y < height_; ++y)
        std::copy_n(bottomUp.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(height_ - 1 - y) * rowBytes), rowBytes,
                    rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * rowBytes));
    return true;
}

namespace {
void appendToStream(void* context, void* data, int size) {
    static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data), size);
}
}  // namespace

bool writePng(const std::string& path, int width, int height, const unsigned char* rgba, std::string& error) {
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    for (std::size_t i = 0, n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height); i < n; ++i) {
        rgb[i * 3] = rgba[i * 4];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "File nahi khuli: " + path;
        return false;
    }
    if (stbi_write_png_to_func(appendToStream, &out, width, height, 3, rgb.data(), width * 3) == 0) {
        error = "PNG likhne mein dikkat: " + path;
        return false;
    }
    out.flush();
    if (!out) {
        error = "PNG likhne mein dikkat: " + path;
        return false;
    }
    return true;
}

}  // namespace meradb::gui
