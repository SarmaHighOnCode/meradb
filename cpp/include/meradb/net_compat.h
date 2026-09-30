// cpp/include/meradb/net_compat.h
//
// The only place that knows about Winsock vs BSD sockets. IPv4 TCP, blocking
// I/O, RAII. Public header: no <winsock2.h> / <sys/socket.h> here, the
// native descriptor is carried as a std::intptr_t.
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace meradb::net {

// what() is the operating system's error text (or "timed out").
class NetError : public std::runtime_error {
public:
    explicit NetError(const std::string& message) : std::runtime_error(message) {}
};

// A move-only owner of one TCP socket (a connection or a listener).
class Socket {
public:
    Socket() = default;
    explicit Socket(std::intptr_t handle) : handle_(handle) {}
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalid; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = kInvalid;
        }
        return *this;
    }

    bool valid() const { return handle_ != kInvalid; }
    std::intptr_t handle() const { return handle_; }

    // Closes the descriptor. Never call it while another thread is using this
    // socket: stop and join that thread first.
    void close();
    // Half-closes both directions so the PEER sees end-of-file. Never throws.
    // (It does NOT reliably wake a local thread blocked in recvSome -- on
    // Windows a blocked recv ignores it. Threads that must be stoppable poll
    // with waitReadable() instead.)
    void shutdownBoth();
    // Waits up to `seconds` until a recvSome() would not block (data arrived
    // or the peer closed). Returns false on timeout. Throws NetError.
    bool waitReadable(double seconds);

    // Sends every byte (looping over partial sends). Throws NetError.
    void sendAll(const std::string& data);
    // Reads at most `capacity` bytes. Returns 0 when the peer closed the
    // connection. Throws NetError on an error or when the receive timeout
    // (see setReceiveTimeout) expires ("timed out").
    std::size_t recvSome(char* buffer, std::size_t capacity);
    // 0 = wait forever.
    void setReceiveTimeout(double seconds);

    std::string peerAddress() const;  // "127.0.0.1", or "?" if unknown
    int peerPort() const;             // 0 if unknown
    int localPort() const;            // 0 if unknown

    static constexpr std::intptr_t kInvalid = -1;

private:
    std::intptr_t handle_ = kInvalid;
};

// Connects to host:port (IPv4; `host` may be a name). Throws NetError
// ("timed out", "Connection refused", ...).
Socket connectTo(const std::string& host, int port, double timeoutSeconds);

// Binds and listens. An empty host means every interface; port 0 lets the OS
// choose (read it back with Socket::localPort). Windows uses
// SO_EXCLUSIVEADDRUSE (a second server can never share the port), POSIX uses
// SO_REUSEADDR. Throws NetError.
Socket listenOn(const std::string& host, int port, int backlog = 64);

// Waits up to `timeoutSeconds` for a connection; returns an invalid Socket on
// timeout. The accepted socket has TCP_NODELAY set. Throws NetError.
Socket acceptWithTimeout(Socket& listener, double timeoutSeconds);

// "Is something listening?" -- a real connect attempt, like protocol.port_open.
bool portOpen(const std::string& host, int port, double timeoutSeconds = 0.5);

}  // namespace meradb::net
