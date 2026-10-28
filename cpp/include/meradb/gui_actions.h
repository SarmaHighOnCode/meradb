// cpp/include/meradb/gui_actions.h -- what a key, a button or a command menu item can ask the window to do.
#pragma once

namespace meradb::gui {

enum class Action { Run, Explain, Previous, Next, ExportCsv, ClearLog, Quit, Connect };

}  // namespace meradb::gui
