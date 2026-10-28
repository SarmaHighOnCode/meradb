// cpp/include/meradb/gui_imconfig.h -- Dear ImGui's configuration for the desktop window (IMGUI_USER_CONFIG).
// Only change: a failed IM_ASSERT calls a replaceable handler. The program's handler prints the failed
// condition and aborts; the headless UI tests install one that throws, so an ID conflict or a misuse of the API
// fails the test that caused it instead of ending the test run.
#pragma once
#include <cstdio>
#include <cstdlib>

namespace meradb::gui {

using AssertHandler = void (*)(const char* expression, const char* file, int line);

inline void abortingAssertHandler(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "Dear ImGui assertion failed: %s (%s:%d)\n", expression, file, line);
    std::fflush(stderr);
    std::abort();
}

// One handler for the whole program (an inline function's static is shared by every translation unit).
inline AssertHandler& assertHandler() {
    static AssertHandler handler = &abortingAssertHandler;
    return handler;
}

}  // namespace meradb::gui

#define IM_ASSERT(_EXPR) ((void)((_EXPR) ? (void)0 : ::meradb::gui::assertHandler()(#_EXPR, __FILE__, __LINE__)))

// Characters beyond the BMP (emoji) reach the editor as one code point.
#define IMGUI_USE_WCHAR32
