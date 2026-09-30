// cpp/tests/server_fixture.h -- a real server on a free port, plus a bare-bones protocol client.
#pragma once
#include "meradb/protocol.h"
#include "meradb/server.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace meradb_test {

inline void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Polls `condition` for up to `seconds`; true if it became true.
template <typename F>
bool waitFor(F condition, double seconds = 3.0) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        sleepMs(10);
    }
    return condition();
}

// A server bound to 127.0.0.1 on an OS-chosen port, serving a temp data folder.
// Statements wait at most 0.5 s for another session's transaction (the Python
// tests do the same). The destructor stops it and joins everything.
class RunningServer {
public:
    explicit RunningServer(const std::string& password = "", bool verbose = false,
                           std::size_t maxMessageBytes = meradb::protocol::kMaxMessageBytes) {
        meradb::ServerOptions options;
        options.dataDir = dir_.str();
        options.port = 0;
        options.password = password;
        options.verbose = verbose;
        options.maxMessageBytes = maxMessageBytes;
        options.log = [this](const std::string& line) {
            std::lock_guard<std::mutex> guard(logMutex_);
            lines_.push_back(line);
        };
        server_ = std::make_unique<meradb::Server>(options);
        server_->instance().lockTimeoutSeconds = 0.5;
        thread_ = std::thread([this] {
            server_->serveForever();
            stopped_ = true;
        });
    }
    ~RunningServer() { stop(); }
    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    void stop() {
        server_->requestStop();
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return server_->port(); }
    meradb::Server& server() { return *server_; }
    meradb::Instance& instance() { return server_->instance(); }
    const std::string& dataDir() const { return server_->instance().dataDir(); }
    bool stopped() const { return stopped_.load(); }
    std::vector<std::string> logLines() {
        std::lock_guard<std::mutex> guard(logMutex_);
        return lines_;
    }
    bool logContains(const std::string& text) {
        for (const auto& line : logLines())
            if (line.find(text) != std::string::npos) return true;
        return false;
    }

private:
    TempDir dir_;
    std::mutex logMutex_;
    std::vector<std::string> lines_;
    std::unique_ptr<meradb::Server> server_;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
};

// The protocol by hand, with no client library: send whatever you like, read
// back exactly what the server sent.
class RawClient {
public:
    explicit RawClient(int port, double timeoutSeconds = 5.0)
        : socket_(meradb::net::connectTo("127.0.0.1", port, 2.0)), reader_(socket_) {
        socket_.setReceiveTimeout(timeoutSeconds);
    }
    void send(const meradb::protocol::Json& message) { meradb::protocol::send(socket_, message); }
    void sendRaw(const std::string& bytes) { socket_.sendAll(bytes); }
    std::optional<meradb::protocol::Json> receive() { return reader_.receive(); }
    meradb::protocol::Json request(const meradb::protocol::Json& message) {
        send(message);
        return receive().value();
    }
    // The hello of protocol.py's Connection; returns the server's reply.
    meradb::protocol::Json hello(const meradb::protocol::Json& password = nullptr,
                                 const meradb::protocol::Json& database = nullptr,
                                 const meradb::protocol::Json& user = nullptr) {
        meradb::protocol::Json message = meradb::protocol::Json::object();
        message["type"] = "hello";
        message["version"] = 1;
        message["password"] = password;
        message["database"] = database;
        message["user"] = user;
        return request(message);
    }
    meradb::protocol::Json query(const std::string& text) {
        meradb::protocol::Json message = meradb::protocol::Json::object();
        message["type"] = "query";
        message["text"] = text;
        return request(message);
    }
    meradb::net::Socket& socket() { return socket_; }

private:
    meradb::net::Socket socket_;
    meradb::protocol::MessageReader reader_;
};

}  // namespace meradb_test
