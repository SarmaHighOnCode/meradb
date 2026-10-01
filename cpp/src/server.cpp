// cpp/src/server.cpp
#include "meradb/server.h"
#include "meradb/ast.h"
#include "meradb/datatypes.h"
#include "meradb/errors.h"
#include "meradb/sys_compat.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <system_error>

namespace meradb {

using protocol::Json;

namespace {

// Python's LOOPBACK tuple (matched against the peer's textual address).
bool isLoopback(const std::string& ip) { return ip == "127.0.0.1" || ip == "::1" || ip == "localhost"; }

const Json* field(const Json& object, const char* key) {
    auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

// Python truthiness of a JSON value (`if user:` / `x or ""`).
bool pyTruthy(const Json* j) {
    if (j == nullptr || j->is_null()) return false;
    if (j->is_boolean()) return j->get<bool>();
    if (j->is_number_integer()) return j->is_number_unsigned() ? j->get<std::uint64_t>() != 0 : j->get<std::int64_t>() != 0;
    if (j->is_number_float()) return j->get<double>() != 0.0;
    if (j->is_string()) return !j->get_ref<const std::string&>().empty();
    return !j->empty();
}

std::string pyReprJson(const Json& j);

// Python's str() of a JSON value.
std::string pyStr(const Json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_null()) return "None";
    if (j.is_boolean()) return j.get<bool>() ? "True" : "False";
    if (j.is_number_float()) return pyReprFloat(j.get<double>());
    if (j.is_number_unsigned()) return std::to_string(j.get<std::uint64_t>());
    if (j.is_number_integer()) return std::to_string(j.get<std::int64_t>());
    return pyReprJson(j);  // str() of a list or dict is its repr
}

// Python's repr() of a JSON value, for `{kind!r}`: ['a'], {'a': 1}, None, True, 'text'.
std::string pyReprJson(const Json& j) {
    if (j.is_string()) return pyRepr(j.get<std::string>());
    if (j.is_array()) {
        std::string out = "[";
        bool first = true;
        for (const auto& item : j) {
            if (!first) out += ", ";
            first = false;
            out += pyReprJson(item);
        }
        return out + "]";
    }
    if (j.is_object()) {
        std::string out = "{";
        bool first = true;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!first) out += ", ";
            first = false;
            out += pyRepr(it.key()) + ": " + pyReprJson(it.value());
        }
        return out + "}";
    }
    return pyStr(j);
}

// hmac.compare_digest: the running time does not depend on where the strings differ.
bool constantTimeEquals(const std::string& a, const std::string& b) {
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

// ' '.join(text.split())[:limit]  (limit counts characters, not bytes)
std::string collapseWhitespace(const std::string& text, std::size_t limit) {
    std::string out;
    bool pendingSpace = false;
    std::size_t chars = 0;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
            pendingSpace = !out.empty();
            continue;
        }
        const bool startsCharacter = (static_cast<unsigned char>(c) & 0xC0) != 0x80;
        if (pendingSpace) {
            if (chars >= limit) break;
            out += ' ';
            ++chars;
            pendingSpace = false;
        }
        if (startsCharacter) {
            if (chars >= limit) break;
            ++chars;
        }
        out += c;
    }
    return out;
}

Json failure(const std::string& message) {
    Json reply = Json::object();
    reply["ok"] = false;
    reply["error"] = message;
    return reply;
}

Json success() {
    Json reply = Json::object();
    reply["ok"] = true;
    return reply;
}

}  // namespace

// ============================================================================
// Server
// ============================================================================

Server::Server(ServerOptions options) : options_(std::move(options)) {
    if (!options_.log) options_.log = [](const std::string& line) { std::cout << line << std::endl; };
    instance_ = std::make_shared<Instance>(options_.dataDir, /*served=*/true);
    started_ = sys::localIsoSeconds();
    listener_ = net::listenOn(options_.host, options_.port);
    port_ = listener_.localPort();
}

Server::~Server() {
    stop_ = true;
    reap(true);  // only non-empty if serveForever() was never allowed to finish
}

void Server::log(const std::string& message) {
    std::lock_guard<std::mutex> guard(logMutex_);
    options_.log(sys::localLogStamp() + "  " + message);
}

void Server::reap(bool everything) {
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (everything || (*it)->finished.load()) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void Server::serveForever() {
    // A persistent accept error (out of descriptors, ...) must not flood the log or spin: the same
    // message is logged at most every 10 s, and the pause between attempts grows from 50 ms to 1 s.
    using clock = std::chrono::steady_clock;
    int failures = 0;
    std::string lastFailure;
    clock::time_point lastLogged;
    while (!stop_) {
        net::Socket accepted;
        try {
            accepted = net::acceptWithTimeout(listener_, 0.1);
            failures = 0;
        } catch (const net::NetError& e) {
            ++failures;
            const auto now = clock::now();
            if (failures == 1 || lastFailure != e.what() || now - lastLogged >= std::chrono::seconds(10)) {
                log(std::string("accept fail: ") + e.what());
                lastFailure = e.what();
                lastLogged = now;
            }
            const int pauseMs = std::min(1000, 50 * failures);
            for (int waited = 0; waited < pauseMs && !stop_; waited += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        reap(false);
        if (!accepted.valid()) continue;

        auto connection = std::make_unique<Connection>();
        connection->socket = std::move(accepted);
        Connection* raw = connection.get();
        try {
            raw->thread = std::thread([this, raw] {
                serveConnection(*raw);
                raw->finished = true;
            });
        } catch (const std::system_error& e) {
            log(std::string("connection thread start nahi hua: ") + e.what());
            continue;  // `connection` (and its socket) is dropped
        }
        connections_.push_back(std::move(connection));
    }
    listener_.close();
    reap(true);  // stop_ is set: readers give up within 100 ms; each thread rolls back its own transaction
}

// Runs on the connection's own thread from the first byte to the last: the
// Engine (and any transaction it opens) never changes threads.
void Server::serveConnection(Connection& connection) {
    net::Socket& socket = connection.socket;
    const std::string peerIp = socket.peerAddress();
    const std::string peer = peerIp + ":" + std::to_string(socket.peerPort());
    try {
        protocol::MessageReader reader(socket, options_.maxMessageBytes, &stop_);

        // ---- 1. handshake ----
        std::optional<Json> hello;
        try {
            hello = reader.receive();
        } catch (const protocol::ProtocolError&) {
            return;
        }
        const Json* type = hello ? field(*hello, "type") : nullptr;
        if (!hello || type == nullptr || !type->is_string() || type->get<std::string>() != "hello") return;

        const Json* userField = field(*hello, "user");
        const Json* passwordField = field(*hello, "password");
        const bool hasUser = pyTruthy(userField);
        const std::string user = hasUser ? pyStr(*userField) : "";
        const std::string password = pyTruthy(passwordField) ? pyStr(*passwordField) : "";
        if (hasUser) {
            // A per-user login SUPERSEDES the shared server password entirely.
            // The message is bare: the client wraps it in ConnectionFailed, which adds the "[Connection Galti] " tag.
            if (!instance_->users().verify(user, password)) {
                protocol::send(socket, failure("User ya password galat hai"), &stop_);
                log(peer + "  login fail (galat user/password: " + pyRepr(user) + ")");
                return;
            }
        } else if (!options_.password.empty() && !constantTimeEquals(password, options_.password)) {
            protocol::send(socket, failure("Password galat hai"), &stop_);
            log(peer + "  login fail (galat password)");
            return;
        }

        Engine session(instance_);
        if (hasUser) session.user = user;
        ++sessions_;
        log(peer + "  connected" + (hasUser ? " as " + pyRepr(user) : std::string()) +
            "  (active sessions: " + std::to_string(sessions_.load()) + ")");

        struct Cleanup {  // Python's `finally:` -- roll back, count down, log
            Server& server;
            Engine& session;
            const std::string& peer;
            ~Cleanup() {
                try {
                    session.close();  // rolls back an unfinished transaction, on the thread that began it
                } catch (...) {
                }
                --server.sessions_;
                server.log(peer + "  disconnected  (active sessions: " + std::to_string(server.sessions_.load()) + ")");
            }
        } cleanup{*this, session, peer};

        const Json* database = field(*hello, "database");
        if (pyTruthy(database)) {
            try {
                ast::UseDatabase use;
                use.name = pyStr(*database);
                session.executeStatement(use);
            } catch (const MeraDBError& e) {
                // .message(), not what(): the client wraps this in ConnectionFailed (see above)
                protocol::send(socket, failure(e.message()), &stop_);
                return;
            }
        }
        Json ready = Json::object();
        ready["ok"] = true;
        ready["server"] = protocol::kServerName;
        ready["protocol"] = protocol::kVersion;
        ready["database"] = session.currentDb;
        protocol::send(socket, ready, &stop_);

        // ---- 2. request loop ----
        for (;;) {
            std::optional<Json> request;
            try {
                request = reader.receive();
            } catch (const protocol::ProtocolError& e) {
                protocol::send(socket, failure(std::string("[Protocol Galti] ") + e.what()), &stop_);
                continue;
            }
            if (!request) break;  // client closed the connection (or the server is stopping)
            bool stopConnection = false;
            Json reply;
            try {
                reply = dispatch(session, *request, peerIp, peer, stopConnection);
            } catch (const std::exception& e) {  // a bug must not kill the whole server
                log(peer + "  INTERNAL ERROR\n" + e.what());
                reply = failure(std::string("[Internal Galti] ") + e.what());
            }
            protocol::send(socket, reply, &stop_);
            if (stopConnection) break;
        }
    } catch (const net::NetError&) {
        // client vanished
    } catch (const std::exception& e) {
        log(peer + "  INTERNAL ERROR\n" + e.what());
    }
}

Json Server::dispatch(Engine& session, const Json& request, const std::string& peerIp, const std::string& peer,
                      bool& stopConnection) {
    const Json* typeField = field(request, "type");
    const Json kind = typeField ? *typeField : Json(nullptr);
    const std::string type = kind.is_string() ? kind.get<std::string>() : "";

    if (type == "query") {
        const Json* textField = field(request, "text");
        const std::string text = textField ? pyStr(*textField) : "";
        if (options_.verbose) log(peer + "  [" + session.currentDb + "]  " + collapseWhitespace(text, 200));
        std::vector<Result> results;
        try {
            results = session.runScript(text);
        } catch (const std::exception& e) {
            log(peer + "  INTERNAL ERROR\n" + e.what());
            Result failed;
            failed.error = std::string("[Internal Galti] ") + e.what();
            results.push_back(std::move(failed));
        }
        Json encoded = Json::array();
        for (const auto& r : results) {
            if (!r.error.empty()) log(peer + "  " + r.error);
            encoded.push_back(protocol::resultToJson(r));
        }
        Json reply = success();
        reply["results"] = std::move(encoded);
        reply["database"] = session.currentDb;
        reply["in_transaction"] = session.inTransaction();
        return reply;
    }

    if (type == "schema") {
        try {
            Json reply = success();
            reply["tree"] = session.schemaTree();
            return reply;
        } catch (const MeraDBError& e) {
            return failure(e.message());  // bare: the client wraps it in ConnectionFailed
        }
    }

    if (type == "status") {
        Json reply = success();
        reply["server"] = protocol::kServerName;
        reply["pid"] = static_cast<std::int64_t>(sys::processId());
        reply["data_dir"] = instance_->dataDir();
        reply["started"] = started_;
        reply["sessions"] = sessions_.load();
        Json databases = Json::array();
        for (const auto& name : instance_->databases()) databases.push_back(name);
        reply["databases"] = std::move(databases);
        return reply;
    }

    if (type == "ping") return success();

    if (type == "shutdown") {
        if (!isLoopback(peerIp))
            return failure("Shutdown sirf usi computer se ho sakta hai jahan server chal raha hai");
        log(peer + "  shutdown requested");
        stop_ = true;
        stopConnection = true;
        return success();
    }

    return failure("Unknown request type: " + pyReprJson(kind));
}

// ============================================================================
// `meradb server`
// ============================================================================

namespace {

std::atomic<Server*> g_running{nullptr};
std::atomic<bool> g_signalled{false};

extern "C" void onStopSignal(int) {
    g_signalled = true;
    if (Server* server = g_running.load()) server->requestStop();
}

std::string plainText(const Json& j) { return j.is_string() ? j.get<std::string>() : j.dump(); }

}  // namespace

int serve(ServerOptions options) {
    namespace fs = std::filesystem;
    options.dataDir = fs::absolute(fs::path(options.dataDir)).string();
    std::error_code ec;
    if (fs::is_directory(options.dataDir, ec)) {
        if (auto existing = protocol::runningServer(options.dataDir)) {
            std::cerr << "Is data folder ka server pehle se chal raha hai: "
                      << plainText(existing->value("host", Json(protocol::kDefaultHost))) << ":"
                      << plainText(existing->value("port", Json(protocol::kDefaultPort))) << " (pid "
                      << plainText(existing->value("pid", Json(nullptr))) << ")\n";
            return 1;
        }
    }

    std::unique_ptr<Server> server;
    try {
        server = std::make_unique<Server>(options);
    } catch (const net::NetError& e) {
        std::cerr << "Server start nahi hua (" << options.host << ":" << options.port << "): " << e.what() << "\n";
        return 1;
    } catch (const MeraDBError& e) {
        std::cerr << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {  // Python's OSError: a data folder that cannot be created or read, ...
        std::cerr << "Server start nahi hua (" << options.host << ":" << options.port << "): " << e.what() << "\n";
        return 1;
    }

    const std::int64_t pid = sys::processId();
    Json info = Json::object();
    info["pid"] = pid;
    info["host"] = options.host;
    info["port"] = server->port();
    info["started"] = server->started();
    try {
        protocol::writePidFile(options.dataDir, info);
    } catch (const MeraDBError& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    server->log(std::string(protocol::kServerName) + " server chal raha hai  ->  " + options.host + ":" +
                std::to_string(server->port()));
    server->log("data folder: " + options.dataDir);
    for (const auto& db : server->instance().recovered())
        server->log("RECOVERY: database '" + db + "' ka adhoora transaction WAPAS kiya (pichli baar crash hua tha)");
    if (options.password.empty()) {
        server->log("password: nahi (koi bhi connect kar sakta hai)");
        if (options.host != "127.0.0.1" && options.host != "::1" && options.host != "localhost")
            server->log("WARNING: bina password ke network par khula hai! --password use karo");
    }
    server->log("band karne ke liye: Ctrl+C  ya  meradb stop");

    g_signalled = false;
    g_running = server.get();
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);
    server->serveForever();
    g_running = nullptr;
    if (g_signalled) server->log("Ctrl+C -- band ho raha hai");

    protocol::removePidFile(options.dataDir, pid);
    server->log("server band");
    return 0;
}

}  // namespace meradb
