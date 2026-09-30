// cpp/include/meradb/protocol.h
//
// The MeraDB WIRE PROTOCOL: newline-delimited JSON over TCP, one object per
// line. Mirrors meradb/protocol.py (framing, pid file, default data folder)
// plus the Result <-> JSON codec from meradb/engine.py (to_dict/from_dict).
// See docs/SERVER.md for the message catalogue.
#pragma once
#include "meradb/engine.h"
#include "meradb/net_compat.h"
#include "meradb/pyjson.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace meradb::protocol {

using pyjson::Json;

constexpr const char* kDefaultHost = "127.0.0.1";
constexpr int kDefaultPort = 6372;  // M-E-R-A on a phone keypad
constexpr int kVersion = 1;
constexpr std::size_t kMaxMessageBytes = 64u * 1024u * 1024u;  // refuse absurdly large lines
constexpr const char* kPidFile = "meradb.pid";
constexpr const char* kServerName = "MeraDB 1.0.0";  // "MeraDB " + Python's __version__

// The bare message is what goes on the wire ("[Protocol Galti] " is added by
// the server when it reports one).
class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& message) : std::runtime_error(message) {}
};

// Reads newline-delimited JSON objects from a socket, keeping the bytes that
// follow a message for the next call.
class MessageReader {
public:
    // With `stop`, the reader polls the flag every 100 ms while waiting for
    // data and gives up (returns nullopt, as if the peer had closed) once it is
    // set -- how server threads are stopped without relying on socket tricks.
    explicit MessageReader(net::Socket& socket, std::size_t maxBytes = kMaxMessageBytes,
                           const std::atomic<bool>* stop = nullptr)
        : socket_(socket), maxBytes_(maxBytes), stop_(stop) {}

    // nullopt = the peer closed the connection (or `stop` was set).
    // Throws ProtocolError for a bad message -- the reader stays usable, the next
    // call continues with the next line. net::NetError passes through.
    std::optional<Json> receive();

private:
    enum class Fill { Data, Eof, Stopped };
    Fill fill();
    Json parseLine(const std::string& line) const;

    net::Socket& socket_;
    std::size_t maxBytes_;
    const std::atomic<bool>* stop_;
    std::string buffer_;
    bool discarding_ = false;  // skipping the rest of an oversize line
};

// One JSON object plus "\n". Throws net::NetError.
void send(net::Socket& socket, const Json& message);

// ---- cells and results (engine.py: Result.to_dict / from_dict) ----
// null / bool / int / float / string as themselves; a DATE as {"$date": "YYYY-MM-DD"}.
Json valueToWire(const Value& value);
Value valueFromWire(const Json& json);  // throws ProtocolError
Json resultToJson(const Result& result);
Result resultFromJson(const Json& json);  // throws ProtocolError

// ---- data folder and pid file ----
// Where databases live unless -D / --data says otherwise: MERADB_DATA, else
// %LOCALAPPDATA%\MeraDB\data (Windows) or ~/.local/share/MeraDB/data.
std::string defaultDataDir();
std::string defaultDataDirFrom(const std::optional<std::string>& meradbData,
                               const std::optional<std::string>& localAppData, const std::string& home,
                               bool windows);

std::string pidFilePath(const std::string& dataDir);
std::optional<Json> readPidFile(const std::string& dataDir);  // nullopt: missing or unreadable
void writePidFile(const std::string& dataDir, const Json& info);  // json.dump(indent=2)
// Removes the file, but only if it still belongs to process `pid`.
void removePidFile(const std::string& dataDir, std::int64_t pid);
// The pid-file info if a server really answers on that port, else nullopt
// (a stale file left by a crashed server).
std::optional<Json> runningServer(const std::string& dataDir);

}  // namespace meradb::protocol
