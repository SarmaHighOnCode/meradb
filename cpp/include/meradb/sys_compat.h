// cpp/include/meradb/sys_compat.h
//
// The only place that knows about OS differences for the small things the
// server and CLI need: environment, pid, wall clock, randomness. Public
// header: no <windows.h> here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace meradb::sys {

// nullopt when the variable is unset (an empty value is returned as "").
std::optional<std::string> getEnv(const std::string& name);

std::int64_t processId();

// Local time, like datetime.now().isoformat(timespec="seconds"): 2026-09-29T14:03:07
std::string localIsoSeconds();
// Local time, like f"{datetime.now():%Y-%m-%d %H:%M:%S}": 2026-09-29 14:03:07
std::string localLogStamp();

// Bytes from the operating system's CSPRNG (BCryptGenRandom / /dev/urandom).
// Throws StorageError if the OS refuses. Never uses std::random_device, which
// is deterministic on some MinGW builds.
std::vector<std::uint8_t> randomBytes(std::size_t count);

// os.path.expanduser("~"): on Windows %USERPROFILE%, else %HOMEDRIVE%%HOMEPATH%, else "~";
// elsewhere $HOME, "." if unknown.
std::string homeDir();

// Overwrites the variable for this process (and the children it starts). An empty
// value removes it on Windows and leaves it empty elsewhere; getEnv callers treat both alike.
void setEnv(const std::string& name, const std::string& value);

// The command-line arguments after the program name, as UTF-8. On Windows they come from the
// wide command line (the narrow argv is in the ANSI code page and loses non-ASCII text);
// elsewhere argv is used as it is.
std::vector<std::string> commandLineArgs(int argc, char** argv);

// While alive, on Windows, a console used for output (stdout or stderr) gets the UTF-8 output code
// page, and a console used for input (stdin) the UTF-8 input code page, so UTF-8 text prints
// correctly and typed non-ASCII text arrives as UTF-8 (Python reaches the same result with its own
// console handling). Redirected or piped streams are not touched; the previous code pages are put
// back at the end. Does nothing on other platforms.
class Utf8Console {
public:
    Utf8Console();
    ~Utf8Console();
    Utf8Console(const Utf8Console&) = delete;
    Utf8Console& operator=(const Utf8Console&) = delete;

private:
    unsigned savedOutput_ = 0;  // 0 = left alone
    unsigned savedInput_ = 0;
};

// Reads one line from the terminal WITHOUT echoing it (a password prompt, like
// getpass). The prompt goes to stderr. When stdin is not a terminal the line is
// simply read.
std::string readHidden(const std::string& prompt);

// UTF-8 path of the running executable ("" if the OS will not say).
std::string executablePath();

// A child process started so that it outlives this one (no terminal, no shared
// stdio). Only ever created by spawnDetached().
class DetachedProcess {
public:
    struct Impl;
    explicit DetachedProcess(std::unique_ptr<Impl> impl);
    ~DetachedProcess();
    DetachedProcess(const DetachedProcess&) = delete;
    DetachedProcess& operator=(const DetachedProcess&) = delete;

    std::int64_t pid() const;
    bool exited();  // true once the process has ended; never blocks

private:
    std::unique_ptr<Impl> impl_;
};

// Starts `exePath args...` detached from this process and terminal: stdin is
// the null device, stdout and stderr are APPENDED to `logPath`. Throws
// StorageError if the process cannot be started.
std::unique_ptr<DetachedProcess> spawnDetached(const std::string& exePath, const std::vector<std::string>& args,
                                               const std::string& logPath);

// Asks the OS to end a process (SIGTERM; TerminateProcess on Windows).
// Returns "" on success, otherwise the OS error text.
std::string killProcess(std::int64_t pid);

}  // namespace meradb::sys
