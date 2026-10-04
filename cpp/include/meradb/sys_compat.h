// cpp/include/meradb/sys_compat.h
//
// The only place that knows about OS differences for the small things the
// server and CLI need: environment, pid, wall clock, randomness. Public
// header: no <windows.h> here.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
    [[maybe_unused]] unsigned savedOutput_ = 0;  // 0 = left alone (Windows only)
    [[maybe_unused]] unsigned savedInput_ = 0;
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

// ---------------------------------------------------------------------------
// The interactive terminal (used by the shell)
// ---------------------------------------------------------------------------

// True when standard stream `fd` (0 = stdin, 1 = stdout, 2 = stderr) is attached to a terminal (a console
// on Windows). Anything else, including a pipe or a file, is false.
bool isTerminal(int fd);

// Turns on ANSI escape-code processing for the console that stdout is attached to, on the classic Windows
// console host (ENABLE_VIRTUAL_TERMINAL_PROCESSING), and puts the old console mode back in the destructor.
// Does nothing elsewhere.
class AnsiConsole {
public:
    AnsiConsole();
    ~AnsiConsole();
    AnsiConsole(const AnsiConsole&) = delete;
    AnsiConsole& operator=(const AnsiConsole&) = delete;

    // True when escape codes will be understood after this call. False when stdout is not a console,
    // or the console refuses. Always true on non-Windows platforms.
    bool enable();

private:
    [[maybe_unused]] unsigned savedMode_ = 0;  // Windows only
    [[maybe_unused]] bool changed_ = false;
};

// While a guard exists, Ctrl+C does not end the process: it only sets a flag that the shell reads (and,
// on Windows, cancels a console read that is waiting for a line). One guard at a time.
class InterruptGuard {
public:
    InterruptGuard();
    ~InterruptGuard();
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;

    // True once per Ctrl+C: reads the flag and clears it.
    static bool consume();
    // Does what the Ctrl+C handler does. For tests, which cannot press the key.
    static void trigger();
    // True while a Ctrl+C is pending, without clearing it.
    static bool pending();
};

// While alive, the terminal's own key handling is switched off so the full-screen workbench receives these keys as
// ordinary input: POSIX ISIG (Ctrl+C, Ctrl+Z, Ctrl+\), IXON (Ctrl+S / Ctrl+Q flow control) and IEXTEN (Ctrl+V, Ctrl+O);
// Windows ENABLE_PROCESSED_INPUT (Ctrl+C). Does nothing when stdin is not a terminal. The destructor puts the old
// mode back. Create it BEFORE the screen's Loop(): FTXUI saves the terminal state when the loop starts and restores
// it when the loop ends, so ours is the outer layer and is restored last.
class TerminalModeGuard {
public:
    TerminalModeGuard();
    ~TerminalModeGuard();
    TerminalModeGuard(const TerminalModeGuard&) = delete;
    TerminalModeGuard& operator=(const TerminalModeGuard&) = delete;
    bool active() const;   // false when stdin is not a terminal or the mode could not be changed
private:
    struct State;
    std::unique_ptr<State> state_;
};

// The hand-over between a thread that reads a console line and the Ctrl+C handler thread (used on Windows,
// portable so it can be unit tested). The handler may cancel the reader's I/O ONLY while the reader is
// inside the read: leaveRead() takes the same lock the handler holds while it cancels, so once leaveRead()
// has returned, no cancel can still be issued or in flight against whatever the thread does next
// (a socket write, a file write). A Ctrl+C that lands before enterRead() is seen by enterRead().
class InterruptGate {
public:
    // Reader, immediately before the blocking read. False: an interrupt is already pending, do not read.
    bool enterRead(const std::atomic<bool>& interruptPending);
    // Reader, immediately after the blocking read returned (whatever the outcome).
    void leaveRead();
    // Handler, after it has set the pending flag. Runs `cancel` (under the lock) and returns true only while
    // the reader is inside the read; false when it is not (nothing is cancelled).
    bool cancelIfReading(const std::function<void()>& cancel);

private:
    std::mutex mutex_;
    bool inRead_ = false;
};

// Whether a shell loop that has already seen Ctrl+C may still read another line (it may not).
inline bool mayReadAnotherLine(bool interruptPending) { return !interruptPending; }

enum class ReadStatus { Line, Eof, Interrupted, Failed };

// Reads one line from the terminal (stdin MUST be one; see isTerminal) as UTF-8 without its line
// terminator. Eof: Ctrl+D (Ctrl+Z then Enter on Windows) or a closed input. Interrupted: Ctrl+C, which
// needs an InterruptGuard; the flag is cleared (consumed) by this return. Failed (POSIX only): the read itself
// failed (for example EIO on a vanished terminal); a one-line "OSError: [Errno N] ..." was written to stderr,
// where Python would have died with a traceback.
ReadStatus readTerminalLine(std::string& line);

// When stdin is NOT a terminal, Windows would translate CRLF and stop at Ctrl+Z (text mode); this puts
// stdin into binary mode so the bytes arrive untouched. Does nothing elsewhere.
void setStdinBinary();

// Reads a whole file as bytes (UTF-8 path). Returns 0 on success, otherwise the errno number that Python's
// open() would report for the same failure: 2 (missing), 13 (permission; on Windows also a directory),
// 21 (a directory, elsewhere), 22 (a name the system rejects), 20, 24, ... or 5 for a read error.
int readFileBytes(const std::string& path, std::string& content);

}  // namespace meradb::sys
