// cpp/src/users.cpp
#include "meradb/users.h"
#include "meradb/crypto.h"
#include "meradb/errors.h"
#include "meradb/sys_compat.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace meradb {

const std::vector<std::string>& allPrivileges() {
    static const std::vector<std::string> privileges = {"DIKHAO", "DAALO", "BADLO", "MITAO"};
    return privileges;
}

namespace {

std::string hashHex(const std::string& password, const crypto::Bytes& salt) {
    return crypto::toHex(crypto::pbkdf2HmacSha256(password, salt, PBKDF2_ITERATIONS, 32));
}

std::set<std::string> currentGrants(const json& user, const std::string& key) {
    std::set<std::string> out;
    auto grants = user.find("grants");
    if (grants == user.end() || !grants->is_object()) return out;
    auto entry = grants->find(key);
    if (entry == grants->end() || !entry->is_array()) return out;
    for (const auto& p : *entry)
        if (p.is_string()) out.insert(p.get<std::string>());
    return out;
}

json sortedPrivileges(const std::set<std::string>& current) {
    json out = json::array();
    for (const auto& p : allPrivileges())
        if (current.count(p)) out.push_back(p);
    return out;
}

ExecutionError noSuchUser(const std::string& name) { return ExecutionError("User '" + name + "' exist nahi karta"); }

}  // namespace

UserStore::UserStore(std::string dataDir) : path_((fs::path(dataDir) / kFileName).string()) { load(); }

void UserStore::load() {
    if (!fs::exists(path_)) return;
    std::ifstream in(path_, std::ios::binary);
    if (!in) throw StorageError(path_ + " khul nahi paayi");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        users_ = json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        throw StorageError(path_ + " corrupt hai: " + e.what());
    }
    if (!users_.is_object()) throw StorageError(path_ + " corrupt hai: object expected tha");
}

void UserStore::saveLocked() {
    std::string text;
    try {
        text = users_.dump(2, ' ', true);  // json.dump(indent=2), ensure_ascii
    } catch (const nlohmann::json::exception& e) {
        throw StorageError(std::string("users.json likh nahi paaye: ") + e.what());
    }
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp);  // text mode on purpose: same line endings Python's open(..., "w") writes
        if (!out) throw StorageError(tmp + " likh nahi paaye");
        out << text;
        out.flush();
        if (!out) throw StorageError(tmp + " likh nahi paaye");
    }
    std::error_code ec;
    fs::rename(tmp, path_, ec);
    if (ec) throw StorageError(path_ + " save nahi hua: " + ec.message());
}

bool UserStore::exists(const std::string& name) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return users_.contains(name);
}

std::vector<std::string> UserStore::names() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<std::string> out;
    for (auto it = users_.begin(); it != users_.end(); ++it) out.push_back(it.key());
    return out;
}

void UserStore::create(const std::string& name, const std::string& password) {
    if (exists(name)) throw ExecutionError("User '" + name + "' pehle se hai");
    // the slow part (100,000 iterations) runs without the lock
    crypto::Bytes salt = sys::randomBytes(16);
    std::string hash = hashHex(password, salt);
    std::lock_guard<std::mutex> guard(mutex_);
    if (users_.contains(name)) throw ExecutionError("User '" + name + "' pehle se hai");  // lost a race
    json user = json::object();
    user["salt"] = crypto::toHex(salt);
    user["hash"] = hash;
    user["grants"] = json::object();
    users_[name] = std::move(user);
    try {
        saveLocked();
    } catch (...) {
        users_.erase(name);
        throw;
    }
}

void UserStore::drop(const std::string& name) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!users_.contains(name)) throw noSuchUser(name);
    users_.erase(name);
    saveLocked();
}

bool UserStore::verify(const std::string& name, const std::string& password) const {
    std::string saltHex, expected;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = users_.find(name);
        if (it == users_.end()) return false;
        try {
            saltHex = (*it).at("salt").get<std::string>();
            expected = (*it).at("hash").get<std::string>();
        } catch (const nlohmann::json::exception&) {
            return false;  // a malformed entry never authenticates
        }
    }
    crypto::Bytes salt;
    try {
        salt = crypto::fromHex(saltHex);
    } catch (const StorageError&) {
        return false;
    }
    return hashHex(password, salt) == expected;
}

void UserStore::grant(const std::string& name, const std::string& db, const std::string& table,
                      const std::vector<std::string>& privileges) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end()) throw noSuchUser(name);
    std::string key = db + "." + table;
    std::set<std::string> current = currentGrants(*it, key);
    current.insert(privileges.begin(), privileges.end());
    (*it)["grants"][key] = sortedPrivileges(current);
    saveLocked();
}

void UserStore::revoke(const std::string& name, const std::string& db, const std::string& table,
                       const std::vector<std::string>& privileges) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end()) throw noSuchUser(name);
    std::string key = db + "." + table;
    std::set<std::string> current = currentGrants(*it, key);
    for (const auto& p : privileges) current.erase(p);
    if (!current.empty()) (*it)["grants"][key] = sortedPrivileges(current);
    else (*it)["grants"].erase(key);
    saveLocked();
}

bool UserStore::hasPrivilege(const std::string& name, const std::string& db, const std::string& table,
                             const std::string& privilege) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end()) return false;
    return currentGrants(*it, db + "." + table).count(privilege) > 0;
}

}  // namespace meradb
