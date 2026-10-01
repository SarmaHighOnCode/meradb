// cpp/include/meradb/server_control.h
//
// `meradb start | stop | status` (mirrors cmd_start / cmd_stop / cmd_status in
// meradb/cli.py). Each function prints like Python does -- results on stdout,
// side notes on stderr -- and returns the process exit code.
#pragma once
#include <optional>
#include <string>

namespace meradb {

struct ControlOptions {
    std::string dataDir;
    std::string host = "127.0.0.1";
    int port = 6372;
    std::optional<std::string> password;  // shared password: given to a starting server, or used to reach a running one
    bool verbose = false;                 // start: log every query
    bool force = false;                   // stop: kill the process if a polite shutdown fails
    std::string exePath;                  // start: the program to run as `<exe> server ...` (default: this program)
    double startTimeoutSeconds = 15.0;
    double stopTimeoutSeconds = 10.0;
};

int serverStart(const ControlOptions& options);
int serverStop(const ControlOptions& options);
int serverStatus(const ControlOptions& options);

}  // namespace meradb
