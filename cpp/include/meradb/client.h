// cpp/include/meradb/client.h
//
// The MeraDB CLIENT library: talk to a running server from C++ (mirrors
// meradb/client.py).
//
//     meradb::ConnectOptions options;      // 127.0.0.1:6372 by default
//     meradb::Connection db(options);
//     for (const auto& result : db.runScript("DIKHAO * SE students;")) ...
//
// A Connection is a Backend, so the shell / workbench / `meradb run` use it
// exactly like an embedded engine.
#pragma once
#include "meradb/backend.h"
#include "meradb/net_compat.h"
#include "meradb/protocol.h"
#include <memory>
#include <optional>
#include <string>

namespace meradb {

struct ConnectOptions {
    std::string host = protocol::kDefaultHost;
    int port = protocol::kDefaultPort;
    std::optional<std::string> password;  // the shared server password
    std::optional<std::string> database;  // start in this database
    std::optional<std::string> user;      // log in as this user (then `password` is THEIR password)
    double connectTimeoutSeconds = 5.0;
    // Queries may legitimately wait (up to ~10 s) for another client's transaction.
    double timeoutSeconds = 60.0;
};

class Connection : public Backend {
public:
    // Throws ServerUnavailable (nobody listening) or ConnectionFailed (refused, e.g. wrong password).
    explicit Connection(const ConnectOptions& options);
    ~Connection() override { close(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    std::vector<Result> runScript(const std::string& text) override;
    // Python quirk kept on purpose: the server's error text already carries its
    // "[Stage Galti]" tag, and is wrapped once more as a plain MeraDBError.
    std::vector<Result> execute(const std::string& text) override;
    std::string currentDb() override { return currentDb_; }
    bool inTransaction() override { return inTransaction_; }
    nlohmann::ordered_json schemaTree() override;
    std::string description() override { return options_.host + ":" + std::to_string(options_.port); }
    void close() override { socket_.close(); }

    const std::string& serverVersion() const { return serverVersion_; }
    protocol::Json status();  // the server's raw status reply
    void shutdown();          // ask the server to stop (loopback only)

private:
    protocol::Json request(const protocol::Json& message);

    ConnectOptions options_;
    net::Socket socket_;
    std::unique_ptr<protocol::MessageReader> reader_;
    std::string currentDb_ = DEFAULT_DATABASE;
    bool inTransaction_ = false;
    std::string serverVersion_ = "MeraDB";
};

}  // namespace meradb
