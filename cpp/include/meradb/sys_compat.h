// cpp/include/meradb/sys_compat.h
//
// The only place that knows about OS differences for the small things the
// server and CLI need: environment, pid, wall clock, randomness. Public
// header: no <windows.h> here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meradb::sys {

// nullopt when the variable is unset (an empty value is returned as "").
std::optional<std::string> getEnv(const std::string& name);

std::int64_t processId();

// Local time, like datetime.now().isoformat(timespec="seconds"): 2026-09-29T14:03:07
std::string localIsoSeconds();
// Local time, like f"{datetime.now():%Y-%m-%d %H:%M:%S}": 2026-09-29 14:03:07
std::string localLogStamp();

// Bytes from the operating system's CSPRNG (BCryptGenRandom / /dev/urandom).
// Throws StorageError if the OS refuses. Never uses std::random_device, which
// is deterministic on some MinGW builds.
std::vector<std::uint8_t> randomBytes(std::size_t count);

// os.path.expanduser("~"): on Windows %USERPROFILE%, else %HOMEDRIVE%%HOMEPATH%, else "~";
// elsewhere $HOME, "." if unknown.
std::string homeDir();

}  // namespace meradb::sys
