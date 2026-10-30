// cpp/src/fs_sync.cpp -- see include/meradb/fs_util.h
#include "meradb/fs_util.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <filesystem>
#include <system_error>

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

#ifdef _WIN32
// A security descriptor that lets in the current user and nobody else, and takes nothing from the folder.
// Null when Windows will not say who the current user is; the file then gets the folder's rights.
class OwnerOnly {
public:
    OwnerOnly() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
        char buffer[256];
        DWORD size = sizeof buffer;
        const BOOL got = GetTokenInformation(token, TokenUser, buffer, size, &size);
        CloseHandle(token);
        LPWSTR sid = nullptr;
        if (!got || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid)) return;
        const std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) descriptor_ = nullptr;
    }
    ~OwnerOnly() {
        if (descriptor_ != nullptr) LocalFree(descriptor_);
    }
    OwnerOnly(const OwnerOnly&) = delete;
    OwnerOnly& operator=(const OwnerOnly&) = delete;
    PSECURITY_DESCRIPTOR get() const { return descriptor_; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
};

// A file from before this rule (or written by the Python engine, which inherits the folder's rights). Best effort: on
// a file system without rights, or a file this user may not change, it stays as it is.
void restrictToOwner(const std::string& path) {
    OwnerOnly owner;
    if (owner.get() == nullptr) return;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(owner.get(), &present, &dacl, &defaulted) || !present) return;
    std::wstring wide = pathOf(path).wstring();
    SetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
}

// Creates a file that must not exist yet, readable by its owner only from the first moment, and writes `text` into it.
bool writeNewPrivateFile(const std::string& path, const std::string& text) {
    OwnerOnly owner;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof attributes;
    attributes.lpSecurityDescriptor = owner.get();
    HANDLE file = CreateFileW(pathOf(path).c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == text.size();
}
#else
void restrictToOwner(const std::string& path) {
    std::error_code ignored;
    std::filesystem::permissions(pathOf(path), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ignored);
}

// O_EXCL | O_NOFOLLOW: a link planted at this name is never written through.
bool writeNewPrivateFile(const std::string& path, const std::string& text) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return done == text.size();
}
#endif

}  // namespace meradb
