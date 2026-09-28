// cpp/tests/test_util.h -- small helpers shared by the on-disk tests.
#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace meradb_test {

// A fresh, uniquely named directory under the system temp dir, removed on
// scope exit. ctest runs each test case as its own process (possibly in
// parallel), so names mix a random_device draw with a clock reading and a
// per-process counter instead of an unseeded rand().
class TempDir {
public:
    explicit TempDir(const std::string& prefix = "meradb_test_") {
        static std::atomic<unsigned> counter{0};
        std::random_device rd;
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                (prefix + std::to_string(rd()) + "_" + std::to_string(stamp) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::string str() const { return path_.string(); }
    std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

// Whole file in TEXT mode (so CRLF line endings on Windows read back as \n).
inline std::string readText(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace meradb_test
