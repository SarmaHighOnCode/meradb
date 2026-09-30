// cpp/src/sys_compat.cpp
#include "meradb/sys_compat.h"
#include "meradb/errors.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#endif

namespace meradb::sys {

std::optional<std::string> getEnv(const std::string& name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, name.c_str()) != 0 || buffer == nullptr) return std::nullopt;
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

std::int64_t processId() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

namespace {

std::tm localTm(std::time_t t) {
    std::tm out{};
#if defined(_MSC_VER)
    if (localtime_s(&out, &t) != 0) throw StorageError("local time nahi mila");
#elif defined(_WIN32)
    // MinGW: std::localtime uses a static buffer, so serialise the calls.
    static std::mutex m;
    std::lock_guard<std::mutex> guard(m);
    const std::tm* p = std::localtime(&t);
    if (p == nullptr) throw StorageError("local time nahi mila");
    out = *p;
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::string formatNow(const char* format) {
    std::tm tmv = localTm(std::time(nullptr));
    char buffer[64];
    std::size_t n = std::strftime(buffer, sizeof buffer, format, &tmv);
    return std::string(buffer, n);
}

}  // namespace

std::string localIsoSeconds() { return formatNow("%Y-%m-%dT%H:%M:%S"); }
std::string localLogStamp() { return formatNow("%Y-%m-%d %H:%M:%S"); }

std::vector<std::uint8_t> randomBytes(std::size_t count) {
    std::vector<std::uint8_t> out(count);
    if (count == 0) return out;
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw StorageError("OS random generator (BCryptGenRandom) ne jawab nahi diya");
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom) throw StorageError("/dev/urandom khul nahi paaya");
    urandom.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count));
    if (!urandom) throw StorageError("/dev/urandom se random bytes nahi mile");
#endif
    return out;
}

std::string homeDir() {
#ifdef _WIN32
    // Python's ntpath.expanduser("~"): USERPROFILE, else HOMEDRIVE + HOMEPATH; HOME is ignored.
    // With neither set the "~" is left as is.
    if (auto v = getEnv("USERPROFILE")) return *v;
    if (auto path = getEnv("HOMEPATH")) {
        std::string drive = getEnv("HOMEDRIVE").value_or("");
        return drive + *path;
    }
    return "~";
#else
    if (auto v = getEnv("HOME"); v && !v->empty()) return *v;
    return ".";
#endif
}

}  // namespace meradb::sys
