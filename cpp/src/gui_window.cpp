// cpp/src/gui_window.cpp -- the window: GLFW + OpenGL 3 + Dear ImGui, the idle main loop, the quit protocol and the
// screenshot mode. All widgets live in gui_ui.cpp; the session (wb_session.h) is shared with the terminal workbench.
//
// Threading: everything here runs on the main thread. The session's worker reaches it only through UiPostQueue,
// whose wake function is glfwPostEmptyEvent (the one GLFW call that may be made from any thread).
#include "meradb/gui_app.h"
#include "meradb/gui_fonts.h"
#include "meradb/gui_gl.h"
#include "meradb/gui_icon.h"
#include "meradb/gui_platform.h"
#include "meradb/gui_poster.h"
#include "meradb/gui_ui.h"
#include "meradb/wb_session.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#if defined(_WIN32)
#include <windows.h>
#include <imm.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <thread>

namespace meradb::gui {

namespace {

std::string g_lastGlfwError;
void onGlfwError(int, const char* description) { g_lastGlfwError = description != nullptr ? description : ""; }

// glfwInit / glfwTerminate as a scope.
struct GlfwLibrary {
    bool ok = false;
    GlfwLibrary() {
        glfwSetErrorCallback(onGlfwError);
        ok = glfwInit() == GLFW_TRUE;
    }
    ~GlfwLibrary() {
        if (ok) glfwTerminate();
    }
    GlfwLibrary(const GlfwLibrary&) = delete;
    GlfwLibrary& operator=(const GlfwLibrary&) = delete;
};

// What the GLFW callbacks need (they have no closure): reached through the window's user pointer.
struct WindowHooks {
    std::function<void()> redraw;
    std::function<void()> closeRequested;
};

// The monitor's scale for the interface. On macOS (and Wayland) the framebuffer scale already makes Dear ImGui draw
// at full resolution, so the helper answers 1 there; Windows and X11 get the real content scale.
float contentScaleOf(GLFWwindow* window) {
    const float scale = ImGui_ImplGlfw_GetContentScaleForWindow(window);
    return scale > 0.0f ? std::min(scale, 6.0f) : 1.0f;
}

#if defined(__APPLE__)
constexpr const char* kGlslVersion = "#version 150";
#else
constexpr const char* kGlslVersion = "#version 130";
#endif

void closeQuietly(Backend& backend) {
    try {
        backend.close();
    } catch (...) {
    }
}

#if defined(_WIN32)
// The editor tells Dear ImGui where its caret is (io.PlatformImeData); the GLFW backend does not pass that on, so the
// Windows input method gets it here: the composition string starts at the caret and the candidate list sits below.
// Other systems: no hook (GLFW has no IME API there); typed text still arrives as characters, only the candidate
// window is not placed at the caret.
GLFWwindow* g_imeWindow = nullptr;

void setImeData(ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
    if (data == nullptr || !data->WantVisible || g_imeWindow == nullptr) return;
    HWND hwnd = viewport != nullptr ? static_cast<HWND>(viewport->PlatformHandleRaw) : nullptr;
    if (hwnd == nullptr) hwnd = glfwGetWin32Window(g_imeWindow);
    if (hwnd == nullptr) return;
    HIMC context = ImmGetContext(hwnd);
    if (context == nullptr) return;
    const ImePlacement at = imePlacement(data->InputPos.x, data->InputPos.y, data->InputLineHeight);
    COMPOSITIONFORM composition = {};
    composition.dwStyle = CFS_FORCE_POSITION;
    composition.ptCurrentPos.x = at.compositionX;
    composition.ptCurrentPos.y = at.compositionY;
    ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidate = {};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos.x = at.candidateX;
    candidate.ptCurrentPos.y = at.candidateY;
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(hwnd, context);
}
#endif

constexpr unsigned kColorBufferBit = 0x00004000;

}  // namespace

int runGui(std::unique_ptr<Backend> backend, const GuiOptions& options, const Reporter& reporter) {
    const bool screenshot = options.screenshot.has_value();
    UiPostQueue poster;
    GlfwLibrary glfw;
    if (!glfw.ok) {
        reporter.error("Window nahi khul saka (display ya OpenGL nahi mila): " + g_lastGlfwError);
        closeQuietly(*backend);
        return kExitNoDisplay;
    }

#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    if (screenshot) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    } else {
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);   // the 1280x800 is in unscaled pixels
    }
    GLFWwindow* window = glfwCreateWindow(options.width, options.height, "MeraDB Workbench", nullptr, nullptr);
    if (window == nullptr) {
        reporter.error("Window nahi khul saka (OpenGL 3 chahiye): " + g_lastGlfwError);
        closeQuietly(*backend);
        return kExitNoDisplay;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(screenshot ? 0 : 1);
    glfwSetWindowSizeLimits(window, 640, 400, GLFW_DONT_CARE, GLFW_DONT_CARE);
#if !defined(__APPLE__)   // macOS takes the icon from the application bundle
    {
        const std::vector<IconImage> icons = windowIconImages();
        std::vector<GLFWimage> images;
        for (const IconImage& icon : icons) images.push_back(GLFWimage{icon.width, icon.height, const_cast<unsigned char*>(icon.rgba.data())});
        if (!images.empty()) glfwSetWindowIcon(window, static_cast<int>(images.size()), images.data());
    }
#endif

    GlApi gl;
    if (!gl.load()) {
        reporter.error("OpenGL ke zaroori functions nahi mile (OpenGL 3 chahiye).");
        glfwDestroyWindow(window);
        closeQuietly(*backend);
        return kExitNoDisplay;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // sizes are kept for the session only
    io.LogFilename = nullptr;
    const FontSet fonts = loadFonts(io);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(kGlslVersion);
#if defined(_WIN32)
    g_imeWindow = window;
    ImGui::GetPlatformIO().Platform_SetImeDataFn = &setImeData;
#endif

    int exitCode = 0;
    try {
        poster.setWake([] { glfwPostEmptyEvent(); });
        wb::SessionOptions sessionOptions;
        sessionOptions.dataDir = options.cli.dataDir;
        bool leave = false;
        wb::Session session(std::move(backend), sessionOptions, [&poster](std::function<void()> fn) { poster.post(std::move(fn)); });
        session.setOnExit([&leave] {
            leave = true;   // on the UI thread, from poster.drain()
            glfwPostEmptyEvent();
        });

        const ThemeMode mode = options.theme ? *options.theme : (screenshot ? ThemeMode::Dark : detectSystemTheme());
        GuiUi ui(session, fonts, mode);

        bool inFrame = false;
        auto frame = [&](bool present, OffscreenTarget* target) {
            if (inFrame) return;
            inFrame = true;
            ui.prepare(screenshot ? 1.0f : contentScaleOf(window));
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            int framebufferWidth = 0, framebufferHeight = 0;
            if (screenshot) {
                io.DisplaySize = ImVec2(static_cast<float>(options.width), static_cast<float>(options.height));
                io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                framebufferWidth = options.width;
                framebufferHeight = options.height;
            } else {
                glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
            }
            ImGui::NewFrame();
            ui.draw();
            ImGui::Render();
            if (target != nullptr) target->bind();
            gl.Viewport(0, 0, framebufferWidth, framebufferHeight);
            const ImVec4 clear = toVec4(ui.theme().background);
            gl.ClearColor(clear.x, clear.y, clear.z, 1.0f);
            gl.Clear(kColorBufferBit);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (present) glfwSwapBuffers(window);
            inFrame = false;
        };

        WindowHooks hooks;
        hooks.redraw = [&] { frame(true, nullptr); };
        hooks.closeRequested = [&] { session.requestQuit(); };   // the window stays until the session says it is done
        glfwSetWindowUserPointer(window, &hooks);
        glfwSetWindowRefreshCallback(window, [](GLFWwindow* w) {
            auto* h = static_cast<WindowHooks*>(glfwGetWindowUserPointer(w));
            if (h != nullptr && h->redraw) h->redraw();
        });
        glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
            glfwSetWindowShouldClose(w, GLFW_FALSE);
            auto* h = static_cast<WindowHooks*>(glfwGetWindowUserPointer(w));
            if (h != nullptr && h->closeRequested) h->closeRequested();
        });

        if (screenshot) {
            if (options.script) session.runText(*options.script);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            for (;;) {
                poster.drain();
                if (!session.busy()) break;
                if (std::chrono::steady_clock::now() > deadline) {
                    reporter.error("Screenshot: statement 60 second mein khatam nahi hua.");
                    exitCode = 1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (options.editorText) session.editor().setText(*options.editorText);
            if (options.expandTables) {
                for (int i = 0; i < static_cast<int>(session.tree().rows().size()); ++i) {   // the rows shift as tables open
                    const wb::TreeRow& row = session.tree().rows()[static_cast<std::size_t>(i)];
                    if (row.key.kind == wb::NodeKey::Kind::Table && !row.expanded) session.tree().toggle(i);
                }
            }
            if (options.open) {
                if (*options.open == "connect") session.openConnectDialog();
                else if (*options.open == "madad") session.showHelp();
                else if (*options.open == "palette") ui.openPalette(options.typeText.value_or(""));
            }
            if (exitCode == 0) {
                OffscreenTarget target;
                if (!target.create(gl, options.width, options.height)) {
                    reporter.error("Screenshot: off-screen image nahi ban saki (OpenGL 3 framebuffer chahiye).");
                    exitCode = kExitNoDisplay;
                } else {
                    for (int i = 0; i < 6; ++i) frame(false, &target);   // fonts and layout settle over a few frames
                    std::vector<unsigned char> rgba;
                    std::string error;
                    if (!target.read(rgba) || !writePng(*options.screenshot, options.width, options.height, rgba.data(), error)) {
                        reporter.error(error.empty() ? std::string("Screenshot: pixels padhne mein dikkat.") : error);
                        exitCode = 1;
                    }
                }
            }
            session.requestQuit();
            const auto quitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!leave) {   // the backend closes (an open transaction rolls back), then onExit fires
                poster.drain();
                if (leave) break;
                if (std::chrono::steady_clock::now() > quitDeadline) {
                    reporter.error("Screenshot: band hone mein 60 second se zyada lage.");
                    exitCode = 1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } else {
            while (!leave) {
                const bool iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE;
                const bool busy = session.busy() || session.quitting() || ui.wantsFastTick();
                const double timeout = busy ? 0.05 : (iconified ? 1.0 : 0.5);   // idle: almost no CPU
                const double before = glfwGetTime();
                glfwWaitEventsTimeout(timeout);
                const bool woken = glfwGetTime() - before < timeout - 0.005;
                poster.drain();
                if (leave) break;
                if (iconified) continue;
                frame(true, nullptr);
                if (woken) frame(true, nullptr);   // a second frame settles hover and layout after an input
            }
        }
        session.shutdown();
        poster.close();
        glfwSetWindowRefreshCallback(window, nullptr);
        glfwSetWindowCloseCallback(window, nullptr);
        glfwSetWindowUserPointer(window, nullptr);
    } catch (const std::exception& e) {
        reporter.error(std::string("MeraDB Workbench mein dikkat: ") + e.what());
        exitCode = 1;
    }
    poster.close();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
#if defined(_WIN32)
    g_imeWindow = nullptr;
#endif
    glfwDestroyWindow(window);
    return exitCode;
}

}  // namespace meradb::gui
