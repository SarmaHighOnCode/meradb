// cpp/include/meradb/gui_platform.h -- the few things the window asks the operating system for.
#pragma once
#include <string>

namespace meradb::gui {

// "C:\x\exports\a.csv" -> "C:\x\exports" (either slash; a bare name -> "."; a root stays a root).
std::string folderOf(const std::string& path);

// Shows `folder` in the system's file manager: ShellExecute on Windows, `open` on macOS, `xdg-open` elsewhere.
// Returns false when the helper could not be started.
bool openFolder(const std::string& folder);

}  // namespace meradb::gui
