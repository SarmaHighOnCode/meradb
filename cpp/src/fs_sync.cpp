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

namespace {
SyncObserver g_syncObserver = nullptr;
void observeSync(const std::string& utf8) {
    if (g_syncObserver != nullptr) g_syncObserver(utf8);
}
}  // namespace

void setSyncObserver(SyncObserver observer) { g_syncObserver = observer; }

#ifdef _WIN32
bool syncFile(const std::string& utf8) {
    observeSync(utf8);
    HANDLE file = CreateFileW(pathOf(utf8).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    return ok;
}

bool syncFolder(const std::string& utf8) {
    observeSync(utf8);
    // NTFS journals a rename, so a crash leaves it done or not done, but only this forces it out NOW. A file system that
    // refuses (FAT, some network shares) has nothing better to offer.
    HANDLE folder = CreateFileW(pathOf(utf8).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (folder == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(folder) != 0;
    CloseHandle(folder);
    return ok;
}
#else
bool syncFile(const std::string& utf8) {
    observeSync(utf8);
    const int fd = ::open(utf8.c_str(), O_RDWR);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

bool syncFolder(const std::string& utf8) {
    observeSync(utf8);
    const int fd = ::open(utf8.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;  // some file systems refuse this on a folder; the rename itself has already happened
    ::close(fd);
    return ok;
}
#endif

void syncFolderOf(const std::string& utf8) { syncFolder(textOf(pathOf(utf8).parent_path())); }

bool syncTree(const std::string& utf8) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(pathOf(utf8), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (it->is_directory(entryEc)) syncFolder(textOf(it->path()));
        else if (!syncFile(textOf(it->path()))) return false;
    }
    if (ec) return false;
    syncFolder(utf8);
    return true;
}

}  // namespace meradb
