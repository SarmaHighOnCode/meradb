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
