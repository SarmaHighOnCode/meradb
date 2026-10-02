// cpp/src/sys_compat.cpp
#include "meradb/sys_compat.h"
#include "meradb/errors.h"
#include <atomic>
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
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
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

#ifdef _WIN32
namespace {
bool isConsole(DWORD standardHandle) {
    HANDLE handle = GetStdHandle(standardHandle);
    DWORD mode = 0;
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0;
}
}  // namespace

Utf8Console::Utf8Console() {
    if (isConsole(STD_OUTPUT_HANDLE) || isConsole(STD_ERROR_HANDLE)) {
        const UINT before = GetConsoleOutputCP();
        if (before != CP_UTF8 && SetConsoleOutputCP(CP_UTF8)) savedOutput_ = before;
    }
    if (isConsole(STD_INPUT_HANDLE)) {
        const UINT before = GetConsoleCP();
        if (before != CP_UTF8 && SetConsoleCP(CP_UTF8)) savedInput_ = before;
    }
}

Utf8Console::~Utf8Console() {
    if (savedOutput_ != 0) SetConsoleOutputCP(savedOutput_);
    if (savedInput_ != 0) SetConsoleCP(savedInput_);
}
#else
Utf8Console::Utf8Console() {}
Utf8Console::~Utf8Console() {}
#endif

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
    long openMax = ::sysconf(_SC_OPEN_MAX);  // looked up before fork(): the child may only call async-signal-safe functions
    const int closeUpTo = static_cast<int>(openMax > 0 && openMax < 65536 ? openMax : 65536);

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
        for (int fd = STDERR_FILENO + 1; fd < closeUpTo; ++fd) ::close(fd);  // like Python's close_fds: nothing else leaks in
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
    // kill(0, ...) signals our own process group and kill(-1, ...) everything we may signal: a
    // damaged pid file must never get that far.
    if (pid <= 1) return "pid galat hai: " + std::to_string(pid);
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

// ---------------------------------------------------------------------------
// the interactive terminal
// ---------------------------------------------------------------------------

bool isTerminal(int fd) {
#ifdef _WIN32
    if (fd == 0) return isConsole(STD_INPUT_HANDLE);
    if (fd == 1) return isConsole(STD_OUTPUT_HANDLE);
    if (fd == 2) return isConsole(STD_ERROR_HANDLE);
    return false;
#else
    return ::isatty(fd) != 0;
#endif
}

#ifdef _WIN32
namespace {
constexpr DWORD kVirtualTerminalProcessing = 0x0004;  // ENABLE_VIRTUAL_TERMINAL_PROCESSING (older headers lack the name)

// The console mode to put back, while a change is outstanding. The destructor restores it on a normal return;
// the atexit handler does the same when the process leaves through std::exit() with a console still switched.
std::mutex g_ansiMutex;
bool g_ansiPending = false;
DWORD g_ansiSavedMode = 0;
bool g_ansiAtexitRegistered = false;

void restoreAnsiMode() {
    std::lock_guard<std::mutex> lock(g_ansiMutex);
    if (!g_ansiPending) return;
    g_ansiPending = false;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) SetConsoleMode(out, g_ansiSavedMode);
}
}  // namespace

AnsiConsole::AnsiConsole() {}

bool AnsiConsole::enable() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out == nullptr || out == INVALID_HANDLE_VALUE || !GetConsoleMode(out, &mode)) return false;
    if ((mode & kVirtualTerminalProcessing) != 0) return true;
    if (!SetConsoleMode(out, mode | kVirtualTerminalProcessing)) return false;
    savedMode_ = static_cast<unsigned>(mode);
    changed_ = true;
    std::lock_guard<std::mutex> lock(g_ansiMutex);
    g_ansiSavedMode = mode;
    g_ansiPending = true;
    if (!g_ansiAtexitRegistered) {
        g_ansiAtexitRegistered = true;
        std::atexit(restoreAnsiMode);
    }
    return true;
}

AnsiConsole::~AnsiConsole() {
    if (changed_) restoreAnsiMode();
}
#else
AnsiConsole::AnsiConsole() {}
AnsiConsole::~AnsiConsole() {}
bool AnsiConsole::enable() { return true; }
#endif

namespace {

std::atomic<bool> g_interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the Ctrl+C flag is set from a signal handler / handler thread");

#ifdef _WIN32
// A real handle to the thread that created the guard (the one that reads input). The control handler runs on a
// thread of its own, so the handle is only used, closed or cleared under this mutex: the handler can never
// pass CancelSynchronousIo a handle the destructor has already closed.
std::mutex g_readerThreadMutex;
HANDLE g_readerThread = nullptr;

InterruptGate g_gate;

void cancelReaderRead() {
    std::lock_guard<std::mutex> lock(g_readerThreadMutex);
    if (g_readerThread != nullptr) CancelSynchronousIo(g_readerThread);
}

BOOL WINAPI onConsoleControl(DWORD event) {
    if (event != CTRL_C_EVENT) return FALSE;  // Ctrl+Break and the rest keep their default meaning
    g_interrupted = true;
    // Wake the console read, and ONLY that: the reader thread also runs statements (socket and file writes),
    // which must finish. The gate cancels while the thread is inside ReadConsoleW and never otherwise. If the
    // flag was set just before ReadConsoleW was entered the first cancel finds nothing to cancel, so repeat
    // (briefly) until the reader has left the read.
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (!g_gate.cancelIfReading(cancelReaderRead)) break;
        Sleep(5);
    }
    return TRUE;
}
#else
struct sigaction g_previousAction;

void onSigint(int) { g_interrupted = true; }
#endif

}  // namespace

InterruptGuard::InterruptGuard() {
    g_interrupted = false;
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(g_readerThreadMutex);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_readerThread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    }
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
#else
    struct sigaction action {};
    action.sa_handler = onSigint;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // no SA_RESTART: a read() waiting for a line must return EINTR
    sigaction(SIGINT, &action, &g_previousAction);
#endif
}

InterruptGuard::~InterruptGuard() {
#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleControl, FALSE);
    std::lock_guard<std::mutex> lock(g_readerThreadMutex);  // waits for a handler that is already running
    if (g_readerThread != nullptr) CloseHandle(g_readerThread);
    g_readerThread = nullptr;
#else
    sigaction(SIGINT, &g_previousAction, nullptr);
#endif
}

bool InterruptGate::enterRead(const std::atomic<bool>& interruptPending) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (interruptPending.load()) return false;
    inRead_ = true;
    return true;
}

void InterruptGate::leaveRead() {
    std::lock_guard<std::mutex> lock(mutex_);
    inRead_ = false;
}

bool InterruptGate::cancelIfReading(const std::function<void()>& cancel) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inRead_) return false;
    cancel();
    return true;
}

bool InterruptGuard::consume() { return g_interrupted.exchange(false); }

bool InterruptGuard::pending() { return g_interrupted.load(); }

void InterruptGuard::trigger() { g_interrupted = true; }

ReadStatus readTerminalLine(std::string& line) {
    line.clear();
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring text;
    for (;;) {
        wchar_t buffer[512];
        DWORD got = 0;
        // A Ctrl+C that arrived before the read (flag already set) is seen here: no read is started.
        if (!g_gate.enterRead(g_interrupted)) {
            InterruptGuard::consume();
            return ReadStatus::Interrupted;
        }
        const BOOL ok = ReadConsoleW(in, buffer, 512, &got, nullptr);
        const DWORD failure = ok ? 0 : GetLastError();
        g_gate.leaveRead();
        const bool aborted = !ok && failure == ERROR_OPERATION_ABORTED;
        const bool completeLine = ok && got > 0 && buffer[got - 1] == L'\n';
        // Ctrl+C with no finished line: discard the partial input. A line that was completed before the Ctrl+C
        // is kept; the flag stays set and the shell exits 130 after running it.
        if (aborted || (InterruptGuard::pending() && !completeLine)) {
            InterruptGuard::consume();
            return ReadStatus::Interrupted;
        }
        if (!ok || got == 0) {
            if (text.empty()) return ReadStatus::Eof;
            break;
        }
        text.append(buffer, got);
        if (text.back() == L'\n') break;  // a longer line arrives in several pieces
    }
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
    if (!text.empty() && text.front() == L'\x1a') return ReadStatus::Eof;  // Ctrl+Z, Enter
    line = narrow(text);
    return ReadStatus::Line;
#else
    // SIGINT stays blocked except inside pselect(): a Ctrl+C that arrives just before the wait cannot slip
    // between "check the flag" and "start waiting"; it stays pending and interrupts the pselect (EINTR).
    sigset_t blocked, original;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    pthread_sigmask(SIG_BLOCK, &blocked, &original);
    struct Restore {
        sigset_t* mask;
        ~Restore() { pthread_sigmask(SIG_SETMASK, mask, nullptr); }
    } restore{&original};
    for (;;) {
        if (g_interrupted.load()) {
            g_interrupted = false;
            return ReadStatus::Interrupted;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(STDIN_FILENO, &readable);
        const int ready = ::pselect(STDIN_FILENO + 1, &readable, nullptr, nullptr, nullptr, &original);
        if (ready < 0) {
            if (errno == EINTR) continue;  // the Ctrl+C handler ran; the flag is looked at above
            return line.empty() ? ReadStatus::Eof : ReadStatus::Line;
        }
        char c = 0;
        const auto got = ::read(STDIN_FILENO, &c, 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            return line.empty() ? ReadStatus::Eof : ReadStatus::Line;
        }
        if (got == 0) return line.empty() ? ReadStatus::Eof : ReadStatus::Line;
        if (c == '\n') return ReadStatus::Line;
        line.push_back(c);
    }
#endif
}

int readFileBytes(const std::string& path, std::string& content) {
    content.clear();
#ifdef _WIN32
    HANDLE file = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // CPython's winerror -> errno table (PC/errmap.h); a directory fails here with ERROR_ACCESS_DENIED.
        switch (GetLastError()) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_INVALID_DRIVE:
            case ERROR_BAD_PATHNAME:
            case ERROR_FILENAME_EXCED_RANGE:
            case ERROR_BAD_NETPATH:
            case ERROR_BAD_NET_NAME:
                return 2;
            case ERROR_ACCESS_DENIED:
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
            case ERROR_NETWORK_ACCESS_DENIED:
            case ERROR_WRITE_PROTECT:
                return 13;
            case ERROR_TOO_MANY_OPEN_FILES:
                return 24;
            case ERROR_DIRECTORY:
                return 20;
            default:
                return 22;
        }
    }
    char buffer[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buffer, sizeof buffer, &got, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            CloseHandle(file);
            return 5;
        }
        if (got == 0) break;
        content.append(buffer, got);
    }
    CloseHandle(file);
    return 0;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return errno;
    struct stat info {};
    if (::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) {
        ::close(fd);
        return 21;
    }
    char buffer[65536];
    for (;;) {
        const auto got = ::read(fd, buffer, sizeof buffer);
        if (got < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return 5;
        }
        if (got == 0) break;
        content.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fd);
    return 0;
#endif
}

void setStdinBinary() {
#ifdef _WIN32
    if (!isConsole(STD_INPUT_HANDLE)) _setmode(_fileno(stdin), _O_BINARY);
#endif
}

}  // namespace meradb::sys
