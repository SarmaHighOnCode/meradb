// cpp/include/meradb/gui_platform.h -- the few things the window asks the operating system for.
#pragma once
#include <string>

namespace meradb::gui {

// Where the system's input method (IME) puts its composition string and its candidate list, in window pixels, for a
// caret whose top-left is (x, y) and whose line is `lineHeight` tall: the composition starts at the caret and the
// candidate list hangs just below the line.
struct ImePlacement {
    int compositionX = 0, compositionY = 0;
    int candidateX = 0, candidateY = 0;
};
ImePlacement imePlacement(float x, float y, float lineHeight);

// The smallest window, in physical pixels: 640 x 400 logical pixels times the monitor's content scale, so that at 200%
// the top bar and the tabs have the room they have at 100% (at 1280 x 800 physical they used to overlap).
struct WindowSize {
    int width = 0, height = 0;
};
constexpr int kMinLogicalWidth = 640, kMinLogicalHeight = 400;
WindowSize minimumWindowSize(float scale);

// "C:\x\exports\a.csv" -> "C:\x\exports" (either slash; a bare name -> "."; a root stays a root).
std::string folderOf(const std::string& path);

// Shows `folder` in the system's file manager: ShellExecute on Windows, `open` on macOS, `xdg-open` elsewhere.
// Returns false when the helper could not be started.
bool openFolder(const std::string& folder);

}  // namespace meradb::gui
