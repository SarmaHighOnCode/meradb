// cpp/src/gui_platform.cpp -- see gui_platform.h.
#include "meradb/gui_platform.h"
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace meradb::gui {

ImePlacement imePlacement(float x, float y, float lineHeight) {
    ImePlacement p;
    p.compositionX = p.candidateX = static_cast<int>(x);
    p.compositionY = static_cast<int>(y);
    p.candidateY = static_cast<int>(y + lineHeight + 2.0f);
    return p;
}

std::string folderOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    if (slash == 0) return path.substr(0, 1);
    if (slash == 2 && path[1] == ':') return path.substr(0, 3);   // "C:\file" -> "C:\"
    return path.substr(0, slash);
}

#if defined(_WIN32)
bool openFolder(const std::string& folder) {
    std::wstring wide(folder.size() + 1, L' ');
    const int n = MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), static_cast<int>(folder.size()), &wide[0], static_cast<int>(wide.size()));
    wide.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}
#else
bool openFolder(const std::string& folder) {
#if defined(__APPLE__)
    const char* tool = "open";
#else
    const char* tool = "xdg-open";
#endif
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execlp(tool, tool, folder.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;   // the helper is not waited for; it is a short-lived child that init collects
}
#endif

}  // namespace meradb::gui
