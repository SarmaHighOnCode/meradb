// cpp/include/meradb/users.h
//
// USERS & PRIVILEGES: the server-wide user store. One users.json at the TOP of
// the data folder (not per database), same format as meradb/users.py:
//
//   { "ravi": { "salt": "<hex>", "hash": "<hex>", "grants": {"main.students": ["DIKHAO", "DAALO"]} } }
//
// Added later, each key written ONLY when used (so an old file loads unchanged and a new file stays readable by the
// Python engine, which ignores them):
//   "grant_option":        {"main.students": ["DIKHAO"]}               privileges ravi may pass on (WITH GRANT OPTION)
//   "column_grants":       {"main.students": {"DIKHAO": ["id", "naam"]}}  column-level grants
//   "column_grant_option": the same shape, for column grants that carry WITH GRANT OPTION
//   "roles":               ["clerk"]                                   roles granted to this user
// A ROLE is an entry shaped { "role": true, "grants": {...} } -- no salt or hash, so nobody can log in as one.
//
// Passwords are never stored: PBKDF2-HMAC-SHA256, 100,000 iterations, a random
// 16-byte salt per user. Every method is thread-safe.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace meradb {

constexpr std::uint32_t PBKDF2_ITERATIONS = 100000;

// DIKHAO, DAALO, BADLO, MITAO -- the order privileges are stored in.
const std::vector<std::string>& allPrivileges();

// What a principal (a user plus its active roles) may do with one privilege on one table or view: everything
// (`full`), or only the listed columns.
struct Access {
    bool full = false;
    std::set<std::string> columns;
    bool any() const { return full || !columns.empty(); }
    bool covers(const std::string& column) const { return full || columns.count(column) > 0; }
};

// privilege (DIKHAO / DAALO / BADLO) -> the columns it is limited to
using ColumnPrivs = std::map<std::string, std::vector<std::string>>;

// DIKHAO -> SELECT, DAALO -> INSERT, BADLO -> UPDATE, MITAO -> DELETE (and the reverse, upper-case in/out)
std::string privilegeEnglish(const std::string& privilege);
std::optional<std::string> privilegeFromName(const std::string& upperName);

class UserStore {
public:
    static constexpr const char* kFileName = "users.json";

    explicit UserStore(std::string dataDir);  // loads users.json if it exists

    bool exists(const std::string& name) const;
    void create(const std::string& name, const std::string& password);
    void drop(const std::string& name);
    bool verify(const std::string& name, const std::string& password) const;

    // Grants are keyed "database.table" (a VIEW uses the same keyspace). `name` is a user or a role. A privilege that
    // has an entry in `columns` is granted on those columns only; the others on the whole table.
    void grant(const std::string& name, const std::string& db, const std::string& table,
               const std::vector<std::string>& privileges, const ColumnPrivs& columns = {},
               bool withGrantOption = false);
    // Revoking a privilege without columns removes it from the whole table AND from every column; with columns it
    // only removes those columns.
    void revoke(const std::string& name, const std::string& db, const std::string& table,
                const std::vector<std::string>& privileges, const ColumnPrivs& columns = {});
    // Table-wide privilege held by the user or by any of the roles granted to it.
    bool hasPrivilege(const std::string& name, const std::string& db, const std::string& table,
                      const std::string& privilege) const;
    // The user's own grants plus those of the active roles (nullopt = every role granted to the user).
    // `grantOption` asks about the right to pass the privilege on instead of the privilege itself.
    Access access(const std::string& name, const std::optional<std::set<std::string>>& activeRoles,
                  const std::string& db, const std::string& table, const std::string& privilege,
                  bool grantOption = false) const;

    // roles
    bool isRole(const std::string& name) const;
    void createRole(const std::string& name);
    void dropRole(const std::string& name);  // also takes it away from every user
    void grantRole(const std::string& role, const std::string& user);
    void revokeRole(const std::string& role, const std::string& user);
    std::vector<std::string> rolesOf(const std::string& user) const;
    std::vector<std::string> roleNames() const;  // creation order

    // SHOW GRANTS: "GRANT SELECT, INSERT ON db.t TO name", "GRANT role TO name", ... (never empty)
    std::vector<std::string> grantLines(const std::string& name) const;

    std::vector<std::string> names() const;  // users only, creation order (roles are listed by roleNames)

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
