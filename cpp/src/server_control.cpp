// cpp/src/server_control.cpp -- see server_control.h.
#include "meradb/fs_util.h"
#include "meradb/server_control.h"
#include "meradb/client.h"
#include "meradb/errors.h"
#include "meradb/net_compat.h"
#include "meradb/protocol.h"
#include "meradb/sys_compat.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace meradb {
namespace {

using protocol::Json;

void note(const std::string& message) { std::cerr << message << "\n"; }

std::string absolutePath(const std::string& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(std::filesystem::u8path(path), ec);
    return ec ? path : absolute.lexically_normal().u8string();
}

bool isWildcardHost(const std::string& host) { return host == "0.0.0.0" || host == "::" || host.empty(); }

std::string tailOf(const std::string& path, std::size_t lines = 15) {
    std::ifstream in(pathOf(path), std::ios::binary);
    if (!in) return "(log nahi mila)";
    std::vector<std::string> all;
    std::string line;
    while (std::getline(in, line)) all.push_back(line + "\n");
    std::string out;
    for (std::size_t i = all.size() > lines ? all.size() - lines : 0; i < all.size(); ++i) out += all[i];
    return out;
}

std::string passwordFor(const ControlOptions& options) {
    if (options.password) return *options.password;
    return sys::getEnv("MERADB_PASSWORD").value_or("");
}

ConnectOptions connectOptionsFor(const ControlOptions& options, const std::string& host, int port) {
    ConnectOptions connect;
    connect.host = host;
    connect.port = port;
    std::string password = passwordFor(options);
    if (!password.empty()) connect.password = password;
    return connect;
}

double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleepShort() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }

}  // namespace

int serverStart(const ControlOptions& options) {
    const std::string data = absolutePath(options.dataDir);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(data), ec);
    if (auto info = protocol::runningServer(data)) {
        std::cout << "Server pehle se chal raha hai: " << info->value("host", std::string("?")) << ":"
                  << (*info)["port"].dump() << " (pid " << (*info)["pid"].dump() << ")\n";
        return 0;
    }
    const std::string checkHost = isWildcardHost(options.host) ? protocol::kDefaultHost : options.host;
    if (net::portOpen(checkHost, options.port, 0.5)) {
        note("Port " + std::to_string(options.port) +
             " par pehle se kuch aur chal raha hai. Doosra port do:  meradb start --port 6373");
        return 1;
    }

    const std::string logPath = (std::filesystem::u8path(data) / "server.log").u8string();
    std::vector<std::string> args = {"server", "--data", data, "--host", options.host, "--port",
                                     std::to_string(options.port)};
    if (options.verbose) args.push_back("--verbose");
    // The password travels in the environment so it does not show in the process list.
    const bool hasPassword = options.password && !options.password->empty();
    std::optional<std::string> savedPassword = sys::getEnv("MERADB_PASSWORD");
    if (hasPassword) sys::setEnv("MERADB_PASSWORD", *options.password);

    std::unique_ptr<sys::DetachedProcess> child;
    try {
        child = sys::spawnDetached(options.exePath.empty() ? sys::executablePath() : options.exePath, args, logPath);
    } catch (const std::exception& e) {
        if (hasPassword) sys::setEnv("MERADB_PASSWORD", savedPassword.value_or(""));
        note(e.what());
        return 1;
    }
    if (hasPassword) sys::setEnv("MERADB_PASSWORD", savedPassword.value_or(""));

    const double deadline = seconds() + options.startTimeoutSeconds;
    while (seconds() < deadline) {
        if (child->exited()) {
            note("Server start nahi hua. Log (" + logPath + "):");
            note(tailOf(logPath));
            return 1;
        }
        // No server served this folder a moment ago, so any that appears now is ours.
        if (auto info = protocol::runningServer(data)) {
            std::cout << "MeraDB server chal gaya: " << options.host << ":" << (*info)["port"].dump() << "  (pid "
                      << (*info)["pid"].dump() << ")\n";
            std::cout << "  data: " << data << "\n";
            std::cout << "  log:  " << logPath << "\n";
            std::cout << "  connect: meradb shell   |   band: meradb stop\n";
            return 0;
        }
        sleepShort();
    }
    note("Server " + std::to_string(static_cast<int>(options.startTimeoutSeconds)) +
         " second mein ready nahi hua. Log dekho: " + logPath);
    return 1;
}

int serverStop(const ControlOptions& options) {
    const std::string data = absolutePath(options.dataDir);
    auto info = protocol::runningServer(data);
    if (!info) {
        if (auto stale = protocol::readPidFile(data))
            protocol::removePidFile(data, stale->value("pid", static_cast<std::int64_t>(-1)));  // left by a crash
        std::cout << "Server nahi chal raha.\n";
        return 0;
    }
    std::string host = info->value("host", std::string(protocol::kDefaultHost));
    if (isWildcardHost(host)) host = protocol::kDefaultHost;
    const int port = static_cast<int>((*info)["port"].get<std::int64_t>());
    const std::int64_t pid = info->value("pid", static_cast<std::int64_t>(0));
    try {
        Connection connection(connectOptionsFor(options, host, port));
        connection.shutdown();
    } catch (const MeraDBError& e) {
        if (!options.force) {
            note(std::string(e.what()) + "\nZabardasti band karne ke liye:  meradb stop --force");
            return 1;
        }
        note(std::string(e.what()) + " -- process " + std::to_string(pid) + " ko kill kar rahe hain (--force)");
        std::string failure = sys::killProcess(pid);
        if (!failure.empty()) {
            note("Kill fail: " + failure);
            return 1;
        }
        protocol::removePidFile(data, pid);
    }
    const double deadline = seconds() + options.stopTimeoutSeconds;
    while (seconds() < deadline) {
        if (!net::portOpen(host, port, 0.5)) {
            std::cout << "MeraDB server band ho gaya (pid " << pid << ").\n";
            return 0;
        }
        sleepShort();
    }
    note("Server abhi bhi chal raha hai -- `meradb stop --force` try karo");
    return 1;
}

int serverStatus(const ControlOptions& options) {
    const std::string data = absolutePath(options.dataDir);
    auto info = protocol::runningServer(data);
    if (!info) {
        std::cout << "MeraDB server nahi chal raha  (data: " << data << ")\n";
        std::cout << "Start karne ke liye: meradb start\n";
        return 3;  // conventional "not running" exit code for database status tools
    }
    const std::string host = info->value("host", std::string(protocol::kDefaultHost));
    const std::string connectHost = isWildcardHost(host) ? protocol::kDefaultHost : host;
    const int port = static_cast<int>((*info)["port"].get<std::int64_t>());
    std::cout << "MeraDB server chal raha hai\n";
    std::cout << "  address:   " << host << ":" << port << "\n";
    std::cout << "  pid:       " << (info->contains("pid") ? (*info)["pid"].dump() : std::string("None")) << "\n";
    std::cout << "  data:      " << data << "\n";
    std::cout << "  started:   " << info->value("started", std::string("?")) << "\n";
    try {
        Connection connection(connectOptionsFor(options, connectHost, port));
        Json status = connection.status();
        std::cout << "  version:   " << status.value("server", std::string("None")) << "\n";
        std::cout << "  sessions:  " << status["sessions"].dump() << " connected\n";
        std::string names;
        for (const auto& name : status["databases"]) names += (names.empty() ? "" : ", ") + name.get<std::string>();
        std::cout << "  databases: " << names << "\n";
    } catch (const MeraDBError& e) {
        std::cout << "  (details nahi mile: " << e.what() << ")\n";
    }
    return 0;
}

}  // namespace meradb
