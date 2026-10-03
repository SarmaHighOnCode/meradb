// cpp/include/meradb/wb_ui.h -- the workbench window: layout, header, footer, key routing and focus.
#pragma once
#include "meradb/wb_panels.h"
#include "meradb/wb_session.h"
#include <ftxui/component/component_base.hpp>
#include <memory>

namespace meradb::wb {

class WorkbenchUi {
public:
    explicit WorkbenchUi(Session& session);
    ~WorkbenchUi();
    WorkbenchUi(const WorkbenchUi&) = delete;
    WorkbenchUi& operator=(const WorkbenchUi&) = delete;
    ftxui::Component component() const;     // the root: Render() draws everything, OnEvent() routes every key
    ftxui::Element render();                // component()->Render()
    bool onEvent(const ftxui::Event& e);    // component()->OnEvent(e)
    Panel focus() const;
    void setFocus(Panel panel);

    static constexpr int kMinWidth = 60;
    static constexpr int kMinHeight = 24;
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace meradb::wb
