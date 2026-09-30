// cpp/include/meradb/server.h
//
// The MeraDB SERVER: many clients, one database (mirrors meradb/server.py).
//
//      shell / workbench / app ── TCP :6372 ── Server ─┬─ Engine (session 1) ─┐
//         (newline-delimited JSON)                     ├─ Engine (session 2) ─┼─ Instance ── data/
//                                                      └─ Engine (session 3) ─┘  (one lock, one cache)
//
//   * One std::thread per connection. Each connection owns its OWN Engine (its
//     current database and transaction), created, used and destroyed on that
//     thread -- which is exactly what Engine's "SHURU thread rule" needs. All
//     sessions share ONE Instance, hence one lock and one index cache.
//   * A client that disconnects in the middle of a transaction is rolled back.
//   * Threads never block for ever in recv(): they poll a stop flag every
//     100 ms, so requestStop() (also callable from a signal handler) ends the
//     server promptly, and every session rolls back its own open transaction.
#pragma once
#include "meradb/engine.h"
#include "meradb/net_compat.h"
#include "meradb/protocol.h"
#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace meradb {

struct ServerOptions {
    std::string dataDir;
    std::string host = protocol::kDefaultHost;
    int port = protocol::kDefaultPort;  // 0 = let the OS choose (tests); read it back with Server::port()
    std::string password;               // empty = anyone may connect (the shared password)
    bool verbose = false;               // log every query
    // Where log lines go (already stamped "YYYY-MM-DD HH:MM:SS  message"). Default: stdout.
    std::function<void(const std::string&)> log;
    std::size_t maxMessageBytes = protocol::kMaxMessageBytes;  // tests lower it
};

class Server {
public:
    // Opens the data folder as the serving process and binds the port.
    // Throws MeraDBError (folder problems) or net::NetError (port busy, ...).
    explicit Server(ServerOptions options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    int port() const { return port_; }
    int sessions() const { return sessions_.load(); }
    Instance& instance() { return *instance_; }
    const std::string& started() const { return started_; }
    const ServerOptions& options() const { return options_; }
    void log(const std::string& message);

    // Accept loop; returns after requestStop() (or a client's shutdown request),
    // once every connection thread has finished.
    void serveForever();
    // Async-signal-safe: only sets a flag.
    void requestStop() { stop_ = true; }

private:
    struct Connection {
        net::Socket socket;
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void serveConnection(Connection& connection);
    protocol::Json dispatch(Engine& session, const protocol::Json& request, const std::string& peerIp,
                            const std::string& peer, bool& stopConnection);
    void reap(bool everything);

    ServerOptions options_;
    std::shared_ptr<Instance> instance_;
    net::Socket listener_;
    int port_ = 0;
    std::string started_;
    std::atomic<bool> stop_{false};
    std::atomic<int> sessions_{0};
    std::mutex logMutex_;
    std::list<std::unique_ptr<Connection>> connections_;  // touched only by the serveForever thread
};

// `meradb server`: run in the foreground until Ctrl+C or `meradb stop`.
// Writes/removes meradb.pid, logs like Python's serve(). Returns the exit code.
int serve(ServerOptions options);

}  // namespace meradb
