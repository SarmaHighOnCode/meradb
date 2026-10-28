// cpp/tests/gui_test_util.h -- a Dear ImGui context without a window or a renderer, for running real frames of the
// desktop window's layout in a test.
#pragma once
#include "imgui.h"
#include "meradb/gui_fonts.h"
#include "meradb/gui_ui.h"
#include <stdexcept>
#include <string>

namespace guitest {

struct ImGuiAssertion : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void throwingAssertHandler(const char* expression, const char* file, int line) {
    throw ImGuiAssertion(std::string("Dear ImGui assertion: ") + expression + " (" + file + ":" + std::to_string(line) + ")");
}

// Creates a context, sets the display size by hand, loads the embedded fonts and turns a failed IM_ASSERT into an
// exception for as long as it lives. There is no renderer: frame() plays its part for the font texture (marks it
// uploaded), which is all Dear ImGui needs to keep going.
class HeadlessImGui {
public:
    explicit HeadlessImGui(float width = 1280.0f, float height = 800.0f) {
        previous_ = meradb::gui::assertHandler();
        meradb::gui::assertHandler() = &throwingAssertHandler;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(width, height);
        fonts_ = meradb::gui::loadFonts(io);
    }
    ~HeadlessImGui() {
        ImGui::DestroyContext();
        meradb::gui::assertHandler() = previous_;
    }
    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    const meradb::gui::FontSet& fonts() const { return fonts_; }
    void setDisplaySize(float width, float height) { ImGui::GetIO().DisplaySize = ImVec2(width, height); }

    // One frame of the whole window.
    void frame(meradb::gui::GuiUi& ui, float scale = 1.0f) {
        frameWith([&ui] { ui.draw(); }, [&ui, scale] { ui.prepare(scale); });
    }

    // One frame with `draw` between NewFrame and Render; `prepare` runs before NewFrame.
    template <typename Draw, typename Prepare>
    void frameWith(Draw&& draw, Prepare&& prepare) {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        prepare();
        ImGui::NewFrame();
        draw();
        ImGui::Render();
        playRenderer();
    }

    int vertexCount() const { return ImGui::GetDrawData() != nullptr ? ImGui::GetDrawData()->TotalVtxCount : 0; }

private:
    void playRenderer() {
        ImDrawData* data = ImGui::GetDrawData();
        if (data == nullptr || data->Textures == nullptr) return;
        for (ImTextureData* texture : *data->Textures) {
            if (texture->Status == ImTextureStatus_WantCreate) texture->SetTexID(static_cast<ImTextureID>(1));
            if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
                texture->SetStatus(ImTextureStatus_OK);
            if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0) {
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    meradb::gui::AssertHandler previous_ = nullptr;
    meradb::gui::FontSet fonts_;
};

}  // namespace guitest
