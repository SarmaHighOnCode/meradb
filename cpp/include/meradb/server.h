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
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
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
    // Limits on a peer that has not logged in. Every connection costs a thread, so these keep idle or slow strangers
    // from using them all up.
    std::size_t maxHelloBytes = protocol::kMaxHelloBytes;
    double handshakeTimeoutSeconds = 10.0;
    int maxConnections = 256;  // beyond this a new connection is told so and closed
    int maxConnectionsPerAddress = 64;  // so one address cannot take all of them; 0 = no limit
    // A session that sends no query or schema request for this long is told so and closed (a ping does not count, or
    // it would keep a session alive for ever). 0 = no limit. A workbench may legitimately sit open for hours, hence
    // the long default, which is MySQL's own wait_timeout.
    double idleTimeoutSeconds = 8 * 3600.0;
    // The same for a session with an open transaction: it holds the one lock every other session waits for, so it gets
    // far less. The connection is closed, not just the transaction rolled back: a client that thinks it is still inside
    // a transaction must not go on outside one without noticing.
    double idleTransactionTimeoutSeconds = 300.0;
    // A client that takes no bytes of a reply for this long is dropped (it may be holding the lock). 0 = no limit.
    double sendStallSeconds = 120.0;
    // Password hashing is the one thing a stranger can make the server do at will: this many logins hash at once, the
    // others wait up to loginHashWaitSeconds and are then told the server is busy. 0 = no limit.
    int maxConcurrentLogins = 4;
    double loginHashWaitSeconds = 5.0;
    // After this many wrong passwords in a row from one address, it gets the same "wrong password" answer for
    // loginLockSeconds whatever it sends (and costs no hashing). 0 = no limit.
    int loginFailureLimit = 10;
    double loginLockSeconds = 30.0;
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
    int peakHashingLogins() const { return peakHashing_.load(); }  // the most logins that ever hashed at the same time
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
        std::string peerIp;
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void serveConnection(Connection& connection);
    protocol::Json dispatch(Engine& session, const protocol::Json& request, const std::string& peerIp,
                            const std::string& peer, bool& stopConnection);
    void reap(bool everything);
    bool loginLocked(const std::string& ip);
    void noteLogin(const std::string& ip, bool succeeded);
    bool beginHashing();  // false: the server stayed too busy for too long
    void endHashing();

    struct LoginFailures {
        int count = 0;
        std::chrono::steady_clock::time_point lockedUntil;
    };
    std::mutex hashingMutex_;
    std::condition_variable hashingFree_;
    int hashing_ = 0;
    std::atomic<int> peakHashing_{0};
    std::mutex loginMutex_;
    std::map<std::string, LoginFailures> loginFailures_;

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

// Reads the MERADB_* limit settings (docs/SERVER.md) into `options`. Returns an error text for a value that is not a
// number >= 0, or "".
std::string applyLimitEnvironment(ServerOptions& options);

// `meradb server`: run in the foreground until Ctrl+C or `meradb stop`.
// Writes/removes meradb.pid, logs like Python's serve(). Returns the exit code.
int serve(ServerOptions options);

}  // namespace meradb
