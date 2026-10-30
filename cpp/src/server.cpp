// cpp/src/server.cpp
#include "meradb/fs_util.h"
#include "meradb/server.h"
#include "meradb/ast.h"
#include "meradb/datatypes.h"
#include "meradb/errors.h"
#include "meradb/pytext.h"
#include "meradb/query_limits.h"
#include "meradb/sys_compat.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <vector>

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

// ' '.join(text.split())[:limit]  (Unicode whitespace; limit counts characters, not bytes)
std::string collapseWhitespace(const std::string& text, std::size_t limit) {
    std::string joined;
    for (const std::string& word : pytext::split(text)) {
        if (!joined.empty()) joined += ' ';
        joined += word;
    }
    std::size_t at = 0;
    for (std::size_t chars = 0; at < joined.size() && chars < limit; ++chars) {
        char32_t cp = 0;
        at += pytext::decode(joined, at, cp);
    }
    return joined.substr(0, at);
}

// A script that may carry a password (BANAO USER naam GUPT '...') must not reach the log, not even echoed in a parse
// error. It looks at whole words anywhere in the text, so PREPARE's quoted statement and a typo are caught too; a
// false alarm only costs one log line its detail.
bool mayHoldPassword(const std::string& text) {
    std::string word;
    auto sensitive = [&] { return word == "USER" || word == "GUPT" || word == "IDENTIFIED"; };
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            word += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else {
            if (sensitive()) return true;
            word.clear();
        }
    }
    return sensitive();
}

std::string replyTooBig(std::size_t limit) {
    return "[Execution Galti] Result bahut bada hai (limit " + std::to_string(limit / (1024 * 1024)) + " MB) -- JAHAN ya SIRF lagao";
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

// Log text holds what clients sent (user names, table names in error messages). One event stays one line, so a client
// cannot write a line that looks like the server's own, and no terminal escape reaches whoever reads the log.
static std::string oneLogLine(const std::string& message) {
    std::string out;
    out.reserve(message.size());
    for (char c : message) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (byte < 0x20 || byte == 0x7f) {
            static const char* const digits = "0123456789abcdef";
            out += "\\x";
            out += digits[byte >> 4];
            out += digits[byte & 15];
        } else {
            out += c;
        }
    }
    return out;
}

void Server::log(const std::string& message) {
    std::lock_guard<std::mutex> guard(logMutex_);
    options_.log(sys::localLogStamp() + "  " + oneLogLine(message));
}

// Where the data lives is the business of whoever runs the server. The log keeps the full text.
std::string Server::withoutPaths(std::string text) const {
    std::vector<std::string> forms;
    for (const std::string& dir : {instance_->dataDir(), options_.dataDir}) {
        if (dir.size() < 2) continue;  // "." or "/": replacing that would wreck ordinary text
        std::string slashes = dir;
        std::replace(slashes.begin(), slashes.end(), '\\', '/');
        std::string doubled;  // as repr() and JSON write it
        for (char c : dir) {
            doubled += c;
            if (c == '\\') doubled += c;
        }
        forms.push_back(doubled);
        forms.push_back(dir);
        forms.push_back(slashes);
    }
    for (const std::string& form : forms)
        for (std::size_t at = text.find(form); at != std::string::npos; at = text.find(form, at + 13))
            text.replace(at, form.size(), "<data folder>");
    return text;
}

bool Server::loginLocked(const std::string& ip) {
    if (options_.loginFailureLimit <= 0) return false;
    std::lock_guard<std::mutex> guard(loginMutex_);
    auto it = loginFailures_.find(ip);
    return it != loginFailures_.end() && std::chrono::steady_clock::now() < it->second.lockedUntil;
}

void Server::noteLogin(const std::string& ip, bool succeeded) {
    if (options_.loginFailureLimit <= 0) return;
    std::lock_guard<std::mutex> guard(loginMutex_);
    if (succeeded) {
        loginFailures_.erase(ip);
        return;
    }
    if (loginFailures_.size() >= 10000 && !loginFailures_.count(ip)) loginFailures_.clear();  // the table stays bounded
    LoginFailures& failures = loginFailures_[ip];
    if (++failures.count >= options_.loginFailureLimit) {
        failures.count = options_.loginFailureLimit - 1;  // after the lock, one more wrong guess locks again
        failures.lockedUntil = std::chrono::steady_clock::now() +
                               std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double>(options_.loginLockSeconds));
    }
}

bool Server::beginHashing() {
    std::unique_lock<std::mutex> guard(hashingMutex_);
    if (options_.maxConcurrentLogins > 0) {
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                                    std::chrono::duration<double>(options_.loginHashWaitSeconds));
        while (hashing_ >= options_.maxConcurrentLogins) {
            if (stop_ || std::chrono::steady_clock::now() >= giveUp) return false;
            hashingFree_.wait_for(guard, std::chrono::milliseconds(50));
        }
    }
    ++hashing_;
    if (hashing_ > peakHashing_) peakHashing_ = hashing_;
    return true;
}

void Server::endHashing() {
    {
        std::lock_guard<std::mutex> guard(hashingMutex_);
        --hashing_;
    }
    hashingFree_.notify_one();
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
    bool atConnectionLimit = false;
    clock::time_point lastAddressNote;
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
        if (options_.maxConnections > 0 && connections_.size() >= static_cast<std::size_t>(options_.maxConnections)) {
            if (!atConnectionLimit) log("connection limit (" + std::to_string(options_.maxConnections) + ") poori -- naye connection mana");
            atConnectionLimit = true;  // once per episode: a flood of refusals must not flood the log too
            try {
                protocol::send(accepted, failure("Server par bahut zyada connections hain -- thodi der baad try karo"), &stop_);
            } catch (const net::NetError&) {
            }
            continue;
        }
        atConnectionLimit = false;
        const std::string peerIp = accepted.peerAddress();
        if (options_.maxConnectionsPerAddress > 0) {
            int fromThisAddress = 0;
            for (const auto& open : connections_)
                if (open->peerIp == peerIp) ++fromThisAddress;
            if (fromThisAddress >= options_.maxConnectionsPerAddress) {
                const auto now = clock::now();
                if (now - lastAddressNote >= std::chrono::seconds(10)) {  // a flood of refusals must not flood the log too
                    log(peerIp + "  is address ke connections ki limit (" + std::to_string(options_.maxConnectionsPerAddress) +
                        ") poori -- naye connection mana");
                    lastAddressNote = now;
                }
                try {
                    protocol::send(accepted, failure("Is address se bahut zyada connections hain -- thodi der baad try karo"), &stop_);
                } catch (const net::NetError&) {
                }
                continue;
            }
        }

        auto connection = std::make_unique<Connection>();
        connection->socket = std::move(accepted);
        connection->peerIp = peerIp;
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
    const std::string& peerIp = connection.peerIp;
    const std::string peer = peerIp + ":" + std::to_string(socket.peerPort());
    try {
        protocol::MessageReader reader(socket, options_.maxMessageBytes, &stop_);

        // ---- 1. handshake ----
        reader.setMaxBytes(std::min(options_.maxHelloBytes, options_.maxMessageBytes));
        reader.setDeadline(std::chrono::steady_clock::now() +
                           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                               std::chrono::duration<double>(options_.handshakeTimeoutSeconds)));
        std::optional<Json> hello;
        try {
            hello = reader.receive();
        } catch (const protocol::ProtocolError&) {
            return;
        }
        reader.setMaxBytes(options_.maxMessageBytes);
        reader.setDeadline(std::nullopt);
        const Json* type = hello ? field(*hello, "type") : nullptr;
        if (!hello || type == nullptr || !type->is_string() || type->get<std::string>() != "hello") return;

        const Json* userField = field(*hello, "user");
        const Json* passwordField = field(*hello, "password");
        const bool hasUser = pyTruthy(userField);
        const std::string user = hasUser ? pyStr(*userField) : "";
        const std::string password = pyTruthy(passwordField) ? pyStr(*passwordField) : "";
        // No username = superuser. That is only fair while no account exists (the first connection makes the first
        // user) or when the shared password was given. `--local` is the way back in, so nobody can be locked out.
        if (!hasUser && options_.password.empty() && !instance_->users().names().empty()) {
            protocol::send(socket, failure("Is server par users bane hue hain -- username ke saath login karo (-U naam -W). "
                                           "Poora access: server par --password lagao, ya server band karke --local chalao"),
                           &stop_);
            log(peer + "  bina username login mana (users bane hue hain, shared password nahi)");
            return;
        }
        if (hasUser || !options_.password.empty()) {
            if (loginLocked(peerIp)) {  // the same answer as a wrong password: it tells a guesser nothing
                protocol::send(socket, failure(hasUser ? "User ya password galat hai" : "Password galat hai"), &stop_);
                log(peer + "  login mana (is address se bahut galat koshishen)");
                return;
            }
            if (hasUser) {
                // A per-user login SUPERSEDES the shared server password entirely.
                // The message is bare: the client wraps it in ConnectionFailed, which adds the "[Connection Galti] " tag.
                if (!beginHashing()) {
                    protocol::send(socket, failure("Server abhi busy hai -- thodi der baad login karo"), &stop_);
                    log(peer + "  login mana (bahut saare login ek saath)");
                    return;
                }
                bool verified = false;
                try {
                    verified = instance_->users().verify(user, password);
                } catch (...) {
                    endHashing();
                    throw;
                }
                endHashing();
                if (!verified) {
                    noteLogin(peerIp, false);
                    protocol::send(socket, failure("User ya password galat hai"), &stop_);
                    log(peer + "  login fail (galat user/password: " + pyRepr(user) + ")");
                    return;
                }
            } else if (!constantTimeEquals(password, options_.password)) {
                noteLogin(peerIp, false);
                protocol::send(socket, failure("Password galat hai"), &stop_);
                log(peer + "  login fail (galat password)");
                return;
            }
            noteLogin(peerIp, true);
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
                log(peer + "  " + e.message());
                protocol::send(socket, failure(withoutPaths(e.message())), &stop_);
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
        using clock = std::chrono::steady_clock;
        auto lastWork = clock::now();
        for (;;) {
            const double idleLimit = session.inTransaction() ? options_.idleTransactionTimeoutSeconds : options_.idleTimeoutSeconds;
            if (idleLimit > 0)
                reader.setDeadline(lastWork + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(idleLimit)));
            else
                reader.setDeadline(std::nullopt);
            std::optional<Json> request;
            try {
                request = reader.receive();
            } catch (const protocol::ProtocolError& e) {
                protocol::send(socket, failure(std::string("[Protocol Galti] ") + e.what()), &stop_);
                continue;
            }
            if (!request) {  // client closed the connection (or the server is stopping, or the session sat idle)
                if (!stop_ && reader.deadlinePassed()) {
                    const bool inTransaction = session.inTransaction();
                    log(peer + "  idle time poora -- session band" + (inTransaction ? ", transaction WAPAS" : ""));
                    // Whatever the client asks next gets this as its answer, then the closed connection.
                    protocol::send(socket,
                                   failure(inTransaction ? "Session bahut der khali raha: transaction WAPAS kar diya aur connection band -- dobara connect karo"
                                                         : "Session bahut der khali raha: connection band -- dobara connect karo"),
                                   &stop_, options_.sendStallSeconds);
                }
                break;  // the Cleanup above rolls the transaction back, on this thread
            }
            const Json* requestType = field(*request, "type");
            const bool isWork = requestType != nullptr && requestType->is_string() &&
                                (requestType->get_ref<const std::string&>() == "query" || requestType->get_ref<const std::string&>() == "schema");
            bool stopConnection = false;
            Json reply;
            try {
                reply = dispatch(session, *request, peerIp, peer, stopConnection);
            } catch (const std::exception& e) {  // a bug must not kill the whole server
                log(peer + "  INTERNAL ERROR\n" + e.what());
                reply = failure(withoutPaths(std::string("[Internal Galti] ") + e.what()));
            }
            sendReply(socket, reply);
            if (isWork) lastWork = clock::now();
            if (stopConnection) break;
        }
    } catch (const net::NetError&) {
        // client vanished
    } catch (const std::exception& e) {
        log(peer + "  INTERNAL ERROR\n" + e.what());
    }
}

// The one place a reply is turned into bytes: past the limit the client gets an error it can show, instead of a line
// its reader refuses (it would call that a broken connection).
void Server::sendReply(net::Socket& socket, Json& reply) {
    std::string wire = pyjson::dump(reply) + "\n";
    if (options_.maxReplyBytes > 0 && wire.size() > options_.maxReplyBytes && reply.contains("results")) {
        Result refused;
        refused.error = replyTooBig(options_.maxReplyBytes);
        Json results = Json::array();
        results.push_back(protocol::resultToJson(refused));
        reply["results"] = std::move(results);
        wire = pyjson::dump(reply) + "\n";
    }
    socket.sendAll(wire, &stop_, options_.sendStallSeconds);
}

Json Server::dispatch(Engine& session, const Json& request, const std::string& peerIp, const std::string& peer,
                      bool& stopConnection) {
    const Json* typeField = field(request, "type");
    const Json kind = typeField ? *typeField : Json(nullptr);
    const std::string type = kind.is_string() ? kind.get<std::string>() : "";

    if (type == "query") {
        const Json* textField = field(request, "text");
        const std::string text = textField ? pyStr(*textField) : "";
        const bool secretText = mayHoldPassword(text);
        if (options_.verbose)
            log(peer + "  [" + session.currentDb + "]  " + (secretText ? std::string("(user/password wali query -- log mein nahi likhi)")
                                                                       : collapseWhitespace(text, 200)));
        std::vector<Result> results;
        try {
            limits::Limits queryLimits;
            queryLimits.seconds = options_.maxQuerySeconds;
            queryLimits.memoryBytes = options_.maxQueryMemoryBytes;
            queryLimits.valueBytes = options_.maxValueBytes;
            queryLimits.stop = &stop_;
            std::optional<limits::Scope> scope;
            if (queryLimits.seconds > 0 || queryLimits.memoryBytes > 0 || queryLimits.valueBytes > 0) scope.emplace(queryLimits);
            results = session.runScript(text);
        } catch (const std::exception& e) {
            log(peer + "  INTERNAL ERROR\n" + e.what());
            Result failed;
            failed.error = std::string("[Internal Galti] ") + e.what();
            results.push_back(std::move(failed));
        }
        // Refused before encoding: the JSON tree of a huge result would cost several times the result itself.
        std::size_t replyBytes = 0;
        for (auto& r : results) {
            if (options_.maxResultRows > 0 && r.rows.size() > options_.maxResultRows) {
                r = Result();
                r.error = "[Execution Galti] Result mein bahut zyada rows hain (limit " + std::to_string(options_.maxResultRows) +
                          ") -- JAHAN ya SIRF lagao, ya server par MERADB_MAX_RESULT_ROWS badhao";
                continue;
            }
            for (const auto& row : r.rows) {
                for (const auto& value : row) {
                    const auto* cell = std::get_if<std::string>(&value.data);
                    replyBytes += cell != nullptr ? cell->size() + 3 : 2;
                }
            }
            if (options_.maxReplyBytes > 0 && replyBytes > options_.maxReplyBytes) {
                r = Result();
                r.error = replyTooBig(options_.maxReplyBytes);
            }
        }
        Json encoded = Json::array();
        for (auto& r : results) {
            if (!r.error.empty()) {
                log(peer + "  " + (secretText ? std::string("user/password wali query fail hui") : r.error));
                r.error = withoutPaths(r.error);
            }
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
            log(peer + "  " + e.message());
            return failure(withoutPaths(e.message()));  // bare: the client wraps it in ConnectionFailed
        }
    }

    if (type == "status") {
        Json reply = success();
        reply["server"] = protocol::kServerName;
        if (session.user) return reply;  // the folder, the pid and every database name are for a superuser
        reply["pid"] = static_cast<std::int64_t>(sys::processId());
        if (isLoopback(peerIp) && !options_.testPeerIsRemote) reply["data_dir"] = instance_->dataDir();
        reply["started"] = started_;
        reply["sessions"] = sessions_.load();
        Json databases = Json::array();
        for (const auto& name : instance_->databases()) databases.push_back(name);
        reply["databases"] = std::move(databases);
        return reply;
    }

    if (type == "ping") return success();

    if (type == "shutdown") {
        if (session.user) return failure("Shutdown sirf superuser (bina username connect kiya session) kar sakta hai");
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

std::string applyLimitEnvironment(ServerOptions& options) {
    std::string problem;
    auto number = [&](const char* name, auto store) {
        const std::optional<std::string> text = sys::getEnv(name);
        if (!text || text->empty()) return;
        std::size_t used = 0;
        double value = -1;
        try {
            value = std::stod(*text, &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used != text->size() || !(value >= 0) || value > 1e15) {
            if (problem.empty()) problem = std::string(name) + " ek number (0 ya zyada) hona chahiye: '" + *text + "'";
            return;
        }
        store(value);
    };
    number("MERADB_MAX_CONNECTIONS_PER_ADDRESS", [&](double v) { options.maxConnectionsPerAddress = static_cast<int>(std::min(v, 1e9)); });
    number("MERADB_IDLE_TIMEOUT", [&](double v) { options.idleTimeoutSeconds = v; });
    number("MERADB_IDLE_TRANSACTION_TIMEOUT", [&](double v) { options.idleTransactionTimeoutSeconds = v; });
    number("MERADB_MAX_QUERY_SECONDS", [&](double v) { options.maxQuerySeconds = v; });
    number("MERADB_MAX_QUERY_MEMORY_MB", [&](double v) { options.maxQueryMemoryBytes = static_cast<std::size_t>(v * 1024 * 1024); });
    number("MERADB_MAX_RESULT_ROWS", [&](double v) { options.maxResultRows = static_cast<std::size_t>(v); });
    number("MERADB_MAX_VALUE_MB", [&](double v) { options.maxValueBytes = static_cast<std::size_t>(v * 1024 * 1024); });
    return problem;
}

int serve(ServerOptions options) {
    namespace fs = std::filesystem;
    const std::string badSetting = applyLimitEnvironment(options);
    if (!badSetting.empty()) {
        std::cerr << "Server start nahi hua: " << badSetting << "\n";
        return 1;
    }
    options.dataDir = absolutePathOf(options.dataDir);
    std::error_code ec;
    if (fs::is_directory(pathOf(options.dataDir), ec)) {
        if (auto existing = protocol::runningServer(options.dataDir)) {
            std::cerr << "Is data folder ka server pehle se chal raha hai: "
                      << plainText(existing->value("host", Json(protocol::kDefaultHost))) << ":"
                      << plainText(existing->value("port", Json(protocol::kDefaultPort))) << " (pid "
                      << plainText(existing->value("pid", Json(nullptr))) << ")\n";
            return 1;
        }
    }

    // Without a password a connection is a superuser (see the login code), so this would open the whole data folder to
    // the network. Refused before anything is bound.
    if (options.password.empty() && !isLoopback(options.host) && sys::getEnv("MERADB_ALLOW_NO_PASSWORD").value_or("") != "1") {
        std::cerr << "Server start nahi hua: bina password ke network par (" << options.host
                  << ") nahi khol sakte -- --password ya MERADB_PASSWORD do "
                     "(sach mein aisa hi chahiye to MERADB_ALLOW_NO_PASSWORD=1 set karo)\n";
        return 1;
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
