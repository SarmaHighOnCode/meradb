// cpp/src/protocol.cpp
#include "meradb/protocol.h"
#include "meradb/errors.h"
#include "meradb/sys_compat.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace meradb::protocol {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// framing
// ---------------------------------------------------------------------------

MessageReader::Fill MessageReader::fill() {
    if (stop_ != nullptr) {
        while (!socket_.waitReadable(0.1))
            if (stop_->load()) return Fill::Stopped;
    }
    char chunk[65536];
    std::size_t n = socket_.recvSome(chunk, sizeof chunk);
    if (n == 0) return Fill::Eof;
    buffer_.append(chunk, n);
    return Fill::Data;
}

Json MessageReader::parseLine(const std::string& line, bool terminated) const {
    const std::string decodeError = pyjson::utf8ErrorText(terminated ? line + "\n" : line);
    if (!decodeError.empty()) throw ProtocolError("Galat message: " + decodeError);
    Json message;
    try {
        message = pyjson::parse(line);
    } catch (const pyjson::ParseFailure& e) {
        throw ProtocolError(std::string("Galat message: ") + e.what());
    }
    if (!message.is_object()) throw ProtocolError("Message ek JSON object hona chahiye");
    return message;
}

std::optional<Json> MessageReader::receive() {
    for (;;) {
        if (discarding_) {  // the rest of an oversize line: drop it up to its newline
            auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                buffer_.erase(0, newline + 1);
                scanned_ = 0;
                discarding_ = false;
            } else {
                buffer_.clear();
                scanned_ = 0;
                if (fill() != Fill::Data) return std::nullopt;
                continue;
            }
        }
        auto newline = buffer_.find('\n', scanned_);  // earlier bytes were already searched: stays linear
        if (newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            scanned_ = 0;
            if (line.size() + 1 > maxBytes_) throw ProtocolError("Message bahut bada hai");  // Python counts the "\n"
            return parseLine(line, true);
        }
        scanned_ = buffer_.size();
        if (buffer_.size() >= maxBytes_) {
            buffer_.clear();
            scanned_ = 0;
            discarding_ = true;
            throw ProtocolError("Message bahut bada hai");
        }
        Fill state = fill();
        if (state == Fill::Stopped) return std::nullopt;
        if (state == Fill::Eof) {
            if (buffer_.empty()) return std::nullopt;
            std::string tail = std::move(buffer_);  // Python's readline() hands back an unterminated tail too
            buffer_.clear();
            scanned_ = 0;
            return parseLine(tail, false);
        }
    }
}

void send(net::Socket& socket, const Json& message, const std::atomic<bool>* stop) {
    socket.sendAll(pyjson::dump(message) + "\n", stop);
}

// ---------------------------------------------------------------------------
// cells and results
// ---------------------------------------------------------------------------

Json valueToWire(const Value& value) {
    const auto& d = value.data;
    if (std::holds_alternative<std::monostate>(d)) return nullptr;
    if (std::holds_alternative<bool>(d)) return std::get<bool>(d);
    if (std::holds_alternative<int64_t>(d)) return static_cast<std::int64_t>(std::get<int64_t>(d));
    if (std::holds_alternative<double>(d)) return std::get<double>(d);
    if (std::holds_alternative<std::string>(d)) return std::get<std::string>(d);
    Json date = Json::object();
    date["$date"] = std::get<Date>(d).isoFormat();
    return date;
}

Value valueFromWire(const Json& json) {
    if (json.is_null()) return Value();
    if (json.is_boolean()) return Value(json.get<bool>());
    if (json.is_number_unsigned()) {
        std::uint64_t u = json.get<std::uint64_t>();
        if (u <= static_cast<std::uint64_t>(INT64_MAX)) return Value(static_cast<int64_t>(u));
        return Value(static_cast<double>(u));  // beyond 64 bits: the closest a C++ INT can get
    }
    if (json.is_number_integer()) return Value(static_cast<int64_t>(json.get<std::int64_t>()));
    if (json.is_number_float()) return Value(json.get<double>());
    if (json.is_string()) return Value(json.get<std::string>());
    if (json.is_object() && json.contains("$date") && json.at("$date").is_string()) {
        try {
            return Value(parseDate(json.at("$date").get<std::string>()));
        } catch (const MeraDBError&) {
            throw ProtocolError("Galat message: $date ki value date nahi hai");
        }
    }
    throw ProtocolError("Galat message: cell ki value samajh nahi aayi");
}

Json resultToJson(const Result& result) {
    Json columns = Json::array();
    for (const auto& name : result.columns) columns.push_back(name);
    Json rows = Json::array();
    for (const auto& row : result.rows) {
        Json cells = Json::array();
        for (const auto& value : row) cells.push_back(valueToWire(value));
        rows.push_back(std::move(cells));
    }
    Json out = Json::object();
    out["columns"] = std::move(columns);
    out["rows"] = std::move(rows);
    out["message"] = result.message;
    out["error"] = result.error;
    return out;
}

Result resultFromJson(const Json& json) {
    Result result;
    try {
        if (!json.is_object()) throw ProtocolError("Galat message: result ek object hona chahiye");
        if (json.contains("columns"))
            for (const auto& name : json.at("columns")) result.columns.push_back(name.get<std::string>());
        if (json.contains("rows")) {
            for (const auto& row : json.at("rows")) {
                std::vector<Value> cells;
                for (const auto& cell : row) cells.push_back(valueFromWire(cell));
                result.rows.push_back(std::move(cells));
            }
        }
        if (json.contains("message")) result.message = json.at("message").get<std::string>();
        if (json.contains("error")) result.error = json.at("error").get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        throw ProtocolError(std::string("Galat message: ") + e.what());
    }
    return result;
}

// ---------------------------------------------------------------------------
// data folder and pid file
// ---------------------------------------------------------------------------

std::string defaultDataDirFrom(const std::optional<std::string>& meradbData,
                               const std::optional<std::string>& localAppData, const std::string& home,
                               bool windows) {
    if (meradbData && !meradbData->empty()) return *meradbData;
    std::string base;
    if (windows && localAppData && !localAppData->empty()) base = *localAppData;
    if (base.empty()) base = (fs::path(home) / ".local" / "share").string();
    return (fs::path(base) / "MeraDB" / "data").string();
}

std::string defaultDataDir() {
#ifdef _WIN32
    const bool windows = true;
#else
    const bool windows = false;
#endif
    return defaultDataDirFrom(sys::getEnv("MERADB_DATA"), sys::getEnv("LOCALAPPDATA"), sys::homeDir(), windows);
}

std::string pidFilePath(const std::string& dataDir) { return (fs::path(dataDir) / kPidFile).string(); }

std::optional<Json> readPidFile(const std::string& dataDir) {
    std::ifstream file(pidFilePath(dataDir), std::ios::binary);
    if (!file) return std::nullopt;
    std::ostringstream text;
    text << file.rdbuf();
    try {
        Json info = pyjson::parse(text.str());
        if (!info.is_object()) return std::nullopt;
        if (info.empty()) return std::nullopt;  // Python: `if not info` -- an empty {} means "no server"
        return info;
    } catch (const pyjson::ParseFailure&) {
        return std::nullopt;
    }
}

void writePidFile(const std::string& dataDir, const Json& info) {
    std::ofstream file(pidFilePath(dataDir), std::ios::binary | std::ios::trunc);
    if (!file) throw StorageError("meradb.pid likh nahi paaye: " + pidFilePath(dataDir));
    file << info.dump(2, ' ', true);  // json.dump(info, f, indent=2): no trailing newline
    if (!file) throw StorageError("meradb.pid likh nahi paaye: " + pidFilePath(dataDir));
}

void removePidFile(const std::string& dataDir, std::int64_t pid) {
    auto info = readPidFile(dataDir);
    if (info && info->contains("pid") && info->at("pid").is_number_integer() &&
        info->at("pid").get<std::int64_t>() == pid) {
        std::error_code ec;
        fs::remove(pidFilePath(dataDir), ec);
    }
}

std::optional<Json> runningServer(const std::string& dataDir) {
    auto info = readPidFile(dataDir);
    if (!info) return std::nullopt;
    std::string host = kDefaultHost;
    if (info->contains("host") && info->at("host").is_string()) host = info->at("host").get<std::string>();
    if (host == "0.0.0.0" || host.empty() || host == "::") host = kDefaultHost;
    int port = kDefaultPort;
    if (info->contains("port") && info->at("port").is_number_integer()) {
        std::int64_t wide = info->at("port").get<std::int64_t>();
        if (wide < 0 || wide > 65535) return std::nullopt;  // no such port can be listening
        port = static_cast<int>(wide);
    }
    if (net::portOpen(host, port)) return info;
    return std::nullopt;  // stale pid file left behind by a crashed server
}

}  // namespace meradb::protocol
