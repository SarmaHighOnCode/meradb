// cpp/src/client.cpp
#include "meradb/client.h"
#include "meradb/errors.h"

namespace meradb {

using protocol::Json;

namespace {

bool isOk(const Json& reply) {
    auto it = reply.find("ok");
    return it != reply.end() && it->is_boolean() && it->get<bool>();
}

// reply.get(key, fallback) for a string-valued key
std::string stringField(const Json& reply, const char* key, const std::string& fallback) {
    auto it = reply.find(key);
    return (it != reply.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

}  // namespace

Connection::Connection(const ConnectOptions& options) : options_(options) {
    try {
        socket_ = net::connectTo(options.host, options.port, options.connectTimeoutSeconds);
    } catch (const net::NetError& e) {
        throw ServerUnavailable(options.host + ":" + std::to_string(options.port) + " par MeraDB server nahi mila (" +
                                e.what() + ")");
    }
    socket_.setReceiveTimeout(options.timeoutSeconds);
    reader_ = std::make_unique<protocol::MessageReader>(socket_);

    // `user`, if given, authenticates as that SPECIFIC user (checked against users.json)
    // instead of the single shared server password.
    Json hello = Json::object();
    hello["type"] = "hello";
    hello["version"] = protocol::kVersion;
    hello["password"] = options.password ? Json(*options.password) : Json(nullptr);
    hello["database"] = options.database ? Json(*options.database) : Json(nullptr);
    hello["user"] = options.user ? Json(*options.user) : Json(nullptr);
    Json reply = request(hello);
    if (!isOk(reply)) {
        close();
        throw ConnectionFailed(stringField(reply, "error", "Server ne connection mana kar diya"));
    }
    serverVersion_ = stringField(reply, "server", "MeraDB");
    currentDb_ = stringField(reply, "database", DEFAULT_DATABASE);
    inTransaction_ = false;
}

Json Connection::request(const Json& message) {
    try {
        protocol::send(socket_, message);
        auto reply = reader_->receive();
        if (!reply) throw ConnectionFailed("Server ne connection band kar diya");
        return *reply;
    } catch (const net::NetError& e) {
        throw ConnectionFailed(std::string("Server se connection toot gaya: ") + e.what());
    } catch (const protocol::ProtocolError& e) {
        throw ConnectionFailed(std::string("Server se connection toot gaya: ") + e.what());
    }
}

std::vector<Result> Connection::runScript(const std::string& text) {
    Json message = Json::object();
    message["type"] = "query";
    message["text"] = text;
    Json reply = request(message);
    if (!isOk(reply)) {
        Result failed;
        failed.error = stringField(reply, "error", "Unknown error");
        return {failed};
    }
    try {
        std::vector<Result> results;
        for (const auto& encoded : reply.at("results")) results.push_back(protocol::resultFromJson(encoded));
        currentDb_ = stringField(reply, "database", currentDb_);
        auto txn = reply.find("in_transaction");
        inTransaction_ = txn != reply.end() && txn->is_boolean() && txn->get<bool>();
        return results;
    } catch (const protocol::ProtocolError& e) {
        throw ConnectionFailed(std::string("Server se connection toot gaya: ") + e.what());
    } catch (const nlohmann::json::exception& e) {
        throw ConnectionFailed(std::string("Server se connection toot gaya: Galat message: ") + e.what());
    }
}

std::vector<Result> Connection::execute(const std::string& text) {
    auto results = runScript(text);
    for (const auto& r : results)
        if (!r.error.empty()) throw MeraDBError(r.error);
    return results;
}

nlohmann::ordered_json Connection::schemaTree() {
    Json message = Json::object();
    message["type"] = "schema";
    Json reply = request(message);
    if (!isOk(reply)) throw ConnectionFailed(stringField(reply, "error", "schema nahi mila"));
    auto tree = reply.find("tree");
    if (tree == reply.end()) throw ConnectionFailed("schema nahi mila");
    return *tree;
}

Json Connection::status() {
    Json message = Json::object();
    message["type"] = "status";
    return request(message);
}

void Connection::shutdown() {
    Json message = Json::object();
    message["type"] = "shutdown";
    Json reply = request(message);
    if (!isOk(reply)) throw ConnectionFailed(stringField(reply, "error", "shutdown fail"));
}

}  // namespace meradb
