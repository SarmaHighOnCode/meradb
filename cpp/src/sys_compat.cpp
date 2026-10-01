// cpp/src/sys_compat.cpp
#include "meradb/sys_compat.h"
#include "meradb/errors.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace meradb::sys {

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

}  // namespace
#endif

std::optional<std::string> getEnv(const std::string& name) {
#ifdef _WIN32
    // The wide API: the narrow one hands back the ANSI code page, not UTF-8.
    const std::wstring wideName = widen(name);
    SetLastError(0);
    DWORD size = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (size == 0) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? std::nullopt : std::optional<std::string>("");
    std::wstring value(size, L'\0');
    DWORD length = GetEnvironmentVariableW(wideName.c_str(), &value[0], size);
    value.resize(length);
    return narrow(value);
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

std::int64_t processId() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

namespace {

std::tm localTm(std::time_t t) {
    std::tm out{};
#if defined(_MSC_VER)
    if (localtime_s(&out, &t) != 0) throw StorageError("local time nahi mila");
#elif defined(_WIN32)
    // MinGW: std::localtime uses a static buffer, so serialise the calls.
    static std::mutex m;
    std::lock_guard<std::mutex> guard(m);
    const std::tm* p = std::localtime(&t);
    if (p == nullptr) throw StorageError("local time nahi mila");
    out = *p;
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::string formatNow(const char* format) {
    std::tm tmv = localTm(std::time(nullptr));
    char buffer[64];
    std::size_t n = std::strftime(buffer, sizeof buffer, format, &tmv);
    return std::string(buffer, n);
}

}  // namespace

std::string localIsoSeconds() { return formatNow("%Y-%m-%dT%H:%M:%S"); }
std::string localLogStamp() { return formatNow("%Y-%m-%d %H:%M:%S"); }

std::vector<std::uint8_t> randomBytes(std::size_t count) {
    std::vector<std::uint8_t> out(count);
    if (count == 0) return out;
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw StorageError("OS random generator (BCryptGenRandom) ne jawab nahi diya");
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom) throw StorageError("/dev/urandom khul nahi paaya");
    urandom.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count));
    if (!urandom) throw StorageError("/dev/urandom se random bytes nahi mile");
#endif
    return out;
}

std::string homeDir() {
#ifdef _WIN32
    // Python's ntpath.expanduser("~"): USERPROFILE, else HOMEDRIVE + HOMEPATH; HOME is ignored.
    // With neither set the "~" is left as is.
    if (auto v = getEnv("USERPROFILE")) return *v;
    if (auto path = getEnv("HOMEPATH")) {
        std::string drive = getEnv("HOMEDRIVE").value_or("");
        return drive + *path;
    }
    return "~";
#else
    if (auto v = getEnv("HOME"); v && !v->empty()) return *v;
    return ".";
#endif
}

// ---------------------------------------------------------------------------
// environment, terminal, processes
// ---------------------------------------------------------------------------

void setEnv(const std::string& name, const std::string& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(widen(name).c_str(), value.empty() ? nullptr : widen(value).c_str());
#else
    ::setenv(name.c_str(), value.c_str(), 1);
#endif
}

std::vector<std::string> commandLineArgs(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    // argv is in the ANSI code page; the wide command line is lossless.
    (void)argc;
    (void)argv;
    int count = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) args.push_back(narrow(wide[i]));
        LocalFree(wide);
        return args;
    }
#endif
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    return args;
}

std::string readHidden(const std::string& prompt) {
    std::cerr << prompt << std::flush;
    std::string line;
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool terminal = (in != INVALID_HANDLE_VALUE) && GetConsoleMode(in, &mode);
    if (terminal) SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    std::getline(std::cin, line);
    if (terminal) {
        SetConsoleMode(in, mode);
        std::cerr << "\n";
    }
#else
    termios saved{};
    const bool terminal = ::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &saved) == 0;
    if (terminal) {
        termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    std::getline(std::cin, line);
    if (terminal) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        std::cerr << "\n";
    }
#endif
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

#ifdef _WIN32
namespace {

// The rules of CommandLineToArgvW, run backwards.
std::wstring quoteArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');  // a trailing run must not escape the closing quote
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

}  // namespace
#endif

std::string executablePath() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
    buffer.resize(n);
    return narrow(buffer);
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) != 0) return std::string();
    return std::string(buffer);
#else
    char buffer[4096];
    auto n = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n <= 0) return std::string();
    return std::string(buffer, static_cast<std::size_t>(n));
#endif
}

struct DetachedProcess::Impl {
    std::int64_t pid = 0;
    bool done = false;
#ifdef _WIN32
    HANDLE process = nullptr;
#endif
};

DetachedProcess::DetachedProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DetachedProcess::~DetachedProcess() {
#ifdef _WIN32
    if (impl_ && impl_->process != nullptr) CloseHandle(impl_->process);
#endif
}

std::int64_t DetachedProcess::pid() const { return impl_->pid; }

bool DetachedProcess::exited() {
    if (impl_->done) return true;
#ifdef _WIN32
    if (WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) impl_->done = true;
#else
    int status = 0;
    pid_t result = ::waitpid(static_cast<pid_t>(impl_->pid), &status, WNOHANG);
    if (result != 0) impl_->done = true;  // > 0: it ended and is reaped now; -1: it is not our child (any more)
#endif
    return impl_->done;
}

std::unique_ptr<DetachedProcess> spawnDetached(const std::string& exePath, const std::vector<std::string>& args,
                                               const std::string& logPath) {
    auto impl = std::make_unique<DetachedProcess::Impl>();
#ifdef _WIN32
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(widen(logPath).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
        throw StorageError("Log file nahi khuli: " + logPath + " (" + std::system_category().message(static_cast<int>(GetLastError())) + ")");
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring commandLine = quoteArgument(widen(exePath));
    for (const auto& arg : args) commandLine += L" " + quoteArgument(widen(arg));

    // Inherit ONLY the log and NUL handles. With plain bInheritHandles the child would also inherit every other
    // inheritable handle of this process -- e.g. the write end of a pipe that our caller reads until EOF, which
    // would then never end for as long as the server lives.
    HANDLE inherited[2] = {nul, log};
    const DWORD inheritedCount = nul != INVALID_HANDLE_VALUE ? 2 : 1;
    HANDLE* inheritedList = nul != INVALID_HANDLE_VALUE ? inherited : inherited + 1;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<char> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedList,
                                   inheritedCount * sizeof(HANDLE), nullptr, nullptr)) {
        CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        throw StorageError("Process start nahi hua: " + exePath + " (handle list)");
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul;
    startup.StartupInfo.hStdOutput = log;
    startup.StartupInfo.hStdError = log;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    BOOL started = CreateProcessW(widen(exePath).c_str(), &commandLine[0], nullptr, nullptr, TRUE,
                                  DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                  nullptr, &startup.StartupInfo, &info);
    const DWORD failure = started ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started)
        throw StorageError("Process start nahi hua: " + exePath + " (" + std::system_category().message(static_cast<int>(failure)) + ")");
    CloseHandle(info.hThread);
    impl->process = info.hProcess;
    impl->pid = static_cast<std::int64_t>(info.dwProcessId);
#else
    // Everything that allocates happens BEFORE fork(): the child may only call async-signal-safe functions.
    std::vector<std::string> storage;
    storage.push_back(exePath);
    for (const auto& arg : args) storage.push_back(arg);
    std::vector<char*> argv;
    for (auto& s : storage) argv.push_back(&s[0]);
    argv.push_back(nullptr);

    int logFd = ::open(logPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (logFd < 0) throw StorageError("Log file nahi khuli: " + logPath + " (" + std::system_category().message(errno) + ")");
    int nullFd = ::open("/dev/null", O_RDONLY);

    pid_t child = ::fork();
    if (child < 0) {
        int e = errno;
        ::close(logFd);
        if (nullFd >= 0) ::close(nullFd);
        throw StorageError("Process start nahi hua: " + exePath + " (" + std::system_category().message(e) + ")");
    }
    if (child == 0) {
        ::setsid();  // no controlling terminal: survives the parent's shell closing
        if (nullFd >= 0) ::dup2(nullFd, STDIN_FILENO);
        ::dup2(logFd, STDOUT_FILENO);
        ::dup2(logFd, STDERR_FILENO);
        ::execv(exePath.c_str(), argv.data());
        ::_exit(127);  // exec failed
    }
    ::close(logFd);
    if (nullFd >= 0) ::close(nullFd);
    impl->pid = static_cast<std::int64_t>(child);
#endif
    return std::make_unique<DetachedProcess>(std::move(impl));
}

std::string killProcess(std::int64_t pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) return std::system_category().message(static_cast<int>(GetLastError()));
    const BOOL ok = TerminateProcess(process, 1);
    const DWORD failure = ok ? 0 : GetLastError();
    CloseHandle(process);
    return ok ? std::string() : std::system_category().message(static_cast<int>(failure));
#else
    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) return std::system_category().message(errno);
    return std::string();
#endif
}

}  // namespace meradb::sys
