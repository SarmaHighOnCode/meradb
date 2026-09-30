// cpp/src/engine.cpp -- mirrors meradb/engine.py
#include "meradb/engine.h"
#include "meradb/aggregates.h"
#include "meradb/ast_util.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/protocol.h"
#include "meradb/pyvalue.h"
#include "meradb/storage.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <unordered_set>

namespace fs = std::filesystem;

namespace meradb {

// ============================================================================
// Instance: shared state for one data folder
// ============================================================================

namespace {

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// shutil.rmtree(path, ignore_errors=True)
void removeTreeQuietly(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

// os.replace for a directory: atomic rename; any failure is a StorageError.
void replacePath(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) throw StorageError("'" + from.string() + "' ko '" + to.string() + "' nahi bana paaye: " + ec.message());
}

}  // namespace

Instance::Instance(std::string dataDir, bool served)
    : dataDir_(fs::absolute(fs::path(dataDir)).string()), users_(dataDir_) {
    fs::create_directories(dataDir_);
    if (!served) {
        // Two processes must never write the same files: refuse a folder a server is serving.
        if (auto info = protocol::runningServer(dataDir_)) {
            std::string port = info->contains("port") ? info->at("port").dump() : "None";
            throw MeraDBError("Is data folder par MeraDB server chal raha hai (port " + port +
                              "). Seedha files mat kholo -- `meradb shell` se server se connect karo.");
        }
    }
    recovered_ = recover();
    fs::create_directories(dbDir(DEFAULT_DATABASE));
}

std::string Instance::dbDir(const std::string& name) const { return (fs::path(dataDir_) / name).string(); }

std::vector<std::string> Instance::databases() const {
    std::vector<std::string> names;
    // Error-code overloads: a concurrent DROP DATABASE may remove an entry mid-listing.
    std::error_code ec;
    for (fs::directory_iterator it(dataDir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        std::string name = it->path().filename().string();
        if (!name.empty() && name[0] != '.' && it->is_directory(entryEc) && !entryEc) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

Catalog& Instance::catalog(const std::string& db) {
    auto it = catalogs_.find(db);
    if (it == catalogs_.end()) it = catalogs_.emplace(db, std::make_unique<Catalog>(dbDir(db))).first;
    return *it->second;
}

void Instance::forget(const std::string& db) {
    catalogs_.erase(db);
    indexes_.erase(db);
}

IndexCache Instance::indexCache(const std::string& db, const std::string& table) {
    auto& slot = indexes_[db][table];
    if (!slot) slot = std::make_shared<std::optional<IndexMap>>();
    return slot;
}

void Instance::dropIndexCache(const std::string& db, const std::string& table) {
    auto it = indexes_.find(db);
    if (it == indexes_.end()) return;
    auto slot = it->second.find(table);
    if (slot == it->second.end()) return;
    slot->second->reset();  // any Table still holding the slot sees it emptied too
    it->second.erase(slot);
}

std::string Instance::snapshotPath(const std::string& db, const std::string& suffix) const {
    return (fs::path(dataDir_) / SNAPSHOT_DIR / (db + suffix)).string();
}

void Instance::takeSnapshot(const std::string& db) {
    fs::path tmp = snapshotPath(db, ".tmp");
    removeTreeQuietly(tmp);
    fs::create_directories(tmp.parent_path());
    std::error_code ec;
    fs::copy(dbDir(db), tmp, fs::copy_options::recursive, ec);
    if (ec) throw StorageError("Database '" + db + "' ki copy nahi ban paayi: " + ec.message());
    replacePath(tmp, snapshotPath(db));  // only a COMPLETE copy ever gets the real name
}

void Instance::discardSnapshot(const std::string& db) {
    fs::path done = snapshotPath(db, ".done");
    replacePath(snapshotPath(db), done);  // <- the COMMIT POINT: after this, no rollback
    removeTreeQuietly(done);
}

void Instance::restoreSnapshot(const std::string& db) {
    removeTreeQuietly(dbDir(db));
    replacePath(snapshotPath(db), dbDir(db));
    forget(db);
}

std::vector<std::string> Instance::recover() {
    // CRASH RECOVERY, run at startup. A snapshot still lying around means the
    // process died in the middle of a transaction that never reached PAKKA,
    // so it is rolled back. `.tmp` = SHURU never finished; `.done` = PAKKA
    // already happened -- both are just deleted.
    fs::path root = fs::path(dataDir_) / SNAPSHOT_DIR;
    std::vector<std::string> recovered;
    if (!fs::is_directory(root)) return recovered;
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(root)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        if (endsWith(name, ".tmp") || endsWith(name, ".done")) {
            removeTreeQuietly(root / name);
        } else {
            restoreSnapshot(name);
            recovered.push_back(name);
        }
    }
    return recovered;
}

// ============================================================================
// Engine: one session
// ============================================================================

Engine::Engine(std::shared_ptr<Instance> instance) : instance_(std::move(instance)) {}
Engine::Engine(const std::string& dataDir) : instance_(std::make_shared<Instance>(dataDir)) {}

Engine::~Engine() {
    // Like a Python Engine that is simply dropped (no close()): an open
    // transaction is NOT rolled back here -- its snapshot stays on disk and
    // crash recovery undoes it at the next start. Only the extra lock hold
    // taken by SHURU is released -- but only from the thread that owns it
    // (unlocking a recursive mutex from another thread is undefined; in that
    // case the hold is left alone rather than corrupting the mutex).
    if (inTransaction() && txnThread_ == std::this_thread::get_id()) instance_->lock.unlock();
}

Catalog& Engine::catalog() { return instance_->catalog(currentDb); }

// ============================================================================
// small helpers
// ============================================================================

namespace {

std::string joinStrs(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : std::string()) + v[i];
    return out;
}

// Python's len(str): characters, not UTF-8 bytes.
size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

Value textValue(std::string s) { return Value(std::move(s)); }

Result messageResult(std::string message) {
    Result r;
    r.message = std::move(message);
    return r;
}

// Python's repr() of a stored value, for the `{v!r}` in error messages.
std::string pyReprValue(const Value& v) {
    if (v.isNull()) return "None";
    if (isBoolValue(v)) return std::get<bool>(v.data) ? "True" : "False";
    if (isIntValue(v)) return std::to_string(std::get<int64_t>(v.data));
    if (isDoubleValue(v)) return pyReprFloat(std::get<double>(v.data));
    if (isTextValue(v)) return pyRepr(std::get<std::string>(v.data));
    if (isDateValue(v)) {
        std::string iso = std::get<Date>(v.data).isoFormat();  // YYYY-MM-DD
        auto d1 = iso.find('-'), d2 = iso.find('-', d1 + 1);
        return "datetime.date(" + std::to_string(std::stoi(iso.substr(0, d1))) + ", " +
               std::to_string(std::stoi(iso.substr(d1 + 1, d2 - d1 - 1))) + ", " +
               std::to_string(std::stoi(iso.substr(d2 + 1))) + ")";
    }
    return "?";
}

std::string pyReprTuple(const std::vector<Value>& key) {
    std::string out = "(";
    for (size_t i = 0; i < key.size(); ++i) out += (i ? ", " : "") + pyReprValue(key[i]);
    return out + (key.size() == 1 ? ",)" : ")");
}

std::string pyReprStrList(const std::vector<std::string>& names) {
    std::string out = "[";
    for (size_t i = 0; i < names.size(); ++i) out += (i ? ", " : "") + pyRepr(names[i]);
    return out + "]";
}

bool nonEmpty(const std::optional<std::string>& s) { return s.has_value() && !s->empty(); }

void checkCompositeColumns(const std::string& table, const std::vector<std::string>& columnNames,
                           const std::vector<std::vector<std::string>>& groups) {
    for (const auto& group : groups) {
        for (const auto& name : group)
            if (std::find(columnNames.begin(), columnNames.end(), name) == columnNames.end())
                throw ExecutionError("Table '" + table + "': composite constraint mein column '" + name + "' nahi hai");
        std::set<std::string> distinct(group.begin(), group.end());
        if (distinct.size() != group.size())
            throw ExecutionError("Table '" + table + "': composite constraint mein ek column do baar diya hai");
    }
}

// Python's Engine._check_length. `len(value)` counts characters.
void checkLength(const Column& col, const Value& value) {
    if (col.maxLength.has_value() && isTextValue(value) &&
        utf8Length(std::get<std::string>(value.data)) > static_cast<size_t>(*col.maxLength))
        throw ExecutionError("Column '" + col.name + "' mein zyada se zyada " + std::to_string(*col.maxLength) +
                             " characters ho sakte hain");
}

// A VIEW's MaterializedTable has no declared column types -- infer one from
// the FIRST non-KHALI value in that column, defaulting to TEXT.
std::string inferColumnType(const std::vector<std::vector<Value>>& rows, size_t position) {
    for (const auto& row : rows) {
        const Value& v = row[position];
        if (v.isNull()) continue;
        if (isBoolValue(v)) return "BOOL";
        if (isIntValue(v)) return "INT";
        if (isDoubleValue(v)) return "FLOAT";
        if (isDateValue(v)) return "DATE";
        return "TEXT";
    }
    return "TEXT";
}

const char* setOpName(const std::string& op) {
    return op == "SANYUKT" ? "UNION" : op == "SAAJHA" ? "INTERSECT" : "EXCEPT";
}

}  // namespace

// ============================================================================
// public API
// ============================================================================

// executeStatement, with a stray std::exception (e.g. std::filesystem_error
// from a locked or vanished file) turned into a StorageError, so callers only
// ever have to deal with MeraDBError.
Result Engine::guarded(const ast::Statement& stmt) {
    try {
        return executeStatement(stmt);
    } catch (const MeraDBError&) {
        throw;
    } catch (const std::exception& e) {
        throw StorageError(e.what());
    }
}

std::vector<Result> Engine::execute(const std::string& text) {
    auto statements = parseScript(text);
    std::vector<Result> results;
    for (const auto& stmt : statements) results.push_back(guarded(*stmt));
    return results;
}

std::vector<Result> Engine::runScript(const std::string& text) {
    std::vector<std::unique_ptr<ast::Statement>> statements;
    try {
        statements = parseScript(text);
    } catch (const MeraDBError& e) {
        Result r;
        r.error = e.what();
        return {r};
    }
    std::vector<Result> results;
    for (const auto& stmt : statements) {
        try {
            results.push_back(guarded(*stmt));
        } catch (const MeraDBError& e) {
            Result r;
            r.error = e.what();
            results.push_back(std::move(r));
        }
    }
    return results;
}

Result Engine::executeStatement(const ast::Statement& stmt) {
    // Hold the instance lock for the statement (waiting at most lockTimeoutSeconds).
    auto guard = acquireLock(instance_->lockTimeoutSeconds);
    if (!fs::is_directory(instance_->dbDir(currentDb))) {
        std::string gone = currentDb;
        currentDb = DEFAULT_DATABASE;
        throw ExecutionError("Database '" + gone + "' ab exist nahi karta. Ab '" + DEFAULT_DATABASE + "' use ho raha hai.");
    }
    checkPrivileges(stmt);
    using namespace ast;
    if (auto* s = dynamic_cast<const CreateDatabase*>(&stmt)) return execCreateDatabase(*s);
    if (auto* s = dynamic_cast<const DropDatabase*>(&stmt)) return execDropDatabase(*s);
    if (auto* s = dynamic_cast<const UseDatabase*>(&stmt)) return execUseDatabase(*s);
    if (auto* s = dynamic_cast<const ShowTables*>(&stmt)) return execShowTables(*s);
    if (auto* s = dynamic_cast<const Describe*>(&stmt)) return execDescribe(*s);
    if (auto* s = dynamic_cast<const CreateTable*>(&stmt)) return execCreateTable(*s);
    if (auto* s = dynamic_cast<const DropTable*>(&stmt)) return execDropTable(*s);
    if (auto* s = dynamic_cast<const TruncateTable*>(&stmt)) return execTruncateTable(*s);
    if (auto* s = dynamic_cast<const CompactTable*>(&stmt)) return execCompactTable(*s);
    if (auto* s = dynamic_cast<const AlterAddColumn*>(&stmt)) return execAlterAddColumn(*s);
    if (auto* s = dynamic_cast<const AlterAddComposite*>(&stmt)) return execAlterAddComposite(*s);
    if (auto* s = dynamic_cast<const AlterDropColumn*>(&stmt)) return execAlterDropColumn(*s);
    if (auto* s = dynamic_cast<const RenameTable*>(&stmt)) return execRenameTable(*s);
    if (auto* s = dynamic_cast<const RenameColumn*>(&stmt)) return execRenameColumn(*s);
    if (auto* s = dynamic_cast<const Begin*>(&stmt)) return execBegin(*s);
    if (auto* s = dynamic_cast<const Commit*>(&stmt)) return execCommit(*s);
    if (auto* s = dynamic_cast<const Rollback*>(&stmt)) return execRollback(*s);
    if (auto* s = dynamic_cast<const Insert*>(&stmt)) return execInsert(*s);
    if (auto* s = dynamic_cast<const Update*>(&stmt)) return execUpdate(*s);
    if (auto* s = dynamic_cast<const Delete*>(&stmt)) return execDelete(*s);
    if (auto* s = dynamic_cast<const Select*>(&stmt)) return execSelect(*s);
    if (auto* s = dynamic_cast<const SetOp*>(&stmt)) return execSetOp(*s);
    if (auto* s = dynamic_cast<const CreateView*>(&stmt)) return execCreateView(*s);
    if (auto* s = dynamic_cast<const DropView*>(&stmt)) return execDropView(*s);
    if (auto* s = dynamic_cast<const ShowViews*>(&stmt)) return execShowViews(*s);
    if (auto* s = dynamic_cast<const Explain*>(&stmt)) return execExplain(*s);
    if (auto* s = dynamic_cast<const CreateUser*>(&stmt)) return execCreateUser(*s);
    if (auto* s = dynamic_cast<const DropUser*>(&stmt)) return execDropUser(*s);
    if (auto* s = dynamic_cast<const Grant*>(&stmt)) return execGrant(*s);
    if (auto* s = dynamic_cast<const Revoke*>(&stmt)) return execRevoke(*s);
    if (auto* s = dynamic_cast<const CreateTrigger*>(&stmt)) return execCreateTrigger(*s);
    if (auto* s = dynamic_cast<const DropTrigger*>(&stmt)) return execDropTrigger(*s);
    if (auto* s = dynamic_cast<const CreateProcedure*>(&stmt)) return execCreateProcedure(*s);
    if (auto* s = dynamic_cast<const DropProcedure*>(&stmt)) return execDropProcedure(*s);
    if (auto* s = dynamic_cast<const CallProcedure*>(&stmt)) return execCallProcedure(*s);
    throw ExecutionError("Ye statement abhi supported nahi hai");
}

std::unique_lock<std::recursive_timed_mutex> Engine::acquireLock(double timeoutSeconds) {
    std::unique_lock<std::recursive_timed_mutex> guard(instance_->lock, std::defer_lock);
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(timeoutSeconds));
    if (!guard.try_lock_for(wait))
        throw ExecutionError("Database busy hai -- kisi aur session ka transaction chal raha hai. Thodi der baad try karo.");
    return guard;
}

nlohmann::ordered_json Engine::schemaTree() {
    using nlohmann::ordered_json;
    auto guard = acquireLock(2.0);
    ordered_json tree = ordered_json::array();
    for (const auto& db : instance_->databases()) {
        Catalog& cat = instance_->catalog(db);
        std::vector<std::string> names;
        for (const auto& entry : cat.tables) names.push_back(entry.first);
        std::sort(names.begin(), names.end());  // Python: sorted(catalog.tables)
        ordered_json tables = ordered_json::array();
        for (const auto& name : names) {
            ordered_json columns = ordered_json::array();
            for (const auto& col : cat.tables.at(name).columns) columns.push_back(col.toJson());
            ordered_json table = ordered_json::object();
            table["name"] = name;
            table["columns"] = std::move(columns);
            tables.push_back(std::move(table));
        }
        ordered_json entry = ordered_json::object();
        entry["name"] = db;
        entry["current"] = (db == currentDb);
        entry["tables"] = std::move(tables);
        tree.push_back(std::move(entry));
    }
    return tree;
}

void Engine::close() {
    if (inTransaction()) {
        try {
            executeStatement(ast::Rollback());
        } catch (const MeraDBError&) {
        }
    }
}

// ============================================================================
// users & privileges (server-wide -- see users.h)
// ============================================================================

std::vector<std::string> Engine::tablesRead(const ast::Select& stmt) {
    // The FROM table plus every MILAO'd table -- NOT tables read only through a
    // VIEW's own stored query (that runs via resolveSource, never through
    // executeStatement, so it is never re-checked; a view is meant to be granted
    // by its own name).
    std::vector<std::string> tables{stmt.table};
    for (const auto& join : stmt.joins) tables.push_back(join.table);
    return tables;
}

void Engine::requirePrivilege(const std::string& privilege, const std::string& table) {
    if (!instance_->users().hasPrivilege(*user, currentDb, table, privilege))
        throw ExecutionError("'" + *user + "' ko table '" + table + "' par " + privilege + " ka adhikar nahi hai");
}

void Engine::checkPrivileges(const ast::Statement& stmt) {
    if (!user) return;  // superuser: checking is skipped entirely
    using namespace ast;
    if (auto* ex = dynamic_cast<const Explain*>(&stmt)) {
        checkPrivileges(*ex->statement);
        return;
    }
    if (dynamic_cast<const SetOp*>(&stmt)) return;  // execSetOp re-enters executeStatement for each side
    if (auto* sel = dynamic_cast<const Select*>(&stmt)) {
        for (const auto& t : tablesRead(*sel)) requirePrivilege("DIKHAO", t);
        return;
    }
    if (auto* ins = dynamic_cast<const Insert*>(&stmt)) {
        requirePrivilege("DAALO", ins->table);
        if (ins->select)
            for (const auto& t : tablesRead(*ins->select)) requirePrivilege("DIKHAO", t);
        return;
    }
    if (auto* upd = dynamic_cast<const Update*>(&stmt)) {
        requirePrivilege("BADLO", upd->table);
        return;
    }
    if (auto* del = dynamic_cast<const Delete*>(&stmt)) {
        requirePrivilege("MITAO", del->table);
        return;
    }
    // Everything else: DDL, VIEW/TRIGGER/PROCEDURE management, users and grants,
    // transactions, database switching.
    throw ExecutionError("'" + *user + "' superuser nahi hai -- '" + astClassName(stmt) +
                         "' jaisa DDL/admin command sirf superuser (bina username connect kiya session) chala sakta hai");
}

Result Engine::execCreateUser(const ast::CreateUser& stmt) {
    instance_->users().create(stmt.name, stmt.password);
    return messageResult("User '" + stmt.name + "' ban gaya");
}

Result Engine::execDropUser(const ast::DropUser& stmt) {
    instance_->users().drop(stmt.name);
    return messageResult("User '" + stmt.name + "' hata diya");
}

Result Engine::execGrant(const ast::Grant& stmt) {
    instance_->users().grant(stmt.user, currentDb, stmt.table, stmt.privileges);
    return messageResult("'" + stmt.user + "' ko '" + currentDb + "." + stmt.table + "' par " +
                         joinStrs(stmt.privileges, ", ") + " ka adhikar mil gaya");
}

Result Engine::execRevoke(const ast::Revoke& stmt) {
    instance_->users().revoke(stmt.user, currentDb, stmt.table, stmt.privileges);
    return messageResult("'" + stmt.user + "' se '" + currentDb + "." + stmt.table + "' par " +
                         joinStrs(stmt.privileges, ", ") + " ka adhikar wapas le liya");
}

// ============================================================================
// triggers (fire once per affected row on DAALO/BADLO/MITAO)
// ============================================================================

Result Engine::execCreateTrigger(const ast::CreateTrigger& stmt) {
    Catalog& cat = catalog();
    if (cat.hasTrigger(stmt.name)) throw ExecutionError("Trigger '" + stmt.name + "' pehle se hai");
    if (cat.find(stmt.table) == nullptr)
        throw ExecutionError("Table '" + stmt.table + "' exist nahi karta -- trigger sirf ek REAL table par lag sakta hai");
    parseScript(stmt.bodyText);  // sanity check: the body must parse cleanly
    cat.addTrigger(stmt.name, stmt.timing, stmt.event, stmt.table, stmt.bodyText);
    return messageResult("Trigger '" + stmt.name + "' ban gaya (" + stmt.timing + " " + stmt.event + " PAR " +
                         stmt.table + ")");
}

Result Engine::execDropTrigger(const ast::DropTrigger& stmt) {
    Catalog& cat = catalog();
    if (!cat.hasTrigger(stmt.name)) throw ExecutionError("Trigger '" + stmt.name + "' exist nahi karta");
    cat.removeTrigger(stmt.name);
    return messageResult("Trigger '" + stmt.name + "' hata diya");
}

Row Engine::rowDict(const TableSchema& schema, const std::vector<Value>& values) {
    Row row;
    for (size_t i = 0; i < schema.columns.size() && i < values.size(); ++i) row[schema.columns[i].name] = values[i];
    return row;
}

void Engine::runBody(const std::string& bodyText, const RefReplacer& replace, std::vector<Result>* results) {
    struct DepthGuard {  // local class: same access rights as the enclosing member function
        Engine& engine;
        explicit DepthGuard(Engine& e) : engine(e) {
            if (engine.bodyDepth_ >= kMaxBodyDepth)
                throw ExecutionError(
                    "Trigger/procedure bahut gehra chal raha hai (limit " + std::to_string(kMaxBodyDepth) +
                    ") -- shayad koi trigger khud ko baar-baar chala raha hai");
            ++engine.bodyDepth_;
        }
        ~DepthGuard() { --engine.bodyDepth_; }
    } guard(*this);

    auto statements = parseScript(bodyText);  // fresh parse every time, like Python
    for (auto& stmt : statements) {
        substituteStatementInPlace(*stmt, replace);
        Result r = executeStatement(*stmt);
        if (results) results->push_back(std::move(r));
    }
}

void Engine::fireTriggers(const std::string& timing, const std::string& event, const std::string& table,
                          const Row* newRow, const Row* oldRow) {
    // Copies (Catalog::triggersFor): a body may run DDL that changes the catalog.
    for (const auto& trigger : catalog().triggersFor(timing, event, table)) {
        RefReplacer replace = [newRow, oldRow](const ast::ColumnRef& ref) -> std::optional<Value> {
            if (ref.table && *ref.table == "naya" && newRow) {
                auto it = newRow->find(ref.name);
                if (it != newRow->end()) return it->second;
            }
            if (ref.table && *ref.table == "purana" && oldRow) {
                auto it = oldRow->find(ref.name);
                if (it != oldRow->end()) return it->second;
            }
            return std::nullopt;
        };
        runBody(trigger.bodyText, replace, nullptr);
    }
}

// ============================================================================
// stored procedures (a named, parameterised sequence of statements)
// ============================================================================

Result Engine::execCreateProcedure(const ast::CreateProcedure& stmt) {
    Catalog& cat = catalog();
    if (cat.hasProcedure(stmt.name)) throw ExecutionError("Procedure '" + stmt.name + "' pehle se hai");
    std::set<std::string> names;
    for (const auto& p : stmt.params) names.insert(p.name);
    if (names.size() != stmt.params.size())
        throw ExecutionError("Procedure '" + stmt.name + "': ek parameter naam do baar diya hai");
    parseScript(stmt.bodyText);  // sanity check: the body must parse cleanly
    std::vector<std::pair<std::string, std::string>> params;
    for (const auto& p : stmt.params) params.emplace_back(p.name, p.typeName);
    cat.addProcedure(stmt.name, params, stmt.bodyText);
    return messageResult("Procedure '" + stmt.name + "' ban gaya (" + std::to_string(stmt.params.size()) +
                         " parameter(s))");
}

Result Engine::execDropProcedure(const ast::DropProcedure& stmt) {
    Catalog& cat = catalog();
    if (!cat.hasProcedure(stmt.name)) throw ExecutionError("Procedure '" + stmt.name + "' exist nahi karta");
    cat.removeProcedure(stmt.name);
    return messageResult("Procedure '" + stmt.name + "' hata diya");
}

Result Engine::execCallProcedure(const ast::CallProcedure& stmt) {
    auto proc = catalog().findProcedure(stmt.name);
    if (!proc) throw ExecutionError("Procedure '" + stmt.name + "' exist nahi karta");
    if (stmt.args.size() != proc->params.size())
        throw ExecutionError("Procedure '" + stmt.name + "' ko " + std::to_string(proc->params.size()) +
                             " argument(s) chahiye, " + std::to_string(stmt.args.size()) + " mile");

    // Arguments are evaluated as CONSTANT expressions (no outer row at a bare CHALAO),
    // left to right, each coerced to its parameter's type.
    std::unordered_map<std::string, Value> values;
    for (size_t i = 0; i < proc->params.size(); ++i) {
        const auto& pname = proc->params[i].first;
        const auto& ptype = proc->params[i].second;
        Value raw = evaluate(*stmt.args[i], Row{});
        Value coerced = coerce(raw, ptype, pname);
        values[pname] = std::move(coerced);
    }

    // Only a BARE (unqualified) reference matching a parameter name is substituted.
    RefReplacer replace = [&values](const ast::ColumnRef& ref) -> std::optional<Value> {
        if (!ref.table) {
            auto it = values.find(ref.name);
            if (it != values.end()) return it->second;
        }
        return std::nullopt;
    };

    std::vector<Result> results;
    runBody(proc->bodyText, replace, &results);
    std::string summary;
    for (const auto& r : results) {
        if (r.message.empty()) continue;
        if (!summary.empty()) summary += "; ";
        summary += r.message;
    }
    return messageResult("Procedure '" + stmt.name + "' chal gaya (" + std::to_string(results.size()) +
                         " statement(s)): " + summary);
}

void Engine::noTransaction(const std::string& command) const {
    if (inTransaction())
        throw ExecutionError(command + " transaction ke andar nahi chal sakta -- pehle PAKKA ya WAPAS karo");
}

// ============================================================================
// databases, SHOW / DESCRIBE
// ============================================================================

Result Engine::execCreateDatabase(const ast::CreateDatabase& stmt) {
    noTransaction("BANAO DATABASE");
    fs::path path = instance_->dbDir(stmt.name);
    if (fs::is_directory(path)) throw ExecutionError("Database '" + stmt.name + "' pehle se hai");
    fs::create_directories(path);
    return messageResult("Database '" + stmt.name + "' ban gaya");
}

Result Engine::execDropDatabase(const ast::DropDatabase& stmt) {
    noTransaction("HATAO DATABASE");
    if (stmt.name == DEFAULT_DATABASE)
        throw ExecutionError(std::string("'") + DEFAULT_DATABASE + "' default database hai, use hata nahi sakte");
    fs::path path = instance_->dbDir(stmt.name);
    if (!fs::is_directory(path)) throw ExecutionError("Database '" + stmt.name + "' exist nahi karta");
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) throw StorageError("Database '" + stmt.name + "' hata nahi paaye: " + ec.message());
    instance_->forget(stmt.name);
    if (currentDb == stmt.name) currentDb = DEFAULT_DATABASE;
    return messageResult("Database '" + stmt.name + "' hata diya");
}

Result Engine::execUseDatabase(const ast::UseDatabase& stmt) {
    noTransaction("ISTEMAL");
    if (!fs::is_directory(instance_->dbDir(stmt.name)))
        throw ExecutionError("Database '" + stmt.name + "' exist nahi karta");
    currentDb = stmt.name;
    return messageResult("Ab database '" + stmt.name + "' istemal ho raha hai");
}

Result Engine::execShowTables(const ast::ShowTables&) {
    std::vector<std::string> names;
    for (const auto& entry : catalog().tables) names.push_back(entry.first);
    std::sort(names.begin(), names.end());
    Result r;
    r.columns = {"table"};
    for (auto& n : names) r.rows.push_back({textValue(n)});
    r.message = std::to_string(names.size()) + " table(s) in '" + currentDb + "'";
    return r;
}

Result Engine::execDescribe(const ast::Describe& stmt) {
    Catalog& cat = catalog();
    if (cat.find(stmt.table) == nullptr && cat.views.count(stmt.table)) {
        Result r;
        r.columns = {"definition"};
        r.rows.push_back({textValue(cat.views.at(stmt.table))});
        r.message = "VIEW '" + stmt.table + "'";
        return r;
    }
    const TableSchema& schema = cat.get(stmt.table);
    Result r;
    r.columns = {"column", "type", "constraints"};
    for (const auto& c : schema.columns) {
        std::string typeDisplay = c.maxLength.has_value() ? c.typeName + "(" + std::to_string(*c.maxLength) + ")" : c.typeName;
        std::vector<std::string> flags;
        if (c.primaryKey) flags.push_back("MUKHYA KUNJI");
        if (c.notNull) flags.push_back("ZAROORI");
        if (c.unique) flags.push_back("ANOKHA");
        if (c.defaultValue.has_value() && !c.defaultValue->isNull())
            flags.push_back("WARNA " + exprLabel(ast::Literal(*c.defaultValue)));
        if (nonEmpty(c.refTable))
            flags.push_back("SANDARBH " + *c.refTable + "(" + c.refColumn.value_or("") + ")");
        if (nonEmpty(c.check)) flags.push_back("SHART (" + *c.check + ")");
        r.rows.push_back({textValue(c.name), textValue(typeDisplay), textValue(joinStrs(flags, " "))});
    }
    for (const auto& group : schema.compositeUnique)
        r.rows.push_back({textValue("(" + joinStrs(group, ", ") + ")"), textValue(""), textValue("ANOKHA")});
    if (schema.compositePk.has_value() && !schema.compositePk->empty())
        r.rows.push_back({textValue("(" + joinStrs(*schema.compositePk, ", ") + ")"), textValue(""), textValue("MUKHYA KUNJI")});
    r.message = "Table '" + schema.name + "'";
    return r;
}

// ============================================================================
// transactions
// ============================================================================

Result Engine::execBegin(const ast::Begin&) {
    if (inTransaction()) throw ExecutionError("Transaction pehle se chal raha hai (PAKKA ya WAPAS karo)");
    instance_->takeSnapshot(currentDb);
    // Take the lock ONE EXTRA time and keep it until PAKKA/WAPAS: other
    // sessions now wait, so nobody sees our half-finished changes.
    instance_->lock.lock();
    txnThread_ = std::this_thread::get_id();
    txnDb = currentDb;
    return messageResult("Transaction SHURU. PAKKA se save karo, WAPAS se sab undo.");
}

Result Engine::execCommit(const ast::Commit&) {
    if (!inTransaction()) throw ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)");
    if (txnThread_ != std::this_thread::get_id())
        throw ExecutionError("Transaction jis thread ne SHURU kiya, PAKKA bhi wahi kar sakta hai");
    instance_->discardSnapshot(*txnDb);
    txnDb.reset();
    instance_->lock.unlock();
    return messageResult("Transaction PAKKA -- saare changes save ho gaye");
}

Result Engine::execRollback(const ast::Rollback&) {
    if (!inTransaction()) throw ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)");
    if (txnThread_ != std::this_thread::get_id())
        throw ExecutionError("Transaction jis thread ne SHURU kiya, WAPAS bhi wahi kar sakta hai");
    instance_->restoreSnapshot(*txnDb);
    txnDb.reset();
    instance_->lock.unlock();
    return messageResult("Transaction WAPAS -- saare changes undo ho gaye");
}

// ============================================================================
// DDL
// ============================================================================

std::unique_ptr<Table> Engine::table(const std::string& name) {
    Catalog& cat = catalog();
    if (cat.find(name) == nullptr && cat.views.count(name))
        throw ExecutionError("'" + name + "' ek VIEW hai, table nahi -- isme DAALO/BADLO/MITAO nahi kar sakte");
    return std::make_unique<Table>(cat.get(name), cat.tablePath(name), instance_->indexCache(currentDb, name));
}

Column Engine::makeColumn(const ast::ColumnDef& def) const {
    Column column;
    column.name = def.name;
    column.typeName = def.typeName;
    column.primaryKey = def.primaryKey;
    column.notNull = def.notNull;
    column.unique = def.unique;
    column.maxLength = def.maxLength;
    column.refTable = def.refTable;
    column.refColumn = def.refColumn;
    column.check = def.check;
    // check the WARNA value fits the column type (and VARCHAR(n) length) now, not at the first INSERT
    if (def.defaultValue.has_value() && !def.defaultValue->isNull()) {
        Value coerced = coerce(*def.defaultValue, column.typeName, column.name);
        checkLength(column, coerced);
        column.defaultValue = coerced;
    }
    return column;
}

// SANDARBH parent(column): the parent table must exist (or be the table being
// created/altered itself -- a self-reference), the parent column must exist,
// be MUKHYA KUNJI/ANOKHA, and have the same type as this column.
const TableSchema& Engine::checkFkTarget(const Column& col, const TableSchema& selfSchema) {
    const std::string refTable = col.refTable.value_or("");
    const std::string refColumn = col.refColumn.value_or("");
    const TableSchema* target = refTable == selfSchema.name ? &selfSchema : catalog().find(refTable);
    if (target == nullptr)
        throw ExecutionError("Column '" + col.name + "': SANDARBH table '" + refTable + "' exist nahi karta");
    auto names = target->columnNames();
    if (std::find(names.begin(), names.end(), refColumn) == names.end())
        throw ExecutionError("Column '" + col.name + "': SANDARBH table '" + refTable + "' mein column '" + refColumn +
                             "' nahi hai");
    const Column& refCol = target->getColumn(refColumn);
    if (!refCol.isUnique())
        throw ExecutionError("Column '" + col.name + "': SANDARBH '" + refTable + "." + refColumn +
                             "' MUKHYA KUNJI ya ANOKHA nahi hai");
    if (refCol.typeName != col.typeName)
        throw ExecutionError("Column '" + col.name + "' (" + col.typeName + ") aur SANDARBH '" + refTable + "." +
                             refColumn + "' (" + refCol.typeName + ") ke types match nahi karte");
    return *target;
}

// SHART (...) may only use plain (unqualified) columns of THIS table, and no aggregates.
void Engine::checkShartExpr(const Column& col, const TableSchema& schema) const {
    auto expr = parseExpression(*col.check);
    std::vector<const ast::FuncCall*> aggs;
    findAggregates(*expr, aggs);
    if (!aggs.empty())
        throw ExecutionError("Column '" + col.name + "': SHART mein aggregate function ('" + aggs[0]->name +
                             "') nahi chal sakta");
    std::vector<const ast::ColumnRef*> refs;
    columnRefNodes(*expr, refs);
    auto names = schema.columnNames();
    for (const auto* ref : refs) {
        if (ref->table.has_value())
            throw ExecutionError("Column '" + col.name + "': SHART mein sirf column ka naam likho, 'table.column' nahi");
        if (std::find(names.begin(), names.end(), ref->name) == names.end())
            throw ExecutionError("Column '" + col.name + "': SHART mein column '" + ref->name + "' table '" +
                                 schema.name + "' mein nahi hai");
    }
}

Result Engine::execCreateTable(const ast::CreateTable& stmt) {
    Catalog& cat = catalog();
    if (cat.find(stmt.name)) throw ExecutionError("Table '" + stmt.name + "' pehle se hai");
    if (cat.views.count(stmt.name))
        throw ExecutionError("'" + stmt.name + "' ek VIEW hai -- table usi naam se nahi ban sakti");
    std::vector<std::string> names;
    for (const auto& c : stmt.columns) names.push_back(c.name);
    std::set<std::string> dupes;
    for (const auto& n : names)
        if (std::count(names.begin(), names.end(), n) > 1) dupes.insert(n);
    if (!dupes.empty()) {
        std::vector<std::string> sorted(dupes.begin(), dupes.end());
        throw ExecutionError("Column naam do baar diya: " + joinStrs(sorted, ", "));
    }
    auto pkCount = std::count_if(stmt.columns.begin(), stmt.columns.end(), [](const ast::ColumnDef& c) { return c.primaryKey; });
    if (pkCount > 1) throw ExecutionError("Ek table mein sirf ek MUKHYA KUNJI ho sakti hai");
    bool hasCompositePk = stmt.compositePk.has_value() && !stmt.compositePk->empty();
    if (stmt.compositePk.has_value() && pkCount > 0)
        throw ExecutionError("Ek table mein sirf ek MUKHYA KUNJI ho sakti hai (single- ya multi-column, dono nahi)");
    {
        auto groups = stmt.compositeUnique;
        if (hasCompositePk) groups.push_back(*stmt.compositePk);
        checkCompositeColumns(stmt.name, names, groups);
    }

    TableSchema schema;
    schema.name = stmt.name;
    for (const auto& c : stmt.columns) schema.columns.push_back(makeColumn(c));
    // a composite PRIMARY KEY's columns must all be NOT NULL, exactly like a
    // normal (single-column) PK -- mark this ONCE, here
    if (hasCompositePk) {
        for (const auto& name : *stmt.compositePk)
            for (auto& c : schema.columns)
                if (c.name == name) c.notNull = true;
    }
    schema.compositeUnique = stmt.compositeUnique;
    if (hasCompositePk) schema.compositePk = *stmt.compositePk;
    for (const auto& col : schema.columns) {
        if (nonEmpty(col.refTable)) checkFkTarget(col, schema);
        if (nonEmpty(col.check)) checkShartExpr(col, schema);
    }
    HeapFile(cat.tablePath(stmt.name)).create();
    cat.add(schema);
    return messageResult("Table '" + stmt.name + "' ban gaya (" + std::to_string(schema.columns.size()) + " columns)");
}

Result Engine::execDropTable(const ast::DropTable& stmt) {
    Catalog& cat = catalog();
    std::vector<std::string> children;
    for (const auto& [otherName, other] : cat.tables) {
        if (otherName == stmt.name) continue;
        for (const auto& c : other.columns)
            if (c.refTable.has_value() && *c.refTable == stmt.name) {
                children.push_back(otherName);
                break;
            }
    }
    std::sort(children.begin(), children.end());
    if (!children.empty())
        throw ExecutionError("Table '" + stmt.name + "' hata nahi sakte -- " + joinStrs(children, ", ") +
                             " ise SANDARBH karte hain");
    auto t = table(stmt.name);
    t->heap().destroy();
    t->invalidateIndexes();
    instance_->dropIndexCache(currentDb, stmt.name);
    cat.remove(stmt.name);
    return messageResult("Table '" + stmt.name + "' hata diya");
}

Result Engine::execTruncateTable(const ast::TruncateTable& stmt) {
    auto t = table(stmt.name);
    const TableSchema schema = t->schema();
    Catalog& cat = catalog();
    std::vector<std::string> otherNames;
    for (const auto& entry : cat.tables) otherNames.push_back(entry.first);
    for (const auto& otherName : otherNames) {
        const TableSchema other = cat.get(otherName);
        for (const auto& childCol : other.columns) {
            if (!childCol.refTable.has_value() || *childCol.refTable != schema.name) continue;
            size_t childPos = other.indexOf(childCol.name);
            for (const auto& [id, values] : table(otherName)->rows()) {
                (void)id;
                if (!values[childPos].isNull())
                    throw ExecutionError("Table '" + schema.name + "' SAAF nahi kar sakte -- table '" + otherName +
                                         "' ka column '" + childCol.name + "' (SANDARBH) abhi bhi values use karta hai");
            }
        }
    }
    size_t count = t->rows().size();
    t->heap().truncate();
    t->invalidateIndexes();
    return messageResult("Table '" + stmt.name + "' saaf -- " + std::to_string(count) + " row(s) hataye");
}

Result Engine::execCompactTable(const ast::CompactTable& stmt) {
    auto t = table(stmt.name);
    auto before = static_cast<int64_t>(fs::file_size(t->heap().path()));
    t->heap().compact();
    t->invalidateIndexes();  // compaction moves rows, so every row id changed
    auto after = static_cast<int64_t>(fs::file_size(t->heap().path()));
    return messageResult("Table '" + stmt.name + "' sikod diya: " + std::to_string(before) + " -> " +
                         std::to_string(after) + " bytes (" + std::to_string(before - after) + " bytes bache)");
}

Result Engine::execAlterAddColumn(const ast::AlterAddColumn& stmt) {
    auto t = table(stmt.table);
    const TableSchema schema = t->schema();
    Column newCol = makeColumn(stmt.column);
    auto names = schema.columnNames();
    if (std::find(names.begin(), names.end(), newCol.name) != names.end())
        throw ExecutionError("Column '" + newCol.name + "' pehle se hai");
    if (newCol.primaryKey &&
        std::any_of(schema.columns.begin(), schema.columns.end(), [](const Column& c) { return c.primaryKey; }))
        throw ExecutionError("Table mein pehle se MUKHYA KUNJI hai");

    std::vector<RowValues> oldRows;
    for (auto& [id, values] : t->rows()) {
        (void)id;
        oldRows.push_back(std::move(values));
    }
    bool hasDefault = newCol.defaultValue.has_value() && !newCol.defaultValue->isNull();
    Value defaultValue = hasDefault ? *newCol.defaultValue : Value();
    if (oldRows.size() > 1 && newCol.isUnique() && hasDefault)
        // every existing row would get the same WARNA value -> duplicates
        throw ExecutionError("Naya ANOKHA column '" + newCol.name + "' sab rows mein ek hi WARNA value nahi le sakta");
    if (!oldRows.empty() && newCol.isRequired() && !hasDefault)
        // every existing row would get KHALI in the new column, breaking ZAROORI
        throw ExecutionError("Table khali nahi hai; naya ZAROORI column '" + newCol.name + "' ko WARNA value chahiye");

    if (nonEmpty(newCol.refTable)) {
        const TableSchema& parent = checkFkTarget(newCol, schema);
        if (hasDefault) {
            std::unique_ptr<Table> parentOwned;
            Table* parentTable = t.get();
            if (*newCol.refTable != schema.name) {
                parentOwned = table(*newCol.refTable);
                parentTable = parentOwned.get();
            }
            size_t parentPos = parent.indexOf(newCol.refColumn.value_or(""));
            IndexMap& idx = parentTable->indexes();
            auto it = idx.find(std::vector<size_t>{parentPos});
            if (it == idx.end() || it->second.find(std::vector<Value>{defaultValue}) == it->second.end())
                throw ExecutionError("Column '" + newCol.name + "': WARNA value " + formatValue(defaultValue) +
                                     " table '" + *newCol.refTable + "' ke column '" + newCol.refColumn.value_or("") +
                                     "' mein nahi mila (SANDARBH)");
        }
    }

    // NOTE (mirrors Python): the new schema is built from the name and columns
    // only, so table-level composite constraints are not carried over.
    TableSchema newSchema;
    newSchema.name = schema.name;
    newSchema.columns = schema.columns;
    newSchema.columns.push_back(newCol);
    if (nonEmpty(newCol.check)) {
        checkShartExpr(newCol, newSchema);
        auto expr = parseExpression(*newCol.check);
        auto newNames = newSchema.columnNames();
        for (const auto& v : oldRows) {
            Row row;
            for (size_t i = 0; i < newNames.size(); ++i) row[newNames[i]] = i < v.size() ? v[i] : defaultValue;
            Value result = evaluate(*expr, row);
            if (isBoolValue(result) && !std::get<bool>(result.data))
                throw ExecutionError("SHART toot gayi: (" + *newCol.check + ") -- column '" + newCol.name +
                                     "' (table ki maujooda rows ke liye, WARNA value ke saath)");
        }
    }

    // existing rows get the WARNA value (or KHALI if there is none)
    auto types = newSchema.types();
    std::vector<std::vector<uint8_t>> payloads;
    for (auto v : oldRows) {
        v.push_back(defaultValue);
        payloads.push_back(encodeRow(v, types));
    }
    t->heap().rewrite(payloads);
    t->invalidateIndexes();
    catalog().add(newSchema);
    return messageResult("Column '" + newCol.name + "' '" + schema.name + "' mein jod diya");
}

Result Engine::execAlterAddComposite(const ast::AlterAddComposite& stmt) {
    auto t = table(stmt.table);
    // Work on a copy; the catalog only sees it once the existing data has
    // passed the new constraint.
    TableSchema schema = t->schema();
    checkCompositeColumns(stmt.table, schema.columnNames(), {stmt.columns});
    if (stmt.kind == "MUKHYA") {
        if (schema.compositePk.has_value() ||
            std::any_of(schema.columns.begin(), schema.columns.end(), [](const Column& c) { return c.primaryKey; }))
            throw ExecutionError("Ek table mein sirf ek MUKHYA KUNJI ho sakti hai");
        for (const auto& name : stmt.columns)
            for (auto& c : schema.columns)
                if (c.name == name) c.notNull = true;
        schema.compositePk = stmt.columns;
    } else {
        schema.compositeUnique.push_back(stmt.columns);
    }

    // existing data must already satisfy the new constraint
    t->invalidateIndexes();
    std::vector<size_t> positions;
    for (const auto& c : stmt.columns) positions.push_back(schema.indexOf(c));
    std::unordered_set<std::vector<Value>, ValueVecHash, ValueVecEq> seen;
    for (const auto& [id, values] : t->rows()) {
        (void)id;
        std::vector<Value> key;
        bool hasNull = false;
        for (size_t p : positions) {
            key.push_back(values[p]);
            if (values[p].isNull()) hasNull = true;
        }
        if (stmt.kind == "MUKHYA" && hasNull)
            throw ExecutionError("Table '" + stmt.table + "' mein columns " + pyReprStrList(stmt.columns) +
                                 " ki maujooda rows mein KHALI hai -- MUKHYA KUNJI ke liye ZAROORI (NOT NULL) chahiye");
        if (hasNull) continue;
        if (seen.count(key))
            throw ExecutionError("Duplicate value " + pyReprTuple(key) + " columns " + pyReprStrList(stmt.columns) +
                                 " mein -- maujooda data ye constraint todta hai");
        seen.insert(key);
    }

    catalog().add(schema);
    std::string kindLabel = stmt.kind == "MUKHYA" ? "MUKHYA KUNJI" : "ANOKHA";
    return messageResult("Table '" + stmt.table + "' mein " + kindLabel + " (" + joinStrs(stmt.columns, ", ") + ") jod diya");
}

Result Engine::execAlterDropColumn(const ast::AlterDropColumn& stmt) {
    auto t = table(stmt.table);
    const TableSchema schema = t->schema();
    size_t idx = schema.indexOf(stmt.column);
    if (schema.columns.size() == 1)
        throw ExecutionError("Table ka aakhri column nahi hata sakte -- HATAO TABLE use karo");

    std::vector<std::string> referencedBy;
    for (const auto& [otherName, other] : catalog().tables)
        for (const auto& c : other.columns)
            if (c.refTable.has_value() && *c.refTable == schema.name && c.refColumn.has_value() && *c.refColumn == stmt.column)
                referencedBy.push_back(otherName + "." + c.name);
    std::sort(referencedBy.begin(), referencedBy.end());
    if (!referencedBy.empty())
        throw ExecutionError("Column '" + stmt.column + "' hata nahi sakte -- " + joinStrs(referencedBy, ", ") +
                             " (SANDARBH) ise use karte hain");
    std::vector<std::string> usedByShart;
    for (const auto& c : schema.columns) {
        if (!nonEmpty(c.check)) continue;
        auto expr = parseExpression(*c.check);
        auto refs = columnRefKeys(expr.get());
        if (std::find(refs.begin(), refs.end(), stmt.column) != refs.end()) usedByShart.push_back(c.name);
    }
    std::sort(usedByShart.begin(), usedByShart.end());
    if (!usedByShart.empty())
        throw ExecutionError("Column '" + stmt.column + "' hata nahi sakte -- column " + joinStrs(usedByShart, ", ") +
                             " ka SHART ise use karta hai");

    // NOTE (mirrors Python): composite constraints are not carried over.
    TableSchema newSchema;
    newSchema.name = schema.name;
    newSchema.columns = schema.columns;
    newSchema.columns.erase(newSchema.columns.begin() + static_cast<std::ptrdiff_t>(idx));
    auto types = newSchema.types();
    std::vector<std::vector<uint8_t>> payloads;
    for (auto& [id, v] : t->rows()) {
        (void)id;
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx));
        payloads.push_back(encodeRow(v, types));
    }
    t->heap().rewrite(payloads);
    t->invalidateIndexes();
    catalog().add(newSchema);
    return messageResult("Column '" + stmt.column + "' '" + schema.name + "' se hata diya");
}

Result Engine::execRenameTable(const ast::RenameTable& stmt) {
    Catalog& cat = catalog();
    TableSchema schema = cat.get(stmt.table);  // raises if it doesn't exist
    if (stmt.newName == stmt.table) throw ExecutionError("Naya naam purane naam '" + stmt.table + "' jaisa hi hai");
    if (cat.find(stmt.newName) != nullptr) throw ExecutionError("Table '" + stmt.newName + "' pehle se hai");

    // the row DATA doesn't change, only the file's name -- a rename is atomic
    std::error_code ec;
    fs::rename(cat.tablePath(stmt.table), cat.tablePath(stmt.newName), ec);
    if (ec) throw StorageError("Table file ka naam badal nahi paaye: " + ec.message());
    cat.tables.erase(stmt.table);
    schema.name = stmt.newName;
    cat.tables[stmt.newName] = schema;
    // every SANDARBH (FK) pointing at the old name must follow it, INCLUDING a self-reference
    for (auto& entry : cat.tables)
        for (auto& col : entry.second.columns)
            if (col.refTable.has_value() && *col.refTable == stmt.table) col.refTable = stmt.newName;
    cat.save();
    // row ids didn't change, but the CACHE KEY (db, table) did
    instance_->dropIndexCache(currentDb, stmt.table);
    return messageResult("Table '" + stmt.table + "' ka naam ab '" + stmt.newName + "' hai");
}

Result Engine::execRenameColumn(const ast::RenameColumn& stmt) {
    Catalog& cat = catalog();
    TableSchema& schema = cat.get(stmt.table);
    size_t idx = schema.indexOf(stmt.column);  // raises if the column doesn't exist
    if (stmt.newName == stmt.column) throw ExecutionError("Naya naam purane naam '" + stmt.column + "' jaisa hi hai");
    auto names = schema.columnNames();
    if (std::find(names.begin(), names.end(), stmt.newName) != names.end())
        throw ExecutionError("Column '" + stmt.newName + "' pehle se hai");

    // A SHART's catalog entry is only SOURCE TEXT, not an AST -- so it can't
    // be "renamed" in place. Refuse instead of leaving a stale CHECK.
    for (const auto& col : schema.columns) {
        if (!nonEmpty(col.check)) continue;
        auto expr = parseExpression(*col.check);
        auto refs = columnRefKeys(expr.get());
        if (std::find(refs.begin(), refs.end(), stmt.column) != refs.end())
            throw ExecutionError("Column '" + stmt.column + "' rename nahi kar sakte -- column '" + col.name +
                                 "' ka SHART '(" + *col.check + ")' ise use karta hai. Pehle wo SHART hatao (SHART "
                                 "text round-trip nahi hoti -- dekho docs/LANGUAGE.md)");
    }

    schema.columns[idx].name = stmt.newName;
    // every SANDARBH (FK) pointing at this column must follow it
    for (auto& entry : cat.tables)
        for (auto& col : entry.second.columns)
            if (col.refTable.has_value() && *col.refTable == stmt.table && col.refColumn.has_value() &&
                *col.refColumn == stmt.column)
                col.refColumn = stmt.newName;
    cat.save();
    return messageResult("Column '" + stmt.column + "' ka naam ab '" + stmt.newName + "' hai");
}

// ============================================================================
// constraint checks
// ============================================================================

namespace {

// SHART (CHECK), SQL semantics: the row is rejected ONLY when the expression
// is exactly JHOOTH. KHALI (unknown, e.g. a column it uses is KHALI) passes.
void checkShart(const TableSchema& schema, const std::vector<Value>& values) {
    Row row;
    for (size_t i = 0; i < schema.columns.size() && i < values.size(); ++i) row[schema.columns[i].name] = values[i];
    for (const auto& col : schema.columns) {
        if (!nonEmpty(col.check)) continue;
        auto expr = parseExpression(*col.check);
        Value result = evaluate(*expr, row);
        if (isBoolValue(result) && !std::get<bool>(result.data))
            throw ExecutionError("SHART toot gayi: (" + *col.check + ") -- column '" + col.name + "'");
    }
}

// Is this index a plain single-column one (Python: int key) rather than a
// composite constraint (Python: tuple key)?
bool isSingleIndex(const TableSchema& schema, const std::vector<size_t>& positions) {
    return positions.size() == 1 && schema.columns[positions[0]].isUnique();
}

// The index key for `positions`, or nullopt when any part is KHALI.
std::optional<std::vector<Value>> indexKey(const std::vector<size_t>& positions, const std::vector<Value>& values) {
    std::vector<Value> key;
    key.reserve(positions.size());
    for (size_t p : positions) {
        if (values[p].isNull()) return std::nullopt;
        key.push_back(values[p]);
    }
    return key;
}

}  // namespace

// Type-check each value and enforce ZAROORI / MUKHYA KUNJI (not null) / VARCHAR(n) / SHART.
std::vector<Value> Engine::validateRow(const TableSchema& schema, const std::vector<Value>& values) const {
    std::vector<Value> out;
    for (size_t i = 0; i < schema.columns.size() && i < values.size(); ++i) {
        const Column& col = schema.columns[i];
        Value value = coerce(values[i], col.typeName, col.name);
        if (value.isNull() && col.isRequired())
            throw ExecutionError("Column '" + col.name + "' ZAROORI hai, KHALI nahi ho sakta");
        checkLength(col, value);
        out.push_back(std::move(value));
    }
    checkShart(schema, out);
    return out;
}

// Enforce ANOKHA / MUKHYA KUNJI (single-column AND composite) using the hash
// indexes. `ignoreRowIds` are rows being replaced (UPDATE), so their old
// values don't count.
void Engine::checkUnique(Table& table, const std::vector<std::vector<Value>>& newRows,
                         const std::set<int64_t>& ignoreRowIds) {
    const TableSchema schema = table.schema();
    IndexMap& indexes = table.indexes();
    using KeySet = std::unordered_set<std::vector<Value>, ValueVecHash, ValueVecEq>;
    std::vector<KeySet> seen(indexes.size());  // values within this statement, one set per index
    for (const auto& values : newRows) {
        size_t k = 0;
        for (auto& [positions, index] : indexes) {
            KeySet& seenHere = seen[k++];
            auto key = indexKey(positions, values);
            if (!key) continue;  // KHALI never participates in a uniqueness violation
            auto it = index.find(*key);
            bool clash = (it != index.end() && !ignoreRowIds.count(it->second)) || seenHere.count(*key);
            if (clash) {
                if (isSingleIndex(schema, positions))
                    throw ExecutionError("Duplicate value " + pyReprValue((*key)[0]) + " column '" +
                                         schema.columns[positions[0]].name +
                                         "' mein -- is column mein har value alag honi chahiye");
                std::vector<std::string> names;
                for (size_t p : positions) names.push_back(schema.columns[p].name);
                throw ExecutionError("Duplicate value " + pyReprTuple(*key) + " columns " + pyReprStrList(names) +
                                     " mein -- ye combination alag hona chahiye");
            }
            seenHere.insert(*key);
        }
    }
}

// SANDARBH (FOREIGN KEY), child side: every non-KHALI value must already exist
// in the parent's column. For a self-reference, a value is also accepted if it
// appears among the OTHER rows of this very statement.
void Engine::checkFk(Table& table, const std::vector<std::vector<Value>>& newRows) {
    const TableSchema schema = table.schema();
    for (size_t pos = 0; pos < schema.columns.size(); ++pos) {
        const Column& col = schema.columns[pos];
        if (!nonEmpty(col.refTable)) continue;
        bool isSelf = *col.refTable == schema.name;
        std::unique_ptr<Table> parentOwned;
        Table* parent = &table;
        if (!isSelf) {
            parentOwned = this->table(*col.refTable);
            parent = parentOwned.get();
        }
        size_t parentPos = parent->schema().indexOf(col.refColumn.value_or(""));
        IndexMap& parentIndexes = parent->indexes();
        auto idxIt = parentIndexes.find(std::vector<size_t>{parentPos});
        const HashIndex* parentIndex = idxIt == parentIndexes.end() ? nullptr : &idxIt->second;
        ValueSet localValues;
        if (isSelf)
            for (const auto& r : newRows)
                if (!r[parentPos].isNull()) localValues.insert(r[parentPos]);
        for (const auto& row : newRows) {
            const Value& v = row[pos];
            if (v.isNull()) continue;
            if (parentIndex && parentIndex->count(std::vector<Value>{v})) continue;
            if (localValues.count(v)) continue;
            throw ExecutionError("Column '" + col.name + "': value " + formatValue(v) + " table '" + *col.refTable +
                                 "' ke column '" + col.refColumn.value_or("") + "' mein nahi mila (SANDARBH)");
        }
    }
}

// SANDARBH, parent side: RESTRICT. Refuse a DELETE/UPDATE on `schema` if any
// child row still points at a value that is disappearing from it.
//   changedByColumn: {parent column position -> values going away}
//   exemptRowIds (DELETE): rows of `schema` that are themselves being removed
//   overrides (UPDATE): {row id -> new values} for rows this statement also updates
void Engine::checkNoChildren(const TableSchema& schema, const std::unordered_map<size_t, ValueSet>& changedByColumn,
                             const std::set<int64_t>& exemptRowIds,
                             const std::unordered_map<int64_t, std::vector<Value>>* overrides) {
    std::vector<std::string> otherNames;
    for (const auto& entry : catalog().tables) otherNames.push_back(entry.first);
    for (const auto& otherName : otherNames) {
        const TableSchema other = catalog().get(otherName);
        for (const auto& childCol : other.columns) {
            if (!childCol.refTable.has_value() || *childCol.refTable != schema.name) continue;
            size_t parentPos = schema.indexOf(childCol.refColumn.value_or(""));
            auto removedIt = changedByColumn.find(parentPos);
            if (removedIt == changedByColumn.end() || removedIt->second.empty()) continue;
            const ValueSet& removed = removedIt->second;
            bool isSelf = otherName == schema.name;
            size_t childPos = other.indexOf(childCol.name);
            for (const auto& [rowId, values] : table(otherName)->rows()) {
                if (isSelf && exemptRowIds.count(rowId)) continue;
                const std::vector<Value>* current = &values;
                if (isSelf && overrides) {
                    auto ov = overrides->find(rowId);
                    if (ov != overrides->end()) current = &ov->second;
                }
                const Value& v = (*current)[childPos];
                if (!v.isNull() && removed.count(v))
                    throw ExecutionError("Table '" + schema.name + "' mein ye value(s) hata/badal nahi sakte -- table '" +
                                         otherName + "' ka column '" + childCol.name + "' (SANDARBH " + schema.name +
                                         "." + childCol.refColumn.value_or("") + ") abhi bhi inhe use karta hai");
            }
        }
    }
}

// Does `values` collide with an EXISTING row on any unique/PK column (single
// or composite)? Returns that row's id. The first matching index wins.
std::optional<int64_t> Engine::findConflict(Table& table, const std::vector<Value>& values) {
    for (auto& [positions, index] : table.indexes()) {
        auto key = indexKey(positions, values);
        if (!key) continue;
        auto it = index.find(*key);
        if (it != index.end()) return it->second;
    }
    return std::nullopt;
}

// The rows worth looking at: one index lookup, or every row (full scan).
std::vector<StoredRow> Engine::candidates(Table& table, const std::optional<IndexLookup>& access) {
    if (access.has_value()) return table.lookup(access->column, access->value);
    return table.rows();
}

// ============================================================================
// DML
// ============================================================================

Result Engine::execInsert(const ast::Insert& stmt) {
    auto t = table(stmt.table);
    const TableSchema schema = t->schema();
    std::vector<std::string> targetCols =
        stmt.columns.has_value() && !stmt.columns->empty() ? *stmt.columns : schema.columnNames();
    for (const auto& col : targetCols) schema.indexOf(col);  // raises if the column doesn't exist
    if (std::set<std::string>(targetCols.begin(), targetCols.end()).size() != targetCols.size())
        throw ExecutionError("Ek column do baar diya hai");

    auto defaults = [&]() {
        std::vector<Value> values;
        for (const auto& c : schema.columns) values.push_back(c.defaultValue.value_or(Value()));  // WARNA values (KHALI if none)
        return values;
    };

    // Validate EVERY row before writing ANY -- so a bad 3rd row doesn't leave
    // rows 1 and 2 half-inserted.
    std::vector<std::vector<Value>> newRows;
    if (stmt.select) {
        Result selectResult = execSelect(*stmt.select);
        if (selectResult.columns.size() != targetCols.size())
            throw ExecutionError(std::to_string(targetCols.size()) + " values chahiye thi, DIKHAO ne " +
                                 std::to_string(selectResult.columns.size()) + " columns di");
        for (const auto& row : selectResult.rows) {
            auto values = defaults();
            for (size_t i = 0; i < targetCols.size(); ++i) values[schema.indexOf(targetCols[i])] = row[i];
            newRows.push_back(validateRow(schema, values));
        }
    } else {
        for (const auto& tuple : stmt.rows) {
            if (tuple.size() != targetCols.size())
                throw ExecutionError(std::to_string(targetCols.size()) + " values chahiye thi, " +
                                     std::to_string(tuple.size()) + " mili");
            auto values = defaults();
            for (size_t i = 0; i < targetCols.size(); ++i) values[schema.indexOf(targetCols[i])] = evaluate(*tuple[i], Row{});
            newRows.push_back(validateRow(schema, values));
        }
    }

    if (!stmt.onConflictUpdate.has_value()) {
        checkUnique(*t, newRows);
        checkFk(*t, newRows);
        std::vector<Row> newDicts;
        for (const auto& r : newRows) newDicts.push_back(rowDict(schema, r));
        for (const auto& nr : newDicts) fireTriggers("PEHLE", "DAALO", stmt.table, &nr, nullptr);
        t->insertMany(newRows);
        for (const auto& nr : newDicts) fireTriggers("BAAD", "DAALO", stmt.table, &nr, nullptr);
        return messageResult(std::to_string(newRows.size()) + " row(s) daal di");
    }

    // TAKRAAV PAR BADLO (simplified upsert): rows colliding with an EXISTING
    // row (by any unique/PK column, single or composite) get UPDATEd instead
    // of inserted. A collision against another row IN THIS SAME BATCH is still
    // a hard error -- only pre-existing rows are rescued.
    std::vector<std::vector<Value>> toInsert;
    std::vector<std::pair<int64_t, std::vector<Value>>> toUpdate;
    for (auto& row : newRows) {
        auto existing = findConflict(*t, row);
        if (!existing) toInsert.push_back(row);
        else toUpdate.emplace_back(*existing, row);
    }
    checkUnique(*t, toInsert);
    checkFk(*t, toInsert);
    std::vector<Row> insertDicts;
    for (const auto& r : toInsert) insertDicts.push_back(rowDict(schema, r));
    for (const auto& nr : insertDicts) fireTriggers("PEHLE", "DAALO", stmt.table, &nr, nullptr);

    std::vector<StoredRow> updatedTargets;
    std::vector<std::vector<Value>> updatedNewRows;
    std::vector<std::pair<size_t, const ast::Expr*>> assignments;
    for (const auto& [col, expr] : *stmt.onConflictUpdate) assignments.emplace_back(schema.indexOf(col), expr.get());
    for (const auto& [rowId, attempted] : toUpdate) {
        auto oldValues = t->get(rowId);
        if (!oldValues) throw ExecutionError("Row #" + std::to_string(rowId) + " nahi mili");
        // assignments see the ATTEMPTED (incoming) row's values, not the
        // existing row's -- so `TAKRAAV PAR BADLO naam = naam` means "keep inserting naam"
        Row env;
        for (size_t i = 0; i < schema.columns.size(); ++i) env[schema.columns[i].name] = attempted[i];
        auto newValues = *oldValues;
        for (const auto& [position, expr] : assignments) newValues[position] = evaluate(*expr, env);
        updatedTargets.emplace_back(rowId, *oldValues);
        updatedNewRows.push_back(validateRow(schema, newValues));
    }
    if (!updatedNewRows.empty()) {
        std::set<int64_t> ignore;
        for (const auto& [rowId, oldValues] : updatedTargets) {
            (void)oldValues;
            ignore.insert(rowId);
        }
        checkUnique(*t, updatedNewRows, ignore);
        checkFk(*t, updatedNewRows);
        // a TAKRAAV collision is really an UPDATE of an existing row, so it fires
        // BADLO triggers (not DAALO) -- matches real upsert semantics
        std::vector<Row> oldDicts, updatedDicts;
        for (const auto& target : updatedTargets) oldDicts.push_back(rowDict(schema, target.second));
        for (const auto& nv : updatedNewRows) updatedDicts.push_back(rowDict(schema, nv));
        for (size_t i = 0; i < oldDicts.size(); ++i)
            fireTriggers("PEHLE", "BADLO", stmt.table, &updatedDicts[i], &oldDicts[i]);
        t->deleteMany(updatedTargets);
        t->insertMany(updatedNewRows);
        for (size_t i = 0; i < oldDicts.size(); ++i)
            fireTriggers("BAAD", "BADLO", stmt.table, &updatedDicts[i], &oldDicts[i]);
    }
    t->insertMany(toInsert);
    for (const auto& nr : insertDicts) fireTriggers("BAAD", "DAALO", stmt.table, &nr, nullptr);
    return messageResult(std::to_string(toInsert.size()) + " row(s) daali, " + std::to_string(updatedNewRows.size()) +
                         " row(s) TAKRAAV par badli");
}

Result Engine::execUpdate(const ast::Update& stmt) {
    auto t = table(stmt.table);
    const TableSchema schema = t->schema();
    Scope scope({{stmt.table, schema}});
    std::vector<std::pair<size_t, std::unique_ptr<ast::Expr>>> assignments;
    for (const auto& [col, expr] : stmt.assignments) assignments.emplace_back(schema.indexOf(col), bind(*expr, scope));
    auto where = bind(stmt.where.get(), scope);
    auto outerKeys = scope.allKeys();

    // Collect matching rows FIRST, then modify. If we updated while scanning,
    // the re-inserted rows (appended at the end of the file) would be scanned
    // again and updated twice -- the famous "Halloween problem".
    auto cands = candidates(*t, chooseAccess(*t, scope, where.get()));
    std::vector<StoredRow> targets;
    if (where) {
        std::vector<Row> candidateRows;
        for (const auto& c : cands) candidateRows.push_back(scope.row(0, c.second));
        auto subq = precomputeSubqueries({where.get()}, candidateRows, outerKeys);
        for (size_t i = 0; i < cands.size(); ++i)
            if (isTrue(evaluate(*where, candidateRows[i], subq.at(i)))) targets.push_back(cands[i]);
    } else {
        targets = cands;
    }

    std::vector<Row> oldRows;  // SET expressions see the OLD values
    for (const auto& target : targets) oldRows.push_back(scope.row(0, target.second));
    std::vector<const ast::Expr*> assignExprs;
    for (const auto& a : assignments) assignExprs.push_back(a.second.get());
    auto setSubq = precomputeSubqueries(assignExprs, oldRows, outerKeys);
    std::vector<std::vector<Value>> newRows;
    for (size_t i = 0; i < targets.size(); ++i) {
        auto newValues = targets[i].second;
        for (const auto& [position, expr] : assignments) newValues[position] = evaluate(*expr, oldRows[i], setSubq.at(i));
        newRows.push_back(validateRow(schema, newValues));
    }

    std::set<int64_t> ignore;
    for (const auto& target : targets) ignore.insert(target.first);
    checkUnique(*t, newRows, ignore);
    checkFk(*t, newRows);  // child side: new FK values must exist in the parent

    // parent side (RESTRICT): if a referenced column's value is CHANGING, no
    // child row may still be pointing at the value that is disappearing
    std::unordered_map<size_t, ValueSet> changedByColumn;
    for (size_t i = 0; i < targets.size(); ++i)
        for (size_t pos = 0; pos < schema.columns.size(); ++pos) {
            const Value& oldValue = targets[i].second[pos];
            if (!oldValue.isNull() && !pyEquals(oldValue, newRows[i][pos])) changedByColumn[pos].insert(oldValue);
        }
    if (!changedByColumn.empty()) {
        std::unordered_map<int64_t, std::vector<Value>> overrides;
        for (size_t i = 0; i < targets.size(); ++i) overrides[targets[i].first] = newRows[i];
        checkNoChildren(schema, changedByColumn, {}, &overrides);
    }

    // An update = delete old versions + insert new versions. All deletes happen
    // first, so an index entry moved from one row to another (e.g. swapping two
    // ids) is never removed by mistake.
    // Plain {column: value} dicts for trigger NAYA/PURANA substitution --
    // independent of Scope's "table.col" aliasing, which triggers don't use.
    std::vector<Row> oldDicts, newDicts;
    for (const auto& target : targets) oldDicts.push_back(rowDict(schema, target.second));
    for (const auto& nr : newRows) newDicts.push_back(rowDict(schema, nr));
    for (size_t i = 0; i < oldDicts.size(); ++i) fireTriggers("PEHLE", "BADLO", stmt.table, &newDicts[i], &oldDicts[i]);
    t->deleteMany(targets);
    t->insertMany(newRows);
    for (size_t i = 0; i < oldDicts.size(); ++i) fireTriggers("BAAD", "BADLO", stmt.table, &newDicts[i], &oldDicts[i]);
    return messageResult(std::to_string(newRows.size()) + " row(s) badal di");
}

Result Engine::execDelete(const ast::Delete& stmt) {
    auto t = table(stmt.table);
    const TableSchema schema = t->schema();
    Scope scope({{stmt.table, schema}});
    auto where = bind(stmt.where.get(), scope);

    auto cands = candidates(*t, chooseAccess(*t, scope, where.get()));
    std::vector<StoredRow> doomed;
    if (where) {
        std::vector<Row> candidateRows;
        for (const auto& c : cands) candidateRows.push_back(scope.row(0, c.second));
        auto subq = precomputeSubqueries({where.get()}, candidateRows, scope.allKeys());
        for (size_t i = 0; i < cands.size(); ++i)
            if (isTrue(evaluate(*where, candidateRows[i], subq.at(i)))) doomed.push_back(cands[i]);
    } else {
        doomed = cands;
    }

    // RESTRICT: refuse if any child row still references a value about to be deleted
    std::unordered_map<size_t, ValueSet> deletedByColumn;
    std::set<int64_t> exempt;
    for (const auto& [rowId, values] : doomed) {
        exempt.insert(rowId);
        for (size_t pos = 0; pos < schema.columns.size(); ++pos)
            if (!values[pos].isNull()) deletedByColumn[pos].insert(values[pos]);
    }
    if (!deletedByColumn.empty()) checkNoChildren(schema, deletedByColumn, exempt);

    std::vector<Row> oldDicts;
    for (const auto& d : doomed) oldDicts.push_back(rowDict(schema, d.second));
    for (const auto& od : oldDicts) fireTriggers("PEHLE", "MITAO", stmt.table, nullptr, &od);
    t->deleteMany(doomed);
    for (const auto& od : oldDicts) fireTriggers("BAAD", "MITAO", stmt.table, nullptr, &od);
    return messageResult(std::to_string(doomed.size()) + " row(s) mita di");
}

// ============================================================================
// SELECT
// ============================================================================

// Everything the planner decided about one DIKHAO (also what SAMJHAO prints).
struct Engine::SelectPlan {
    struct JoinStep {
        const ast::Join* join;
        std::unique_ptr<ast::Expr> on;                                   // bound PAR
        std::optional<std::pair<std::string, std::string>> hashKeys;     // (left key, right key) or nullopt
    };
    std::vector<std::unique_ptr<Table>> tables;
    std::unique_ptr<Scope> scope;
    std::vector<std::string> labels;
    std::vector<std::unique_ptr<ast::Expr>> outputs;
    std::unique_ptr<ast::Expr> where;
    std::vector<std::unique_ptr<ast::Expr>> groupBy;
    std::unique_ptr<ast::Expr> having;
    std::vector<std::pair<std::unique_ptr<ast::Expr>, bool>> orderBy;  // (bound expr, descending)
    std::vector<const ast::FuncCall*> aggregates;
    bool grouped = false;
    std::optional<IndexLookup> access;
    std::vector<JoinStep> joins;
};

namespace {

// KHALI sorts before every real value (in SEEDHA / ascending order).
bool sortLess(const Value& a, const Value& b) {
    if (a.isNull()) return !b.isNull();
    if (b.isNull()) return false;
    return pyLess(a, b);
}

Row combineRows(const Row& left, const Row& right) {
    Row combined = left;
    for (const auto& kv : right) combined[kv.first] = kv.second;  // keys never clash: they carry the source alias
    return combined;
}

using ValueBuckets = std::unordered_map<Value, std::vector<size_t>, PyValueHash, PyValueEq>;

// Combine every left row with the right rows that satisfy PAR.
//
// HASH JOIN (when PAR has `left.x = right.y`): put the right rows in a hash
// map keyed by y, then each left row finds its partners in O(1).  O(n + m)
// NESTED LOOP (anything else): try every pair.                    O(n * m)
//
//   INNER/NATURAL  only matched rows (NATURAL just has a synthesised PAR)
//   LEFT   (BAAYAN MILAO)  every left row kept once, KHALI-padded if unmatched
//   RIGHT  (DAHINA MILAO)  every right row kept once, KHALI-padded if unmatched
//   FULL   (DONO MILAO)    LEFT semantics PLUS any right row that matched nothing
std::vector<Row> joinRows(const std::vector<Row>& leftRows, const std::vector<Row>& rightRows, const ast::Expr& on,
                          const std::optional<std::pair<std::string, std::string>>& hashKeys,
                          const std::string& kind, const Row& nullLeft, const Row& nullRight) {
    std::vector<Row> out;
    if (kind == "RIGHT") {
        // Symmetric to LEFT but on the OTHER side: hash the left rows, drive the
        // loop from the right rows; the OUTPUT still holds both sides' keys.
        ValueBuckets buckets;
        std::vector<size_t> everyLeft;
        if (hashKeys) {
            for (size_t i = 0; i < leftRows.size(); ++i) {
                const Value& v = leftRows[i].at(hashKeys->first);
                if (!v.isNull()) buckets[v].push_back(i);
            }
        } else {
            for (size_t i = 0; i < leftRows.size(); ++i) everyLeft.push_back(i);
        }
        for (const auto& rRow : rightRows) {
            const std::vector<size_t>* partners = &everyLeft;
            static const std::vector<size_t> none;
            if (hashKeys) {
                const Value& v = rRow.at(hashKeys->second);
                auto it = v.isNull() ? buckets.end() : buckets.find(v);
                partners = it == buckets.end() ? &none : &it->second;
            }
            bool matched = false;
            for (size_t li : *partners) {
                Row combined = combineRows(leftRows[li], rRow);
                if (isTrue(evaluate(on, combined))) {
                    out.push_back(std::move(combined));
                    matched = true;
                }
            }
            if (!matched) out.push_back(combineRows(nullLeft, rRow));
        }
        return out;
    }

    bool keepLeftUnmatched = kind == "LEFT" || kind == "FULL";
    bool keepRightUnmatched = kind == "FULL";
    ValueBuckets buckets;
    std::vector<size_t> everyRight;
    if (hashKeys) {
        for (size_t i = 0; i < rightRows.size(); ++i) {
            const Value& v = rightRows[i].at(hashKeys->second);
            if (!v.isNull()) buckets[v].push_back(i);  // KHALI never equals anything, so it never joins
        }
    } else {
        for (size_t i = 0; i < rightRows.size(); ++i) everyRight.push_back(i);
    }
    std::vector<bool> matchedRight(rightRows.size(), false);
    for (const auto& lRow : leftRows) {
        const std::vector<size_t>* partners = &everyRight;
        static const std::vector<size_t> none;
        if (hashKeys) {
            const Value& v = lRow.at(hashKeys->first);
            auto it = v.isNull() ? buckets.end() : buckets.find(v);
            partners = it == buckets.end() ? &none : &it->second;
        }
        bool matched = false;
        for (size_t ri : *partners) {
            Row combined = combineRows(lRow, rightRows[ri]);
            if (isTrue(evaluate(on, combined))) {  // re-check the full PAR (it may have more conditions)
                out.push_back(std::move(combined));
                matched = true;
                matchedRight[ri] = true;
            }
        }
        if (keepLeftUnmatched && !matched) out.push_back(combineRows(lRow, nullRight));
    }
    if (keepRightUnmatched) {
        // FULL = LEFT (matched + left-unmatched-padded) UNION right-only-unmatched
        for (size_t ri = 0; ri < rightRows.size(); ++ri)
            if (!matchedRight[ri]) out.push_back(combineRows(nullLeft, rightRows[ri]));
    }
    return out;
}

// Replace every ColumnRef in `expr` that does NOT resolve against the
// subquery's OWN scope with a Literal of the matching outer-row value.
std::unique_ptr<ast::Expr> correlateExpr(const ast::Expr& expr, const Scope& sub, const Row& outerRow,
                                         const std::vector<std::string>& outerKeys, bool& fired) {
    using namespace ast;
    auto rec = [&](const Expr* e) -> std::unique_ptr<Expr> {
        return e ? correlateExpr(*e, sub, outerRow, outerKeys, fired) : nullptr;
    };
    if (dynamic_cast<const Literal*>(&expr) || dynamic_cast<const Star*>(&expr)) return cloneExpr(expr);
    if (auto* ref = dynamic_cast<const ColumnRef*>(&expr)) {
        try {
            sub.resolve(*ref);
            return cloneExpr(expr);  // resolves locally -- this is NOT a correlation
        } catch (const ExecutionError&) {
        }
        std::string key;
        if (ref->table.has_value()) {
            key = *ref->table + "." + ref->name;
            if (!outerRow.count(key))
                throw ExecutionError("'" + key + "' na is subquery mein na outer query mein mila");
        } else {
            std::vector<std::string> matches;
            std::string suffix = "." + ref->name;
            for (const auto& k : outerKeys)
                if (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0)
                    matches.push_back(k);
            if (matches.empty())
                throw ExecutionError("Column '" + ref->name + "' na is subquery mein na outer query mein mila");
            if (matches.size() > 1)
                throw ExecutionError("Column '" + ref->name + "' outer query mein ek se zyada tables mein hai -- " +
                                     joinStrs(matches, " ya ") + " likho");
            key = matches[0];
        }
        fired = true;
        return std::make_unique<Literal>(outerRow.at(key));
    }
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
        auto l = rec(b->left.get());  // left first: its error wins, like Python
        auto r = rec(b->right.get());
        return std::make_unique<BinaryOp>(b->op, std::move(l), std::move(r));
    }
    if (auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
        auto out = std::make_unique<UnaryOp>();
        out->op = u->op;
        out->operand = rec(u->operand.get());
        return out;
    }
    if (auto* isn = dynamic_cast<const IsNull*>(&expr)) {
        auto out = std::make_unique<IsNull>();
        out->expr = rec(isn->expr.get());
        out->negated = isn->negated;
        return out;
    }
    if (auto* f = dynamic_cast<const FuncCall*>(&expr)) {
        auto out = std::make_unique<FuncCall>();
        out->name = f->name;
        out->arg = rec(f->arg.get());
        return out;
    }
    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) {
        auto out = std::make_unique<Coalesce>();
        for (const auto& a : co->args) out->args.push_back(rec(a.get()));
        return out;
    }
    if (auto* cw = dynamic_cast<const CaseWhen*>(&expr)) {
        auto out = std::make_unique<CaseWhen>();
        for (const auto& [cond, value] : cw->branches) {
            auto c = rec(cond.get());
            auto v = rec(value.get());
            out->branches.emplace_back(std::move(c), std::move(v));
        }
        out->elseExpr = rec(cw->elseExpr.get());
        return out;
    }
    if (dynamic_cast<const Subquery*>(&expr) || dynamic_cast<const InSubquery*>(&expr))
        return cloneExpr(expr);  // a NESTED subquery correlates against ITS OWN nesting when IT runs
    throw ExecutionError("Unknown expression");
}

// Scalar context (Subquery): exactly 1 column, 0 or 1 row. List context
// (InSubquery): exactly 1 column, any number of rows.
SubqueryResult reduceSubqueryResult(const ast::Expr& node, const Result& result) {
    if (result.columns.size() != 1) throw ExecutionError("Subquery sirf 1 column return kar sakti hai is jagah");
    SubqueryResult out;
    if (dynamic_cast<const ast::Subquery*>(&node)) {
        if (result.rows.size() > 1)
            throw ExecutionError("Subquery ek se zyada rows return kar rahi hai -- sirf 1 row honi chahiye");
        if (!result.rows.empty()) out.scalar = result.rows[0][0];
    } else {
        for (const auto& r : result.rows) out.values.push_back(r[0]);
    }
    return out;
}

}  // namespace

// A SE/MILAO source: a real Table, or -- if `name` isn't a table -- a VIEW,
// materialized fresh by re-running its stored DIKHAO text (so it always
// reflects the CURRENT schema of whatever it selects from).
std::unique_ptr<Table> Engine::resolveSource(const std::string& name) {
    Catalog& cat = catalog();
    if (cat.find(name) != nullptr) return table(name);
    if (cat.views.count(name)) {
        auto parsed = parseScript(cat.views.at(name));
        auto* viewStmt = parsed.empty() ? nullptr : dynamic_cast<const ast::Select*>(parsed[0].get());
        if (viewStmt == nullptr) throw ExecutionError("View '" + name + "' ki definition DIKHAO nahi hai");
        Result result = execSelect(*viewStmt);
        TableSchema schema;
        schema.name = name;
        for (size_t i = 0; i < result.columns.size(); ++i) {
            Column c;
            c.name = result.columns[i];
            c.typeName = inferColumnType(result.rows, i);
            schema.columns.push_back(c);
        }
        return std::make_unique<MaterializedTable>(schema, result.rows);
    }
    throw ExecutionError("Table '" + name + "' exist nahi karta");
}

Scope Engine::selectSourcesScope(const ast::Select& stmt, std::vector<std::unique_ptr<Table>>* tablesOut) {
    std::vector<std::pair<std::string, std::string>> sources;  // (alias, table)
    sources.emplace_back(stmt.alias.has_value() && !stmt.alias->empty() ? *stmt.alias : stmt.table, stmt.table);
    for (const auto& j : stmt.joins) sources.emplace_back(j.alias, j.table);
    std::vector<std::unique_ptr<Table>> tables;
    for (const auto& s : sources) tables.push_back(resolveSource(s.second));
    std::vector<std::pair<std::string, TableSchema>> withSchemas;
    for (size_t i = 0; i < sources.size(); ++i) withSchemas.emplace_back(sources[i].first, tables[i]->schema());
    Scope scope(std::move(withSchemas));
    if (tablesOut) *tablesOut = std::move(tables);
    return scope;
}

std::unique_ptr<Engine::SelectPlan> Engine::planSelect(const ast::Select& stmt) {
    auto plan = std::make_unique<SelectPlan>();
    plan->scope = std::make_unique<Scope>(selectSourcesScope(stmt, &plan->tables));
    const Scope& scope = *plan->scope;

    // KAHO: an output alias becomes the column header, and (only) KRAM may
    // refer back to it by name -- JAHAN/JINKA do not (standard SQL: they run
    // before the output list exists). We collect {alias: unbound expr} here so
    // KRAM can be rewritten to that expr BEFORE binding.
    std::unordered_map<std::string, const ast::Expr*> aliasExprs;
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        const ast::Expr& e = *stmt.columns[i];
        std::optional<std::string> alias = i < stmt.aliases.size() ? stmt.aliases[i] : std::nullopt;
        bool hasAlias = alias.has_value() && !alias->empty();
        if (auto* star = dynamic_cast<const ast::Star*>(&e)) {
            for (const auto& [label, ref] : scope.expandStar(*star)) {
                plan->labels.push_back(label);
                plan->outputs.push_back(bind(ref, scope));
            }
        } else {
            plan->labels.push_back(hasAlias ? *alias : exprLabel(e));  // KAHO wins over the default header
            plan->outputs.push_back(bind(e, scope));
            if (hasAlias) aliasExprs[*alias] = &e;
        }
    }

    auto isRealColumn = [&](const std::string& name) {
        for (const auto& [alias, schema] : scope.sources()) {
            (void)alias;
            for (const auto& c : schema.columns)
                if (c.name == name) return true;
        }
        return false;
    };
    // a bare `naam` in KRAM that matches an alias (and isn't a real column)
    // means the output expression, not a column lookup
    auto orderExpr = [&](const ast::Expr& e) -> const ast::Expr& {
        if (auto* ref = dynamic_cast<const ast::ColumnRef*>(&e)) {
            auto it = aliasExprs.find(ref->name);
            if (!ref->table.has_value() && it != aliasExprs.end() && !isRealColumn(ref->name)) return *it->second;
        }
        return e;
    };

    plan->where = bind(stmt.where.get(), scope);
    for (const auto& g : stmt.groupBy) plan->groupBy.push_back(bind(*g, scope));
    plan->having = bind(stmt.having.get(), scope);
    for (const auto& o : stmt.orderBy) plan->orderBy.emplace_back(bind(orderExpr(*o.expr), scope), o.descending);

    // every distinct aggregate call, in first-seen order
    std::vector<const ast::Expr*> aggSources;
    for (const auto& o : plan->outputs) aggSources.push_back(o.get());
    aggSources.push_back(plan->having.get());
    for (const auto& o : plan->orderBy) aggSources.push_back(o.first.get());
    std::set<std::string> seenAggs;
    for (const auto* e : aggSources) {
        if (!e) continue;
        std::vector<const ast::FuncCall*> found;
        findAggregates(*e, found);
        for (const auto* func : found) {
            canonicalName(*func);  // fail early on unknown functions
            if (seenAggs.insert(aggKey(*func)).second) plan->aggregates.push_back(func);
        }
    }
    plan->grouped = !plan->groupBy.empty() || !plan->aggregates.empty() || plan->having != nullptr;
    if (plan->grouped) {
        // In a grouped query each output row stands for a whole GROUP of rows,
        // so a bare column like `naam` has no single value -- unless we grouped by it.
        std::set<std::string> grouped;
        for (const auto& g : plan->groupBy)
            for (const auto& name : columnRefKeys(g.get())) grouped.insert(name);
        for (const auto* e : aggSources)
            for (const auto& name : columnRefKeys(e, true))
                if (!grouped.count(name))
                    throw ExecutionError("Column '" + scope.display(name) +
                                         "' SAMOOH mein nahi hai -- ise SAMOOH mein daalo ya kisi aggregate "
                                         "(GINO, KUL, AUSAT...) ke andar use karo");
    }

    plan->access = chooseAccess(*plan->tables[0], scope, plan->where.get());
    for (size_t i = 0; i < stmt.joins.size(); ++i) {
        const ast::Join& join = stmt.joins[i];
        size_t sourceIndex = i + 1;
        SelectPlan::JoinStep step;
        step.join = &join;
        if (join.kind == "NATURAL") {
            // no PAR written by the user -- synthesise `earlier.col = new.col`
            // for every column name shared with an already-joined table
            step.on = naturalJoinCondition(scope, sourceIndex);
        } else {
            step.on = bind(join.on.get(), scope);
        }
        checkJoinCondition(step.on.get(), scope, sourceIndex);
        step.hashKeys = chooseJoin(step.on.get(), scope, sourceIndex);
        plan->joins.push_back(std::move(step));
    }
    return plan;
}

// Bucket rows by their SAMOOH values, then turn each bucket into ONE row: the
// bucket's first row (for the grouped columns) plus every aggregate's result
// stored under aggKey(). evaluate() then finds them there.
std::vector<Row> Engine::group(const std::vector<Row>& rows, const SelectPlan& plan) {
    std::vector<std::vector<Row>> buckets;  // in first-seen order
    std::unordered_map<std::vector<Value>, size_t, PyValuesHash, PyValuesEq> bucketOf;
    for (const auto& r : rows) {
        std::vector<Value> key;
        for (const auto& g : plan.groupBy) key.push_back(evaluate(*g, r));
        auto it = bucketOf.find(key);
        if (it == bucketOf.end()) {
            it = bucketOf.emplace(std::move(key), buckets.size()).first;
            buckets.emplace_back();
        }
        buckets[it->second].push_back(r);
    }
    if (plan.groupBy.empty() && buckets.empty()) buckets.emplace_back();  // `DIKHAO GINO(*) SE empty_table` must still return one row: 0

    std::vector<Row> out;
    for (const auto& members : buckets) {
        Row groupRow;
        if (!members.empty()) {
            groupRow = members[0];
        } else {
            for (const auto& k : plan.scope->allKeys()) groupRow[k] = Value();
        }
        for (const auto* func : plan.aggregates) groupRow[aggKey(*func)] = computeAggregate(*func, members);
        out.push_back(std::move(groupRow));
    }
    return out;
}

std::unique_ptr<ast::Select> Engine::correlateSelect(const ast::Select& stmt, const Scope& subqueryScope,
                                                     const Row& outerRow, const std::vector<std::string>& outerKeys,
                                                     bool& fired) {
    auto corr = [&](const ast::Expr* e) -> std::unique_ptr<ast::Expr> {
        return e ? correlateExpr(*e, subqueryScope, outerRow, outerKeys, fired) : nullptr;
    };
    auto out = std::make_unique<ast::Select>();
    for (const auto& c : stmt.columns) out->columns.push_back(corr(c.get()));
    out->table = stmt.table;
    out->alias = stmt.alias;
    for (const auto& j : stmt.joins) {
        ast::Join nj;
        nj.table = j.table;
        nj.alias = j.alias;
        nj.on = corr(j.on.get());
        nj.kind = j.kind;
        out->joins.push_back(std::move(nj));
    }
    out->where = corr(stmt.where.get());
    for (const auto& g : stmt.groupBy) out->groupBy.push_back(corr(g.get()));
    out->having = corr(stmt.having.get());
    for (const auto& o : stmt.orderBy) {
        ast::OrderItem item;
        item.expr = corr(o.expr.get());
        item.descending = o.descending;
        out->orderBy.push_back(std::move(item));
    }
    out->limit = stmt.limit;
    out->distinct = stmt.distinct;
    out->aliases = stmt.aliases;
    return out;
}

// Runs `sub` against one outer row and returns its Result; `fired` says
// whether it turned out to be CORRELATED (some column resolved in the outer
// row). If it never fires, the result doesn't depend on the outer row's
// VALUES at all (only its KEYS, which are the same for every row of the same
// outer query -- see precomputeSubqueries).
Result Engine::runSubquery(const ast::Subquery& sub, const Row& outerRow, const std::vector<std::string>& outerKeys,
                           bool& fired) {
    const ast::Select& stmt = *sub.statement;
    Scope subqueryScope = selectSourcesScope(stmt, nullptr);
    auto newStmt = correlateSelect(stmt, subqueryScope, outerRow, outerKeys, fired);
    return execSelect(*newStmt);
}

// One SubqueryResults per row, ready to pass into evaluate(). A subquery is
// structurally either correlated or not -- whether its columns resolve
// locally depends only on which KEYS the outer row has, not their values, and
// every row here has the same keys -- so ONE dry run (against the first row)
// decides correlated-vs-not for ALL rows: uncorrelated results are computed
// once and shared; correlated ones are recomputed per row.
Engine::RowSubqueries Engine::precomputeSubqueries(const std::vector<const ast::Expr*>& exprs,
                                                   const std::vector<Row>& rows,
                                                   const std::vector<std::string>& outerKeys) {
    RowSubqueries out;
    std::vector<const ast::Expr*> nodes;
    std::unordered_set<const ast::Expr*> seen;
    for (const auto* e : exprs) {
        if (!e) continue;
        std::vector<const ast::Expr*> found;
        findSubqueries(*e, found);
        for (const auto* node : found)
            if (seen.insert(node).second) nodes.push_back(node);
    }
    if (nodes.empty() || rows.empty()) return out;

    auto subqueryOf = [](const ast::Expr* node) -> const ast::Subquery& {
        if (auto* s = dynamic_cast<const ast::Subquery*>(node)) return *s;
        return *static_cast<const ast::InSubquery*>(node)->subquery;
    };
    std::unordered_set<const ast::Expr*> correlated;
    for (const auto* node : nodes) {
        bool fired = false;
        Result result = runSubquery(subqueryOf(node), rows[0], outerKeys, fired);
        if (fired) correlated.insert(node);
        else out.shared[node] = reduceSubqueryResult(*node, result);
    }
    if (!correlated.empty()) {
        out.perRow.reserve(rows.size());
        for (const auto& row : rows) {
            SubqueryResults d = out.shared;
            for (const auto* node : nodes) {
                if (!correlated.count(node)) continue;
                bool fired = false;
                Result result = runSubquery(subqueryOf(node), row, outerKeys, fired);
                d[node] = reduceSubqueryResult(*node, result);
            }
            out.perRow.push_back(std::move(d));
        }
    }
    return out;
}

Result Engine::execSelect(const ast::Select& stmt) {
    auto planPtr = planSelect(stmt);
    SelectPlan& plan = *planPtr;
    const Scope& scope = *plan.scope;
    const std::vector<std::string> keys = scope.allKeys();

    // The pipeline, in the same order a real database runs it:
    //   scan/index -> MILAO -> JAHAN -> SAMOOH + aggregates -> JINKA -> KRAM
    //   -> project -> ALAG -> SIRF

    // 1. READ the first table (index lookup or full scan)
    std::vector<Row> rows;
    for (const auto& c : candidates(*plan.tables[0], plan.access)) rows.push_back(scope.row(0, c.second));

    // 2. JOIN (MILAO) each further table
    for (size_t i = 0; i < plan.joins.size(); ++i) {
        size_t src = i + 1;
        std::vector<Row> rightRows;
        for (const auto& r : plan.tables[src]->rows()) rightRows.push_back(scope.row(src, r.second));
        Row nullLeft;
        for (size_t j = 0; j < src; ++j)
            for (auto& kv : scope.nullRow(j)) nullLeft[kv.first] = kv.second;
        rows = joinRows(rows, rightRows, *plan.joins[i].on, plan.joins[i].hashKeys, plan.joins[i].join->kind,
                        nullLeft, scope.nullRow(src));
    }

    // 3. FILTER (JAHAN) -- any WHERE subquery is pre-computed per outer row
    //    (once, if uncorrelated; per row, if correlated -- see precomputeSubqueries)
    if (plan.where) {
        auto subq = precomputeSubqueries({plan.where.get()}, rows, keys);
        std::vector<Row> kept;
        for (size_t i = 0; i < rows.size(); ++i)
            if (isTrue(evaluate(*plan.where, rows[i], subq.at(i)))) kept.push_back(std::move(rows[i]));
        rows = std::move(kept);
    }

    // 4. GROUP (SAMOOH) + compute aggregates, then filter groups (JINKA)
    if (plan.grouped) {
        rows = group(rows, plan);
        if (plan.having) {
            auto subq = precomputeSubqueries({plan.having.get()}, rows, keys);
            std::vector<Row> kept;
            for (size_t i = 0; i < rows.size(); ++i)
                if (isTrue(evaluate(*plan.having, rows[i], subq.at(i)))) kept.push_back(std::move(rows[i]));
            rows = std::move(kept);
        }
    }

    // 5. SORT (KRAM). The sort is *stable*, so sorting by the LAST key first
    //    and the FIRST key last gives a correct multi-column sort.
    for (auto it = plan.orderBy.rbegin(); it != plan.orderBy.rend(); ++it) {
        const ast::Expr& expr = *it->first;
        bool descending = it->second;
        auto subq = precomputeSubqueries({&expr}, rows, keys);
        std::vector<Value> sortKeys;
        for (size_t i = 0; i < rows.size(); ++i) sortKeys.push_back(evaluate(expr, rows[i], subq.at(i)));
        std::vector<size_t> order(rows.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        if (descending)
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sortLess(sortKeys[b], sortKeys[a]); });
        else
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sortLess(sortKeys[a], sortKeys[b]); });
        std::vector<Row> sorted;
        sorted.reserve(rows.size());
        for (size_t idx : order) sorted.push_back(std::move(rows[idx]));
        rows = std::move(sorted);
    }

    // 6. PROJECT (pick / compute the output columns)
    std::vector<const ast::Expr*> outputPtrs;
    for (const auto& o : plan.outputs) outputPtrs.push_back(o.get());
    auto projSubq = precomputeSubqueries(outputPtrs, rows, keys);
    std::vector<std::vector<Value>> outRows;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<Value> out;
        for (const auto* e : outputPtrs) out.push_back(evaluate(*e, rows[i], projSubq.at(i)));
        outRows.push_back(std::move(out));
    }

    // 7. DISTINCT (ALAG): keep the first copy of each row, preserving order
    if (stmt.distinct) {
        std::unordered_set<std::vector<Value>, PyValuesHash, PyValuesEq> seen;
        std::vector<std::vector<Value>> unique;
        for (auto& row : outRows)
            if (seen.insert(row).second) unique.push_back(std::move(row));
        outRows = std::move(unique);
    }

    // 8. LIMIT (SIRF)  (Python's `rows[:n]`)
    if (stmt.limit.has_value()) {
        int64_t n = *stmt.limit;
        int64_t size = static_cast<int64_t>(outRows.size());
        int64_t keep = n >= 0 ? std::min(n, size) : std::max<int64_t>(0, size + n);
        outRows.resize(static_cast<size_t>(keep));
    }

    Result result;
    result.columns = plan.labels;
    result.message = std::to_string(outRows.size()) + " row(s)";
    result.rows = std::move(outRows);
    return result;
}

// ============================================================================
// set operations: SANYUKT (UNION), SAAJHA (INTERSECT), CHHODKAR (EXCEPT)
// ============================================================================

Result Engine::execSetOp(const ast::SetOp& stmt) {
    Result left = executeStatement(*stmt.left);
    Result right = executeStatement(*stmt.right);
    if (left.columns.size() != right.columns.size())
        throw ExecutionError(stmt.op + " (" + setOpName(stmt.op) + ") ke dono taraf " +
                             std::to_string(left.columns.size()) + " columns chahiye, " +
                             std::to_string(left.columns.size()) + " aur " + std::to_string(right.columns.size()) +
                             " mile");
    using RowSet = std::unordered_set<std::vector<Value>, PyValuesHash, PyValuesEq>;
    std::vector<std::vector<Value>> out;
    RowSet seen;
    if (stmt.op == "SANYUKT") {  // UNION: dedupe, preserving first-occurrence order
        for (const auto* side : {&left.rows, &right.rows})
            for (const auto& row : *side)
                if (seen.insert(row).second) out.push_back(row);
    } else {
        RowSet rightSet(right.rows.begin(), right.rows.end());
        bool intersect = stmt.op == "SAAJHA";  // INTERSECT: in BOTH; EXCEPT: in left but NOT right -- both deduped, left's order
        for (const auto& row : left.rows)
            if ((rightSet.count(row) > 0) == intersect && seen.insert(row).second) out.push_back(row);
    }
    Result result;
    result.columns = left.columns;
    result.message = std::to_string(out.size()) + " row(s)";
    result.rows = std::move(out);
    return result;
}

// ============================================================================
// views
// ============================================================================

Result Engine::execShowViews(const ast::ShowViews&) {
    std::vector<std::string> names;
    for (const auto& entry : catalog().views) names.push_back(entry.first);
    std::sort(names.begin(), names.end());
    Result r;
    r.columns = {"view"};
    for (auto& n : names) r.rows.push_back({textValue(n)});
    r.message = std::to_string(names.size()) + " view(s) in '" + currentDb + "'";
    return r;
}

Result Engine::execCreateView(const ast::CreateView& stmt) {
    Catalog& cat = catalog();
    if (cat.find(stmt.name) != nullptr)
        throw ExecutionError("Table '" + stmt.name + "' pehle se hai -- VIEW usi naam se nahi ban sakti");
    if (cat.views.count(stmt.name)) throw ExecutionError("View '" + stmt.name + "' pehle se hai");
    auto parsed = parseScript(stmt.queryText);
    auto* select = parsed.empty() ? nullptr : dynamic_cast<const ast::Select*>(parsed[0].get());
    if (select == nullptr) throw ExecutionError("BANAO VIEW ke baad sirf ek DIKHAO query aa sakti hai");
    execSelect(*select);  // sanity check: must run cleanly against the CURRENT schema
    cat.addView(stmt.name, stmt.queryText);
    return messageResult("View '" + stmt.name + "' ban gaya");
}

Result Engine::execDropView(const ast::DropView& stmt) {
    Catalog& cat = catalog();
    if (!cat.views.count(stmt.name)) throw ExecutionError("View '" + stmt.name + "' exist nahi karta");
    cat.removeView(stmt.name);
    return messageResult("View '" + stmt.name + "' hata diya");
}

// ============================================================================
// SAMJHAO (EXPLAIN)
// ============================================================================

Result Engine::execExplain(const ast::Explain& stmt) {
    const ast::Statement& inner = *stmt.statement;
    std::vector<std::string> lines;
    if (auto* sel = dynamic_cast<const ast::Select*>(&inner)) {
        auto plan = planSelect(*sel);
        lines = explainSelect(*sel, *plan);
    } else if (auto* setOp = dynamic_cast<const ast::SetOp*>(&inner)) {
        lines.push_back(setOp->op + " (" + setOpName(setOp->op) + ") of:");
        const std::pair<const ast::Statement*, const char*> sides[] = {{setOp->left.get(), "LEFT"},
                                                                       {setOp->right.get(), "RIGHT"}};
        for (const auto& [side, label] : sides) {
            auto subLines = explainOne(*side);
            lines.push_back(std::string("  ") + label + ":");
            for (const auto& line : subLines) lines.push_back("    " + line);
        }
    } else if (dynamic_cast<const ast::Update*>(&inner) || dynamic_cast<const ast::Delete*>(&inner)) {
        auto* upd = dynamic_cast<const ast::Update*>(&inner);
        const std::string& tableName = upd ? upd->table : static_cast<const ast::Delete&>(inner).table;
        const ast::Expr* where = upd ? upd->where.get() : static_cast<const ast::Delete&>(inner).where.get();
        auto t = table(tableName);
        Scope scope({{tableName, t->schema()}});
        auto access = chooseAccess(*t, scope, bind(where, scope).get());
        lines.push_back(access ? access->describe(*t) : "FULL SCAN " + tableName);
        if (where) lines.push_back("FILTER  JAHAN " + exprLabel(*where));
        lines.push_back(std::string(upd ? "BADLO (update)" : "MITAO (delete)") + " matching rows");
    } else {
        throw ExecutionError("SAMJHAO sirf DIKHAO, BADLO aur MITAO ke saath chalta hai");
    }
    Result r;
    r.columns = {"plan"};
    for (size_t i = 0; i < lines.size(); ++i) r.rows.push_back({textValue(std::to_string(i + 1) + ". " + lines[i])});
    r.message = "Query plan (query chalayi nahi gayi)";
    return r;
}

// One side of a SetOp -- itself a Select or (recursively) a SetOp.
std::vector<std::string> Engine::explainOne(const ast::Statement& stmt) {
    if (auto* setOp = dynamic_cast<const ast::SetOp*>(&stmt)) {
        std::vector<std::string> lines{setOp->op + " (" + setOpName(setOp->op) + ") of:"};
        for (const ast::Statement* side : {setOp->left.get(), setOp->right.get()})
            for (const auto& line : explainOne(*side)) lines.push_back("  " + line);
        return lines;
    }
    auto* sel = dynamic_cast<const ast::Select*>(&stmt);
    if (sel == nullptr) throw ExecutionError("SAMJHAO sirf DIKHAO, BADLO aur MITAO ke saath chalta hai");
    auto plan = planSelect(*sel);
    return explainSelect(*sel, *plan);
}

std::vector<std::string> Engine::explainSelect(const ast::Select& stmt, const SelectPlan& plan) {
    auto joinLabel = [](const std::string& kind) -> std::string {
        if (kind == "LEFT") return "LEFT ";
        if (kind == "RIGHT") return "RIGHT ";
        if (kind == "FULL") return "FULL ";
        if (kind == "NATURAL") return "NATURAL ";
        return "";
    };
    std::vector<std::string> lines;
    std::string alias = stmt.alias.has_value() && !stmt.alias->empty() ? " " + *stmt.alias : "";
    lines.push_back(plan.access ? plan.access->describe(*plan.tables[0]) : "FULL SCAN " + stmt.table + alias);
    for (const auto& step : plan.joins) {
        std::string kind = joinLabel(step.join->kind) + (step.hashKeys ? "HASH JOIN" : "NESTED LOOP JOIN");
        std::string name = step.join->alias == step.join->table ? step.join->table : step.join->table + " " + step.join->alias;
        lines.push_back(kind + " " + name + " PAR " + exprLabel(*step.on));
    }
    auto append = [&](const std::vector<std::string>& more) { lines.insert(lines.end(), more.begin(), more.end()); };
    if (stmt.where) lines.push_back("FILTER  JAHAN " + exprLabel(*stmt.where));
    append(explainSubqueries({stmt.where.get()}, *plan.scope));
    if (plan.grouped) {
        std::vector<std::string> aggLabels;
        for (const auto* a : plan.aggregates) aggLabels.push_back(exprLabel(*a));
        std::string aggs = aggLabels.empty() ? "-" : joinStrs(aggLabels, ", ");
        if (!stmt.groupBy.empty()) {
            std::vector<std::string> groupLabels;
            for (const auto& g : stmt.groupBy) groupLabels.push_back(exprLabel(*g));
            lines.push_back("GROUP  SAMOOH " + joinStrs(groupLabels, ", ") + "  [aggregates: " + aggs + "]");
        } else {
            lines.push_back("AGGREGATE saari rows ek group  [aggregates: " + aggs + "]");
        }
    }
    if (stmt.having) {
        lines.push_back("FILTER GROUPS  JINKA " + exprLabel(*stmt.having));
        append(explainSubqueries({stmt.having.get()}, *plan.scope));
    }
    if (!stmt.orderBy.empty()) {
        std::vector<std::string> keys;
        for (const auto& o : stmt.orderBy) keys.push_back(exprLabel(*o.expr) + (o.descending ? " ULTA" : ""));
        lines.push_back("SORT  KRAM " + joinStrs(keys, ", "));
    }
    lines.push_back("PROJECT  " + joinStrs(plan.labels, ", "));
    std::vector<const ast::Expr*> outputs;
    for (const auto& o : plan.outputs) outputs.push_back(o.get());
    append(explainSubqueries(outputs, *plan.scope));
    if (stmt.distinct) lines.push_back("DISTINCT  ALAG");
    if (stmt.limit.has_value()) lines.push_back("LIMIT  SIRF " + std::to_string(*stmt.limit));
    return lines;
}

// One line per DISTINCT subquery node found in `exprs`, naming whether it's
// correlated. Deliberately shallow: only the subquery's OWN first plan line is
// shown, not its whole nested plan tree.
std::vector<std::string> Engine::explainSubqueries(const std::vector<const ast::Expr*>& exprs, const Scope& scope) {
    std::vector<std::string> lines;
    std::unordered_set<const ast::Subquery*> seen;
    const std::vector<std::string> keys = scope.allKeys();
    Row dummyOuter;
    for (const auto& k : keys) dummyOuter[k] = Value();
    for (const auto* e : exprs) {
        if (!e) continue;
        std::vector<const ast::Expr*> nodes;
        findSubqueries(*e, nodes);
        for (const auto* node : nodes) {
            const ast::Subquery* sub = dynamic_cast<const ast::Subquery*>(node);
            if (!sub) sub = static_cast<const ast::InSubquery*>(node)->subquery.get();
            if (!seen.insert(sub).second) continue;
            Scope subqueryScope = selectSourcesScope(*sub->statement, nullptr);
            bool fired = false;
            auto substituted = correlateSelect(*sub->statement, subqueryScope, dummyOuter, keys, fired);
            auto innerPlan = planSelect(*substituted);
            std::string firstLine = explainSelect(*substituted, *innerPlan).at(0);
            lines.push_back(std::string("SUBQUERY (") + (fired ? "correlated" : "uncorrelated") + "): " + firstLine);
        }
    }
    return lines;
}

}  // namespace meradb
