// cpp/src/net_compat.cpp
#include "meradb/net_compat.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace meradb::net {

namespace {

#ifdef _WIN32
using Native = SOCKET;
using SockLen = int;
using IoLen = int;
using IoCap = int;
constexpr Native kBadNative = INVALID_SOCKET;
int lastError() { return WSAGetLastError(); }
void closeNative(Native s) { ::closesocket(s); }
bool isTimeout(int e) { return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK; }
bool isInterrupted(int e) { return e == WSAEINTR; }
#else
using Native = int;
using SockLen = socklen_t;
using IoLen = ssize_t;
using IoCap = std::size_t;
constexpr Native kBadNative = -1;
int lastError() { return errno; }
void closeNative(Native s) { ::close(s); }
bool isTimeout(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
bool isInterrupted(int e) { return e == EINTR; }
#endif

Native toNative(std::intptr_t h) { return static_cast<Native>(h); }

// The operating system's own wording ("Connection refused", ...).
std::string errorText(int code) { return std::system_category().message(code); }

// One-time process setup: Winsock on Windows; on POSIX a peer that vanished
// must produce an error from send(), not kill the process with SIGPIPE.
void ensureInit() {
    static const bool done = [] {
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw NetError("Winsock start nahi hua");
#else
        std::signal(SIGPIPE, SIG_IGN);
#endif
        return true;
    }();
    (void)done;
}

sockaddr_in resolveV4(const std::string& host, int port, bool passive) {
    if (port < 0 || port > 65535) throw NetError("port 0-65535 ke beech hona chahiye");  // Python: "port must be 0-65535."
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (passive) hints.ai_flags = AI_PASSIVE;
    addrinfo* found = nullptr;
    const std::string portText = std::to_string(port);
    const char* node = host.empty() ? nullptr : host.c_str();
    int rc = ::getaddrinfo(node, portText.c_str(), &hints, &found);
    if (rc != 0 || found == nullptr) throw NetError("address nahi mila: " + host);
    sockaddr_in out{};
    std::memcpy(&out, found->ai_addr, sizeof out);
    ::freeaddrinfo(found);
    return out;
}

void setNonBlocking(Native s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    ::ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

// A child process must not inherit sockets (no-op on Windows, where handles are not inherited by default).
void setCloseOnExec(Native s) {
#ifndef _WIN32
    int flags = ::fcntl(s, F_GETFD, 0);
    if (flags >= 0) ::fcntl(s, F_SETFD, flags | FD_CLOEXEC);
#else
    (void)s;
#endif
}

void setNoDelay(Native s) {
    int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

timeval toTimeval(double seconds) {
    timeval tv{};
    double whole = std::floor(seconds);
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(whole);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((seconds - whole) * 1e6);
    return tv;
}

// Waits until the socket is readable (or, with `forWrite`, writable) or the
// timeout passes. A failed connect shows up as an error/hangup condition and
// counts as "ready" (the caller then reads SO_ERROR). A signal (EINTR) restarts
// the wait with the time that is left. POSIX uses poll(), so a descriptor
// number above FD_SETSIZE is fine; Windows uses select().
bool waitReady(Native s, bool forWrite, double timeoutSeconds) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
                                             std::chrono::duration<double>(timeoutSeconds < 0 ? 0 : timeoutSeconds));
    for (;;) {
        double left = std::chrono::duration<double>(deadline - clock::now()).count();
        if (left < 0) left = 0;
#ifdef _WIN32
        fd_set set;
        fd_set except;
        FD_ZERO(&set);
        FD_ZERO(&except);
        FD_SET(s, &set);
        FD_SET(s, &except);
        timeval tv = toTimeval(left);
        int rc = ::select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, &except, &tv);
#else
        pollfd pfd{};
        pfd.fd = s;
        pfd.events = forWrite ? POLLOUT : POLLIN;
        int rc = ::poll(&pfd, 1, static_cast<int>(std::ceil(left * 1000.0)));
#endif
        if (rc < 0) {
            if (isInterrupted(lastError())) continue;
            throw NetError(errorText(lastError()));
        }
        return rc > 0;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Socket
// ---------------------------------------------------------------------------

void Socket::close() {
    if (handle_ == kInvalid) return;
    closeNative(toNative(handle_));
    handle_ = kInvalid;
}

void Socket::shutdownBoth() {
    if (handle_ == kInvalid) return;
#ifdef _WIN32
    ::shutdown(toNative(handle_), SD_BOTH);
#else
    ::shutdown(toNative(handle_), SHUT_RDWR);
#endif
}

bool Socket::waitReadable(double seconds) {
    if (handle_ == kInvalid) throw NetError("socket band hai");
    return waitReady(toNative(handle_), false, seconds);
}

void Socket::sendAll(const std::string& data, const std::atomic<bool>* stop) {
    if (handle_ == kInvalid) throw NetError("socket band hai");
    std::size_t sent = 0;
    if (stop != nullptr) {
        struct NonBlocking {  // blocking mode is restored on every exit path
            Native s;
            explicit NonBlocking(Native native) : s(native) { setNonBlocking(s, true); }
            ~NonBlocking() { setNonBlocking(s, false); }
        } mode(toNative(handle_));
        while (sent < data.size()) {
            std::size_t chunk = std::min<std::size_t>(data.size() - sent, 1u << 16);
            IoLen n = ::send(toNative(handle_), data.data() + sent, static_cast<IoCap>(chunk), 0);
            if (n >= 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            int e = lastError();
            if (isInterrupted(e)) continue;
            if (!isTimeout(e)) throw NetError(errorText(e));  // timeout here = would block: no room yet
            if (stop->load()) throw NetError("server band ho raha hai");  // only a blocked send gives up
            waitReady(toNative(handle_), true, 0.1);
        }
        return;
    }
    while (sent < data.size()) {
        std::size_t chunk = data.size() - sent;
        if (chunk > 1u << 20) chunk = 1u << 20;
        IoLen n = ::send(toNative(handle_), data.data() + sent, static_cast<IoCap>(chunk), 0);
        if (n < 0) {
            int e = lastError();
            if (isInterrupted(e)) continue;
            throw NetError(errorText(e));
        }
        sent += static_cast<std::size_t>(n);
    }
}

std::size_t Socket::recvSome(char* buffer, std::size_t capacity) {
    if (handle_ == kInvalid) throw NetError("socket band hai");
    for (;;) {
        IoLen n = ::recv(toNative(handle_), buffer, static_cast<IoCap>(capacity), 0);
        if (n >= 0) return static_cast<std::size_t>(n);
        int e = lastError();
        if (isInterrupted(e)) continue;
        if (isTimeout(e)) throw NetError("timed out");
        throw NetError(errorText(e));
    }
}

void Socket::setReceiveTimeout(double seconds) {
    if (handle_ == kInvalid) return;
#ifdef _WIN32
    DWORD ms = seconds <= 0 ? 0 : static_cast<DWORD>(std::max(1.0, seconds * 1000.0));  // 0 would mean "wait forever"
    ::setsockopt(toNative(handle_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
    timeval tv = toTimeval(seconds <= 0 ? 0 : seconds);
    ::setsockopt(toNative(handle_), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

std::string Socket::peerAddress() const {
    if (handle_ == kInvalid) return "?";
    sockaddr_in addr{};
    SockLen len = sizeof addr;
    if (::getpeername(toNative(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "?";
    char text[64] = {0};
    if (::inet_ntop(AF_INET, &addr.sin_addr, text, sizeof text) == nullptr) return "?";
    return text;
}

int Socket::peerPort() const {
    if (handle_ == kInvalid) return 0;
    sockaddr_in addr{};
    SockLen len = sizeof addr;
    if (::getpeername(toNative(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

int Socket::localPort() const {
    if (handle_ == kInvalid) return 0;
    sockaddr_in addr{};
    SockLen len = sizeof addr;
    if (::getsockname(toNative(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

// ---------------------------------------------------------------------------
// free functions
// ---------------------------------------------------------------------------

Socket connectTo(const std::string& host, int port, double timeoutSeconds) {
    ensureInit();
    sockaddr_in addr = resolveV4(host, port, false);
    Native s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadNative) throw NetError(errorText(lastError()));
    Socket sock(static_cast<std::intptr_t>(s));  // owned (and closed on any throw) from here on
    setCloseOnExec(s);

    setNonBlocking(s, true);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        int e = lastError();
#ifdef _WIN32
        bool inProgress = (e == WSAEWOULDBLOCK);
#else
        bool inProgress = (e == EINPROGRESS || e == EINTR);
#endif
        if (!inProgress) throw NetError(errorText(e));
        if (!waitReady(s, true, timeoutSeconds)) throw NetError("timed out");
        int soError = 0;
        SockLen len = sizeof soError;
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
        if (soError != 0) throw NetError(errorText(soError));
    }
    setNonBlocking(s, false);
    setNoDelay(s);
    return sock;
}

Socket listenOn(const std::string& host, int port, int backlog) {
    ensureInit();
    sockaddr_in addr = resolveV4(host, port, true);
    Native s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadNative) throw NetError(errorText(lastError()));
    Socket sock(static_cast<std::intptr_t>(s));
    setCloseOnExec(s);

    int one = 1;
#ifdef _WIN32
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) throw NetError(errorText(lastError()));
    if (::listen(s, backlog) != 0) throw NetError(errorText(lastError()));
    // Non-blocking, so accept() after a ready select/poll can never hang if the
    // pending connection was aborted in between (acceptWithTimeout treats
    // EWOULDBLOCK as "nothing yet").
    setNonBlocking(s, true);
    return sock;
}

Socket acceptWithTimeout(Socket& listener, double timeoutSeconds) {
    if (!listener.valid()) throw NetError("listener band hai");
    Native l = toNative(listener.handle());
    if (!waitReady(l, false, timeoutSeconds)) return Socket();
    Native c = ::accept(l, nullptr, nullptr);
    if (c == kBadNative) {
        int e = lastError();
        if (isInterrupted(e) || isTimeout(e)) return Socket();
#ifndef _WIN32
        if (e == ECONNABORTED) return Socket();
#endif
        throw NetError(errorText(e));
    }
    Socket accepted(static_cast<std::intptr_t>(c));
    setNonBlocking(c, false);  // Windows hands the listener's non-blocking mode on to accepted sockets
    setCloseOnExec(c);
    setNoDelay(c);
    return accepted;
}

bool portOpen(const std::string& host, int port, double timeoutSeconds) {
    try {
        Socket s = connectTo(host, port, timeoutSeconds);
        return s.valid();
    } catch (const NetError&) {
        return false;
    }
}

}  // namespace meradb::net
