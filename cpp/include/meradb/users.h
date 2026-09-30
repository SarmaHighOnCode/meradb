// cpp/include/meradb/users.h
//
// USERS & PRIVILEGES: the server-wide user store. One users.json at the TOP of
// the data folder (not per database), same format as meradb/users.py:
//
//   { "ravi": { "salt": "<hex>", "hash": "<hex>", "grants": {"main.students": ["DIKHAO", "DAALO"]} } }
//
// Passwords are never stored: PBKDF2-HMAC-SHA256, 100,000 iterations, a random
// 16-byte salt per user. Every method is thread-safe.
#pragma once
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace meradb {

constexpr std::uint32_t PBKDF2_ITERATIONS = 100000;

// DIKHAO, DAALO, BADLO, MITAO -- the order privileges are stored in.
const std::vector<std::string>& allPrivileges();

class UserStore {
public:
    static constexpr const char* kFileName = "users.json";

    explicit UserStore(std::string dataDir);  // loads users.json if it exists

    bool exists(const std::string& name) const;
    void create(const std::string& name, const std::string& password);
    void drop(const std::string& name);
    bool verify(const std::string& name, const std::string& password) const;

    // Grants are keyed "database.table" (a VIEW uses the same keyspace).
    void grant(const std::string& name, const std::string& db, const std::string& table,
               const std::vector<std::string>& privileges);
    void revoke(const std::string& name, const std::string& db, const std::string& table,
                const std::vector<std::string>& privileges);
    bool hasPrivilege(const std::string& name, const std::string& db, const std::string& table,
                      const std::string& privilege) const;

    std::vector<std::string> names() const;  // creation order

private:
    mutable std::mutex mutex_;
    std::string path_;
    nlohmann::ordered_json users_ = nlohmann::ordered_json::object();

    void load();
    void saveLocked();  // caller holds mutex_
    template <typename Change>
    void mutateAndSave(Change&& change);  // caller holds mutex_; rolls back on failure
};

}  // namespace meradb
