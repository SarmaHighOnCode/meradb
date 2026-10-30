// cpp/src/fs_sync.cpp -- see include/meradb/fs_util.h
#include "meradb/fs_util.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace meradb {

#ifdef _WIN32
bool syncFile(const std::string& utf8) {
    HANDLE file = CreateFileW(pathOf(utf8).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    return ok;
}

void syncFolderOf(const std::string&) {}
#else
bool syncFile(const std::string& utf8) {
    const int fd = ::open(utf8.c_str(), O_RDWR);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

void syncFolderOf(const std::string& utf8) {
    const int fd = ::open(textOf(pathOf(utf8).parent_path()).c_str(), O_RDONLY);
    if (fd < 0) return;
    ::fsync(fd);   // some file systems refuse this on a folder; the rename itself has already happened
    ::close(fd);
}
#endif

}  // namespace meradb
