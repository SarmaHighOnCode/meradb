// cpp/src/users.cpp
#include "meradb/fs_util.h"
#include "meradb/users.h"
#include "meradb/crypto.h"
#include "meradb/errors.h"
#include "meradb/sys_compat.h"
#include <algorithm>
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

std::string privilegeEnglish(const std::string& privilege) {
    if (privilege == "DIKHAO") return "SELECT";
    if (privilege == "DAALO") return "INSERT";
    if (privilege == "BADLO") return "UPDATE";
    if (privilege == "MITAO") return "DELETE";
    return privilege;
}

std::optional<std::string> privilegeFromName(const std::string& upperName) {
    for (const auto& p : allPrivileges())
        if (upperName == p || upperName == privilegeEnglish(p)) return p;
    return std::nullopt;
}

namespace {

std::string hashHex(const std::string& password, const crypto::Bytes& salt) {
    return crypto::toHex(crypto::pbkdf2HmacSha256(password, salt, PBKDF2_ITERATIONS, 32));
}

// The table-wide privileges stored under `field` ("grants" or "grant_option") for db.table.
std::set<std::string> tableSet(const json& user, const char* field, const std::string& key) {
    std::set<std::string> out;
    auto grants = user.find(field);
    if (grants == user.end() || !grants->is_object()) return out;
    auto entry = grants->find(key);
    if (entry == grants->end() || !entry->is_array()) return out;
    for (const auto& p : *entry)
        if (p.is_string()) out.insert(p.get<std::string>());
    return out;
}

std::set<std::string> currentGrants(const json& user, const std::string& key) { return tableSet(user, "grants", key); }

// The columns stored under `field` ("column_grants" or "column_grant_option") for db.table and one privilege.
std::set<std::string> columnSet(const json& user, const char* field, const std::string& key, const std::string& priv) {
    std::set<std::string> out;
    auto grants = user.find(field);
    if (grants == user.end() || !grants->is_object()) return out;
    auto entry = grants->find(key);
    if (entry == grants->end() || !entry->is_object()) return out;
    auto cols = entry->find(priv);
    if (cols == entry->end() || !cols->is_array()) return out;
    for (const auto& c : *cols)
        if (c.is_string()) out.insert(c.get<std::string>());
    return out;
}

bool isRoleEntry(const json& entry) {
    auto flag = entry.find("role");
    return flag != entry.end() && flag->is_boolean() && flag->get<bool>();
}

// The roles granted to `entry` (a user), skipping anything that is not a string.
std::vector<std::string> roleList(const json& entry) {
    std::vector<std::string> out;
    auto roles = entry.find("roles");
    if (roles == entry.end() || !roles->is_array()) return out;
    for (const auto& r : *roles)
        if (r.is_string()) out.push_back(r.get<std::string>());
    return out;
}

json sortedPrivileges(const std::set<std::string>& current) {
    json out = json::array();
    for (const auto& p : allPrivileges())
        if (current.count(p)) out.push_back(p);
    return out;
}

// Writes (or, when empty, removes) the table-wide privilege list `field`[key].
void storeTableSet(json& user, const char* field, const std::string& key, const std::set<std::string>& privileges) {
    if (privileges.empty()) {
        auto it = user.find(field);
        if (it != user.end() && it->is_object()) {
            it->erase(key);
            if (it->empty() && std::string(field) != "grants") user.erase(field);  // optional keys vanish when unused
        }
        return;
    }
    if (!user.contains(field) || !user[field].is_object()) user[field] = json::object();
    user[field][key] = sortedPrivileges(privileges);
}

// Writes (or removes) the column list column fields[key][priv]; empty objects are cleaned away.
void storeColumnSet(json& user, const char* field, const std::string& key, const std::string& priv,
                    const std::set<std::string>& columns) {
    if (columns.empty()) {
        auto it = user.find(field);
        if (it == user.end() || !it->is_object()) return;
        auto entry = it->find(key);
        if (entry != it->end() && entry->is_object()) {
            entry->erase(priv);
            if (entry->empty()) it->erase(key);
        }
        if (it->empty()) user.erase(field);
        return;
    }
    if (!user.contains(field) || !user[field].is_object()) user[field] = json::object();
    json& entry = user[field][key];
    if (!entry.is_object()) entry = json::object();
    json list = json::array();
    for (const auto& c : columns) list.push_back(c);
    entry[priv] = std::move(list);
}

ExecutionError noSuchUser(const std::string& name) { return ExecutionError("User '" + name + "' exist nahi karta"); }

}  // namespace

UserStore::UserStore(std::string dataDir) : path_((pathOf(dataDir) / kFileName).u8string()) { load(); }

void UserStore::load() {
    if (!fs::exists(pathOf(path_))) return;
    std::ifstream in(pathOf(path_), std::ios::binary);
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
        std::ofstream out(pathOf(tmp));  // text mode on purpose: same line endings Python's open(..., "w") writes
        if (!out) throw StorageError(tmp + " likh nahi paaye");
        out << text;
        out.flush();
        if (!out) throw StorageError(tmp + " likh nahi paaye");
    }
    std::error_code ec;
    fs::rename(pathOf(tmp), pathOf(path_), ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(pathOf(tmp), ignored);  // don't leave a stale .tmp behind
        throw StorageError(path_ + " save nahi hua: " + ec.message());
    }
}

// Runs `change` on the in-memory store, then saves. On ANY failure (a malformed hand-edited
// users.json, or the save itself) the in-memory state is put back exactly as it was.
template <typename Change>
void UserStore::mutateAndSave(Change&& change) {
    json backup = users_;
    try {
        try {
            change();
        } catch (const nlohmann::json::exception& e) {
            throw StorageError(path_ + " corrupt hai: " + e.what());
        }
        saveLocked();
    } catch (...) {
        users_ = std::move(backup);
        throw;
    }
}

bool UserStore::exists(const std::string& name) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return users_.contains(name);
}

std::vector<std::string> UserStore::names() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<std::string> out;
    for (auto it = users_.begin(); it != users_.end(); ++it)
        if (!it.value().is_object() || !isRoleEntry(it.value())) out.push_back(it.key());
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
    if (!users_.contains(name) || isRoleEntry(users_[name])) throw noSuchUser(name);
    mutateAndSave([&] { users_.erase(name); });
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
    return crypto::constantTimeEquals(hashHex(password, salt), expected);
}

void UserStore::grant(const std::string& name, const std::string& db, const std::string& table,
                      const std::vector<std::string>& privileges, const ColumnPrivs& columns, bool withGrantOption) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end()) throw noSuchUser(name);
    std::string key = db + "." + table;
    mutateAndSave([&] {
        if (!it->is_object() || (it->contains("grants") && !(*it)["grants"].is_object()))
            throw StorageError(path_ + " corrupt hai: user '" + name + "' ki entry sahi nahi hai");  // hand-edited file
        std::set<std::string> whole = currentGrants(*it, key);
        std::set<std::string> wholeOption = tableSet(*it, "grant_option", key);
        for (const auto& p : privileges) {
            auto limited = columns.find(p);
            if (limited != columns.end() && !limited->second.empty()) {
                std::set<std::string> cols = columnSet(*it, "column_grants", key, p);
                cols.insert(limited->second.begin(), limited->second.end());
                storeColumnSet(*it, "column_grants", key, p, cols);
                if (withGrantOption) {
                    std::set<std::string> opt = columnSet(*it, "column_grant_option", key, p);
                    opt.insert(limited->second.begin(), limited->second.end());
                    storeColumnSet(*it, "column_grant_option", key, p, opt);
                }
            } else {
                whole.insert(p);
                if (withGrantOption) wholeOption.insert(p);
            }
        }
        storeTableSet(*it, "grants", key, whole);
        storeTableSet(*it, "grant_option", key, wholeOption);
    });
}

void UserStore::revoke(const std::string& name, const std::string& db, const std::string& table,
                       const std::vector<std::string>& privileges, const ColumnPrivs& columns) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end()) throw noSuchUser(name);
    std::string key = db + "." + table;
    mutateAndSave([&] {
        if (!it->is_object() || (it->contains("grants") && !(*it)["grants"].is_object()))
            throw StorageError(path_ + " corrupt hai: user '" + name + "' ki entry sahi nahi hai");  // hand-edited file
        std::set<std::string> whole = currentGrants(*it, key);
        std::set<std::string> wholeOption = tableSet(*it, "grant_option", key);
        for (const auto& p : privileges) {
            auto limited = columns.find(p);
            if (limited != columns.end() && !limited->second.empty()) {
                for (const char* field : {"column_grants", "column_grant_option"}) {
                    std::set<std::string> cols = columnSet(*it, field, key, p);
                    for (const auto& c : limited->second) cols.erase(c);
                    storeColumnSet(*it, field, key, p, cols);
                }
            } else {
                whole.erase(p);
                wholeOption.erase(p);
                storeColumnSet(*it, "column_grants", key, p, {});
                storeColumnSet(*it, "column_grant_option", key, p, {});
            }
        }
        storeTableSet(*it, "grants", key, whole);
        storeTableSet(*it, "grant_option", key, wholeOption);
    });
}

bool UserStore::hasPrivilege(const std::string& name, const std::string& db, const std::string& table,
                             const std::string& privilege) const {
    return access(name, std::nullopt, db, table, privilege).full;
}

Access UserStore::access(const std::string& name, const std::optional<std::set<std::string>>& activeRoles,
                         const std::string& db, const std::string& table, const std::string& privilege,
                         bool grantOption) const {
    std::lock_guard<std::mutex> guard(mutex_);
    Access out;
    auto it = users_.find(name);
    if (it == users_.end() || !it.value().is_object()) return out;
    const std::string key = db + "." + table;
    auto addFrom = [&](const json& principal) {
        if (tableSet(principal, grantOption ? "grant_option" : "grants", key).count(privilege)) out.full = true;
        for (const auto& c : columnSet(principal, grantOption ? "column_grant_option" : "column_grants", key, privilege))
            out.columns.insert(c);
    };
    addFrom(*it);
    for (const auto& role : roleList(*it)) {
        if (activeRoles && !activeRoles->count(role)) continue;
        auto r = users_.find(role);
        if (r != users_.end() && r.value().is_object() && isRoleEntry(r.value())) addFrom(*r);
    }
    return out;
}

// ---- roles ----

bool UserStore::isRole(const std::string& name) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    return it != users_.end() && it.value().is_object() && isRoleEntry(it.value());
}

void UserStore::createRole(const std::string& name) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto existing = users_.find(name);
    if (existing != users_.end()) {
        if (existing.value().is_object() && isRoleEntry(existing.value()))
            throw ExecutionError("Role '" + name + "' pehle se hai");
        throw ExecutionError("'" + name + "' naam ka user pehle se hai -- role ka naam alag rakho");
    }
    mutateAndSave([&] {
        json role = json::object();
        role["role"] = true;
        role["grants"] = json::object();
        users_[name] = std::move(role);
    });
}

void UserStore::dropRole(const std::string& name) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end() || !it.value().is_object() || !isRoleEntry(it.value()))
        throw ExecutionError("Role '" + name + "' exist nahi karta");
    mutateAndSave([&] {
        users_.erase(name);
        for (auto user = users_.begin(); user != users_.end(); ++user) {
            if (!user.value().is_object() || !user.value().contains("roles")) continue;
            json kept = json::array();
            for (const auto& r : roleList(user.value()))
                if (r != name) kept.push_back(r);
            if (kept.empty()) user.value().erase("roles");
            else user.value()["roles"] = std::move(kept);
        }
    });
}

void UserStore::grantRole(const std::string& role, const std::string& user) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto r = users_.find(role);
    if (r == users_.end() || !r.value().is_object() || !isRoleEntry(r.value()))
        throw ExecutionError("Role '" + role + "' exist nahi karta");
    auto it = users_.find(user);
    if (it == users_.end()) throw noSuchUser(user);
    if (it.value().is_object() && isRoleEntry(it.value()))
        throw ExecutionError("'" + user + "' ek role hai, user nahi -- role sirf user ko diya ja sakta hai");
    mutateAndSave([&] {
        json list = json::array();
        bool present = false;
        for (const auto& existing : roleList(*it)) {
            list.push_back(existing);
            if (existing == role) present = true;
        }
        if (!present) list.push_back(role);
        (*it)["roles"] = std::move(list);
    });
}

void UserStore::revokeRole(const std::string& role, const std::string& user) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(user);
    if (it == users_.end()) throw noSuchUser(user);
    std::vector<std::string> have = roleList(*it);
    if (std::find(have.begin(), have.end(), role) == have.end())
        throw ExecutionError("Role '" + role + "' user '" + user + "' ko diya hi nahi gaya");
    mutateAndSave([&] {
        json kept = json::array();
        for (const auto& r : have)
            if (r != role) kept.push_back(r);
        if (kept.empty()) it->erase("roles");
        else (*it)["roles"] = std::move(kept);
    });
}

std::vector<std::string> UserStore::rolesOf(const std::string& user) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(user);
    if (it == users_.end() || !it.value().is_object()) return {};
    return roleList(*it);
}

std::vector<std::string> UserStore::roleNames() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<std::string> out;
    for (auto it = users_.begin(); it != users_.end(); ++it)
        if (it.value().is_object() && isRoleEntry(it.value())) out.push_back(it.key());
    return out;
}

std::vector<std::string> UserStore::grantLines(const std::string& name) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end()) throw noSuchUser(name);
    const json& principal = it.value();

    std::vector<std::string> keys;  // "db.table" in the order first stored
    auto collect = [&](const char* field) {
        auto f = principal.find(field);
        if (f == principal.end() || !f->is_object()) return;
        for (auto k = f->begin(); k != f->end(); ++k)
            if (std::find(keys.begin(), keys.end(), k.key()) == keys.end()) keys.push_back(k.key());
    };
    collect("grants");
    collect("column_grants");

    auto names = [](const std::set<std::string>& privileges) {
        std::string out;
        for (const auto& p : allPrivileges())
            if (privileges.count(p)) out += (out.empty() ? "" : ", ") + privilegeEnglish(p);
        return out;
    };
    auto joinColumns = [](const std::set<std::string>& cols) {
        std::string out;
        for (const auto& c : cols) out += (out.empty() ? "" : ", ") + c;
        return out;
    };

    std::vector<std::string> lines;
    for (const auto& key : keys) {
        std::set<std::string> whole = tableSet(principal, "grants", key);
        std::set<std::string> option = tableSet(principal, "grant_option", key);
        std::set<std::string> plain, withOption;
        for (const auto& p : whole) (option.count(p) ? withOption : plain).insert(p);
        std::string tail = " ON " + key + " TO " + name;
        if (!plain.empty()) lines.push_back("GRANT " + names(plain) + tail);
        if (!withOption.empty()) lines.push_back("GRANT " + names(withOption) + tail + " WITH GRANT OPTION");
        for (const auto& p : allPrivileges()) {
            std::set<std::string> cols = columnSet(principal, "column_grants", key, p);
            std::set<std::string> colOption = columnSet(principal, "column_grant_option", key, p);
            std::set<std::string> colPlain, colWith;
            for (const auto& c : cols) (colOption.count(c) ? colWith : colPlain).insert(c);
            if (!colPlain.empty())
                lines.push_back("GRANT " + privilegeEnglish(p) + " (" + joinColumns(colPlain) + ")" + tail);
            if (!colWith.empty())
                lines.push_back("GRANT " + privilegeEnglish(p) + " (" + joinColumns(colWith) + ")" + tail +
                                " WITH GRANT OPTION");
        }
    }
    for (const auto& role : roleList(principal)) lines.push_back("GRANT " + role + " TO " + name);
    if (lines.empty()) lines.push_back("GRANT USAGE ON *.* TO " + name);
    return lines;
}

}  // namespace meradb
