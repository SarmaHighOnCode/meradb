// cpp/src/engine.cpp -- mirrors meradb/engine.py
#include "meradb/fs_util.h"
#include "meradb/engine.h"
#include "meradb/aggregates.h"
#include "meradb/ast_util.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/protocol.h"
#include "meradb/scalar_funcs.h"
#include "meradb/pyvalue.h"
#include "meradb/stack_guard.h"
#include "meradb/storage.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
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
    if (ec) throw StorageError("'" + from.u8string() + "' ko '" + to.u8string() + "' nahi bana paaye: " + ec.message());
}

// The data folder must exist before anything (UserStore) is built on top of it.
const std::string& ensureDir(const std::string& dir) {
    fs::create_directories(pathOf(dir));
    return dir;
}

}  // namespace

Instance::Instance(std::string dataDir, bool served)
    : dataDir_(absolutePathOf(dataDir)), users_(ensureDir(dataDir_)) {
    if (!served) {
        // Two processes must never write the same files: refuse a folder a server is serving.
        if (auto info = protocol::runningServer(dataDir_)) {
            std::string port = info->contains("port") ? info->at("port").dump() : "None";
            throw MeraDBError("Is data folder par MeraDB server chal raha hai (port " + port +
                              "). Seedha files mat kholo -- `meradb shell` se server se connect karo.");
        }
    }
    recovered_ = recover();
    fs::create_directories(pathOf(dbDir(DEFAULT_DATABASE)));
}

std::string Instance::dbDir(const std::string& name) const {
    if (!isSafeFileName(name)) throw ExecutionError("Database ka naam " + pyRepr(name) + " sahi nahi hai");
    return (pathOf(dataDir_) / pathOf(name)).u8string();
}

std::vector<std::string> Instance::databases() const {
    std::vector<std::string> names;
    // Error-code overloads: a concurrent DROP DATABASE may remove an entry mid-listing.
    std::error_code ec;
    for (fs::directory_iterator it(pathOf(dataDir_), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        std::string name = it->path().filename().u8string();
        if (isSafeFileName(name) && it->is_directory(entryEc) && !entryEc) names.push_back(name);
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
    return (pathOf(dataDir_) / SNAPSHOT_DIR / pathOf(db + suffix)).u8string();
}

void Instance::takeSnapshot(const std::string& db) {
    fs::path tmp = pathOf(snapshotPath(db, ".tmp"));
    removeTreeQuietly(tmp);
    fs::create_directories(tmp.parent_path());
    std::error_code ec;
    fs::copy(pathOf(dbDir(db)), tmp, fs::copy_options::recursive, ec);
    if (ec) throw StorageError("Database '" + db + "' ki copy nahi ban paayi: " + ec.message());
    // The copy is on the disk BEFORE it gets its name and before the transaction touches a live file: a name without
    // its bytes would "roll back" to garbage after a power cut.
    if (!syncRelaxed() && !syncTree(textOf(tmp))) throw StorageError("Database '" + db + "' ki copy disk par pakki nahi hui");
    crashPoint("snapshot-before-rename");
    replacePath(tmp, pathOf(snapshotPath(db)));  // only a COMPLETE copy ever gets the real name
    if (!syncRelaxed()) syncFolder(textOf(tmp.parent_path()));
    crashPoint("snapshot-after-rename");
    durability_.enterTransaction();
}

void Instance::discardSnapshot(const std::string& db) {
    durability_.syncDirty();  // every change of the transaction is on the disk BEFORE the commit point
    crashPoint("commit-before-marker");
    fs::path done = pathOf(snapshotPath(db, ".done"));
    replacePath(pathOf(snapshotPath(db)), done);  // <- the COMMIT POINT: after this, no rollback
    if (!syncRelaxed()) syncFolder(textOf(done.parent_path()));  // and the commit point itself, before PAKKA is acknowledged
    crashPoint("commit-after-marker");
    removeTreeQuietly(done);
    durability_.leaveTransaction();
}

void Instance::restoreSnapshot(const std::string& db) {
    durability_.forgetDirty();  // those files are about to go
    removeTreeQuietly(pathOf(dbDir(db)));
    crashPoint("rollback-after-remove");
    fs::path snapshot = pathOf(snapshotPath(db));
    replacePath(snapshot, pathOf(dbDir(db)));
    if (!syncRelaxed()) {
        syncFolder(textOf(snapshot.parent_path()));
        syncFolder(dataDir_);
    }
    durability_.leaveTransaction();
    forget(db);
}

void Instance::takeCopy(const std::string& db, const std::string& tag) {
    const std::string suffix = "." + tag + ".sp";
    fs::path tmp = pathOf(snapshotPath(db, suffix + ".tmp"));
    removeTreeQuietly(tmp);
    fs::create_directories(tmp.parent_path());
    std::error_code ec;
    fs::copy(pathOf(dbDir(db)), tmp, fs::copy_options::recursive, ec);
    if (ec) throw StorageError("Database '" + db + "' ki copy nahi ban paayi: " + ec.message());
    removeTreeQuietly(pathOf(snapshotPath(db, suffix)));
    replacePath(tmp, pathOf(snapshotPath(db, suffix)));
}

void Instance::restoreCopy(const std::string& db, const std::string& tag) {
    fs::path from = pathOf(snapshotPath(db, "." + tag + ".sp"));
    fs::path staging = pathOf(snapshotPath(db, ".restoring.tmp"));  // a half-done restore is cleaned up like any .tmp
    removeTreeQuietly(staging);
    std::error_code ec;
    fs::copy(from, staging, fs::copy_options::recursive, ec);
    if (ec) throw StorageError("Database '" + db + "' ki copy wapas nahi aa paayi: " + ec.message());
    // The live folder steps aside in ONE rename, so a crash leaves it whole or leaves the finished staging copy, which
    // recover() moves into place. (Deleting it first would lose the database if a crash came before the second rename.)
    fs::path aside = pathOf(snapshotPath(db, ".aside.tmp"));
    removeTreeQuietly(aside);
    replacePath(pathOf(dbDir(db)), aside);
    crashPoint("restore-copy-between-renames");
    replacePath(staging, pathOf(dbDir(db)));
    removeTreeQuietly(aside);
    durability_.markTree(dbDir(db));  // every file is a new one now; PAKKA or the statement's end syncs them
    durability_.markFolder(dataDir_);
    forget(db);
}

void Instance::dropCopy(const std::string& db, const std::string& tag) {
    removeTreeQuietly(pathOf(snapshotPath(db, "." + tag + ".sp")));
}

std::vector<std::string> Instance::recover() {
    // CRASH RECOVERY, run at startup. A snapshot still lying around means the
    // process died in the middle of a transaction that never reached PAKKA,
    // so it is rolled back. `.tmp` = SHURU never finished; `.done` = PAKKA
    // already happened -- both are just deleted.
    fs::path root = pathOf(dataDir_) / SNAPSHOT_DIR;
    std::vector<std::string> recovered;
    if (!fs::is_directory(root)) return recovered;
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(root)) names.push_back(entry.path().filename().u8string());
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        const std::string restoring = ".restoring.tmp";
        if (endsWith(name, restoring)) {  // a restoreCopy that died between its two renames: this IS the database now
            fs::path live = pathOf(dbDir(name.substr(0, name.size() - restoring.size())));
            if (!fs::exists(live)) {
                replacePath(root / pathOf(name), live);
                continue;
            }
        }
        if (endsWith(name, ".tmp") || endsWith(name, ".done") || endsWith(name, ".sp")) {  // .sp = savepoint / statement copy
            removeTreeQuietly(root / pathOf(name));
        } else if (isSafeFileName(name)) {  // anything else was not put here by a transaction
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
Value intValue(int64_t n) { return Value(n); }

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

// ENUM('a','b'): the value must be one of the listed members (text compare).
void checkEnum(const Column& col, const Value& value) {
    if (col.enumValues.empty() || !isTextValue(value)) return;
    const std::string& s = std::get<std::string>(value.data);
    if (std::find(col.enumValues.begin(), col.enumValues.end(), s) != col.enumValues.end()) return;
    std::string members;
    for (size_t i = 0; i < col.enumValues.size(); ++i) members += (i ? ", " : "") + pyRepr(col.enumValues[i]);
    throw ExecutionError("Column '" + col.name + "' ENUM(" + members + ") hai, par value " + pyRepr(s) +
                         " mili -- sirf inhi mein se ek chalegi");
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
    StackBase base;
    auto statements = parseScript(text);
    std::vector<Result> results;
    for (const auto& stmt : statements) results.push_back(guarded(*stmt));
    return results;
}

std::vector<Result> Engine::runScript(const std::string& text) {
    StackBase base;
    std::vector<std::unique_ptr<ast::Statement>> statements;
    try {
        statements = parseScript(text);
    } catch (const MeraDBError& e) {
        Result r;
        r.error = e.formatted();
        return {r};
    }
    std::vector<Result> results;
    for (const auto& stmt : statements) {
        try {
            results.push_back(guarded(*stmt));
        } catch (const MeraDBError& e) {
            Result r;
            r.error = e.formatted();
            results.push_back(std::move(r));
        }
    }
    return results;
}

Result Engine::executeStatement(const ast::Statement& stmt) {
    auto guard = acquireLock(instance_->lockTimeoutSeconds);  // held until the statement is on the disk
    StatementScope durable(instance_->durability());
    Result result = runStatement(stmt);
    durable.finish();
    return result;
}

Result Engine::runStatement(const ast::Statement& stmt) {
    StackBase base;
    requireStack();
    ScopedSessionHost sessionHost(this);  // @variables and stored functions, for every expression this statement evaluates
    // Hold the instance lock for the statement (waiting at most lockTimeoutSeconds).
    auto guard = acquireLock(instance_->lockTimeoutSeconds);
    if (!fs::is_directory(pathOf(instance_->dbDir(currentDb)))) {
        std::string gone = currentDb;
        currentDb = DEFAULT_DATABASE;
        throw ExecutionError("Database '" + gone + "' ab exist nahi karta. Ab '" + DEFAULT_DATABASE + "' use ho raha hai.");
    }
    if (user && !rolesInit_) {  // a session starts with the roles SET DEFAULT ROLE chose for the user (none chosen = all)
        rolesInit_ = true;
        if (auto defaults = instance_->users().defaultRoles(*user)) activeRoles = std::set<std::string>(defaults->begin(), defaults->end());
    }
    checkPrivileges(stmt);
    using namespace ast;
    if (functionDepth_ > 0 && !isReadOnlyStatement(stmt))
        throw ExecutionError("Function ke andar sirf padhne wali statements (DIKHAO) chal sakti hain -- data ya schema badalna mana hai");
    // SET autocommit = 0: the first statement that changes something starts a transaction, kept open until PAKKA / WAPAS
    if (!autocommit_ && !inTransaction() && startsTransaction(stmt)) {
        beginTransaction();
        implicitTxn_ = true;
    }
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
    if (auto* s = dynamic_cast<const AlterModifyColumn*>(&stmt)) return execAlterModifyColumn(*s);
    if (auto* s = dynamic_cast<const AlterAddForeignKey*>(&stmt)) return execAlterAddForeignKey(*s);
    if (auto* s = dynamic_cast<const AlterDropForeignKey*>(&stmt)) return execAlterDropForeignKey(*s);
    if (auto* s = dynamic_cast<const RenameTable*>(&stmt)) return execRenameTable(*s);
    if (auto* s = dynamic_cast<const RenameColumn*>(&stmt)) return execRenameColumn(*s);
    if (auto* s = dynamic_cast<const Begin*>(&stmt)) return execBegin(*s);
    if (auto* s = dynamic_cast<const Commit*>(&stmt)) return execCommit(*s);
    if (auto* s = dynamic_cast<const Rollback*>(&stmt)) return execRollback(*s);
    if (auto* s = dynamic_cast<const Savepoint*>(&stmt)) return execSavepoint(*s);
    if (auto* s = dynamic_cast<const RollbackToSavepoint*>(&stmt)) return execRollbackToSavepoint(*s);
    if (auto* s = dynamic_cast<const ReleaseSavepoint*>(&stmt)) return execReleaseSavepoint(*s);
    if (auto* s = dynamic_cast<const SetVariable*>(&stmt)) return execSetVariable(*s);
    if (auto* s = dynamic_cast<const ShowVariables*>(&stmt)) return execShowVariables(*s);
    if (auto* s = dynamic_cast<const Prepare*>(&stmt)) return execPrepare(*s);
    if (auto* s = dynamic_cast<const ExecutePrepared*>(&stmt)) return execExecutePrepared(*s);
    if (auto* s = dynamic_cast<const Deallocate*>(&stmt)) return execDeallocate(*s);
    if (auto* s = dynamic_cast<const CreateFunction*>(&stmt)) return execCreateFunction(*s);
    if (auto* s = dynamic_cast<const DropFunction*>(&stmt)) return execDropFunction(*s);
    if (auto* s = dynamic_cast<const ShowFunctions*>(&stmt)) return execShowFunctions(*s);
    if (auto* s = dynamic_cast<const Insert*>(&stmt)) return guardSignal(s->table, [&] { return execInsert(*s); });
    if (auto* s = dynamic_cast<const Update*>(&stmt)) return guardSignal(s->table, [&] { return execUpdate(*s); });
    if (auto* s = dynamic_cast<const Delete*>(&stmt)) return guardSignal(s->table, [&] { return execDelete(*s); });
    if (auto* s = dynamic_cast<const Select*>(&stmt)) return execSelect(*s);
    if (auto* s = dynamic_cast<const SetOp*>(&stmt)) return execSetOp(*s);
    if (auto* s = dynamic_cast<const CreateView*>(&stmt)) return execCreateView(*s);
    if (auto* s = dynamic_cast<const DropView*>(&stmt)) return execDropView(*s);
    if (auto* s = dynamic_cast<const ShowViews*>(&stmt)) return execShowViews(*s);
    if (auto* s = dynamic_cast<const CreateIndex*>(&stmt)) return execCreateIndex(*s);
    if (auto* s = dynamic_cast<const DropIndex*>(&stmt)) return execDropIndex(*s);
    if (auto* s = dynamic_cast<const ShowIndex*>(&stmt)) return execShowIndex(*s);
    if (auto* s = dynamic_cast<const SetDefaultRole*>(&stmt)) return execSetDefaultRole(*s);
    if (auto* s = dynamic_cast<const SetIsolation*>(&stmt)) return execSetIsolation(*s);
    if (auto* s = dynamic_cast<const ShowDatabases*>(&stmt)) return execShowDatabases(*s);
    if (auto* s = dynamic_cast<const ShowTriggers*>(&stmt)) return execShowTriggers(*s);
    if (auto* s = dynamic_cast<const ShowProcedures*>(&stmt)) return execShowProcedures(*s);
    if (auto* s = dynamic_cast<const ShowGrants*>(&stmt)) return execShowGrants(*s);
    if (auto* s = dynamic_cast<const ShowCreateTable*>(&stmt)) return execShowCreateTable(*s);
    if (auto* s = dynamic_cast<const ShowCreateView*>(&stmt)) return execShowCreateView(*s);
    if (auto* s = dynamic_cast<const CreateRole*>(&stmt)) return execCreateRole(*s);
    if (auto* s = dynamic_cast<const DropRole*>(&stmt)) return execDropRole(*s);
    if (auto* s = dynamic_cast<const GrantRole*>(&stmt)) return execGrantRole(*s);
    if (auto* s = dynamic_cast<const RevokeRole*>(&stmt)) return execRevokeRole(*s);
    if (auto* s = dynamic_cast<const SetRole*>(&stmt)) return execSetRole(*s);
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
    if (user && !rolesInit_) {  // the same start as executeStatement: this may be the session's first request
        rolesInit_ = true;
        if (auto defaults = instance_->users().defaultRoles(*user)) activeRoles = std::set<std::string>(defaults->begin(), defaults->end());
    }
    ordered_json tree = ordered_json::array();
    for (const auto& db : instance_->databases()) {
        Catalog& cat = instance_->catalog(db);
        std::vector<std::string> names;
        for (const auto& entry : cat.tables) names.push_back(entry.first);
        std::sort(names.begin(), names.end());  // Python: sorted(catalog.tables)
        ordered_json tables = ordered_json::array();
        for (const auto& name : names) {
            // A restricted user sees only what a grant lets it name: the tables it holds a privilege on, and on a
            // column-limited table only those columns.
            Access held;
            if (user) {
                for (const auto& privilege : allPrivileges()) {
                    Access one = instance_->users().access(*user, activeRoles, db, name, privilege);
                    held.full = held.full || one.full;
                    held.columns.insert(one.columns.begin(), one.columns.end());
                }
                if (!held.any()) continue;
            }
            ordered_json columns = ordered_json::array();
            for (const auto& col : cat.tables.at(name).columns)
                if (!user || held.covers(col.name)) columns.push_back(col.toJson());
            ordered_json table = ordered_json::object();
            table["name"] = name;
            table["columns"] = std::move(columns);
            tables.push_back(std::move(table));
        }
        if (user && tables.empty() && db != currentDb) continue;  // not even the name of a database it has nothing in
        ordered_json entry = ordered_json::object();
        entry["name"] = db;
        entry["current"] = (db == currentDb);
        entry["tables"] = std::move(tables);
        tree.push_back(std::move(entry));
    }
    return tree;
}

void Engine::close() {
    if (!inTransaction()) return;
    try {
        executeStatement(ast::Rollback());
    } catch (const std::exception&) {  // MeraDBError, or a filesystem_error out of the snapshot restore
    }
    // Whatever happened, a closing session must not keep the shared lock: the
    // snapshot stays on disk and crash recovery undoes the transaction next start.
    if (inTransaction() && txnThread_ == std::this_thread::get_id()) {
        txnDb.reset();
        instance_->lock.unlock();
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
    // A derived table (subquery in FROM) contributes the tables ITS query reads.
    std::vector<std::string> tables;
    auto add = [&](const std::string& name, const ast::Select* derived) {
        if (derived == nullptr) {
            if (!name.empty()) tables.push_back(name);  // DIKHAO 1 + 1 has no table at all
            return;
        }
        for (auto& inner : tablesRead(*derived)) tables.push_back(std::move(inner));
    };
    add(stmt.table, stmt.derived.get());
    for (const auto& join : stmt.joins) add(join.table, join.derived.get());
    return tables;
}

namespace {

// Visits the column references, `*` items and sub-selects of an expression tree. A sub-select is reported, not entered:
// it has tables of its own.
struct ExprVisitor {
    virtual ~ExprVisitor() = default;
    virtual void ref(ast::ColumnRef&) {}
    virtual void star(ast::Star&) {}
    virtual void sub(ast::Select&) {}
};

void walkExpr(ast::Expr* e, ExprVisitor& v) {
    if (e == nullptr) return;
    requireStack();
    using namespace ast;
    if (auto* c = dynamic_cast<ColumnRef*>(e)) v.ref(*c);
    else if (auto* s = dynamic_cast<Star*>(e)) v.star(*s);
    else if (auto* b = dynamic_cast<BinaryOp*>(e)) { walkExpr(b->left.get(), v); walkExpr(b->right.get(), v); }
    else if (auto* u = dynamic_cast<UnaryOp*>(e)) walkExpr(u->operand.get(), v);
    else if (auto* n = dynamic_cast<IsNull*>(e)) walkExpr(n->expr.get(), v);
    else if (auto* f = dynamic_cast<FuncCall*>(e)) {
        if (!dynamic_cast<Star*>(f->arg.get())) walkExpr(f->arg.get(), v);  // COUNT(*) reads no column
    } else if (auto* sf = dynamic_cast<ScalarFunc*>(e)) { for (auto& a : sf->args) walkExpr(a.get(), v); }
    else if (auto* co = dynamic_cast<Coalesce*>(e)) { for (auto& a : co->args) walkExpr(a.get(), v); }
    else if (auto* cw = dynamic_cast<CaseWhen*>(e)) {
        for (auto& [cond, value] : cw->branches) { walkExpr(cond.get(), v); walkExpr(value.get(), v); }
        walkExpr(cw->elseExpr.get(), v);
    } else if (auto* sq = dynamic_cast<Subquery*>(e)) {
        if (sq->statement) v.sub(*sq->statement);
    } else if (auto* in = dynamic_cast<InSubquery*>(e)) {
        walkExpr(in->left.get(), v);
        if (in->subquery && in->subquery->statement) v.sub(*in->subquery->statement);
    }
}

struct RefCollector : ExprVisitor {
    std::vector<const ast::ColumnRef*> refs;
    std::vector<const ast::Star*> stars;
    std::vector<const ast::Select*> subs;
    void ref(ast::ColumnRef& r) override { refs.push_back(&r); }
    void star(ast::Star& s) override { stars.push_back(&s); }
    void sub(ast::Select& s) override { subs.push_back(&s); }
    void add(const ast::Expr* e) { walkExpr(const_cast<ast::Expr*>(e), *this); }
};

// Calls f(ColumnRef&) for every column reference of an expression owned by the caller.
template <typename F>
void forEachRef(ast::Expr* e, F f) {
    struct Visitor : ExprVisitor {
        F& f;
        explicit Visitor(F& fn) : f(fn) {}
        void ref(ast::ColumnRef& r) override { f(r); }
    } visitor(f);
    walkExpr(e, visitor);
}

}  // namespace

// The tables of one SELECT as the privilege check sees them (innermost query first, `outer` = the query around it).
struct Engine::PrivScope {
    struct Source {
        std::string alias, table;      // table is empty for a derived table
        Access access;                 // what the session may DIKHAO there
        std::vector<std::string> columns;
        bool unknown = false;          // a view or derived table: its columns are not known here
    };
    std::vector<Source> sources;
    const PrivScope* outer = nullptr;
};

Access Engine::accessOf(const std::string& privilege, const std::string& table) {
    return instance_->users().access(*user, activeRoles, currentDb, table, privilege);
}

void Engine::requirePrivilege(const std::string& privilege, const std::string& table) {
    if (!accessOf(privilege, table).full)
        throw ExecutionError("'" + *user + "' ko table '" + table + "' par " + privilege + " ka adhikar nahi hai");
}

Access Engine::requireAccess(const std::string& privilege, const std::string& table) {
    Access access = accessOf(privilege, table);
    if (!access.any())
        throw ExecutionError("'" + *user + "' ko table '" + table + "' par " + privilege + " ka adhikar nahi hai");
    return access;
}

void Engine::denyColumn(const std::string& privilege, const std::string& table, const std::string& column) const {
    throw ExecutionError("'" + *user + "' ko table '" + table + "' ke column '" + column + "' par " + privilege +
                         " ka adhikar nahi hai");
}

// DIKHAO needs DIKHAO on every table it reads (FROM, MILAO, derived tables and sub-selects). If a table is granted only
// column by column, the select list, JAHAN, SAMOOH, JINKA, KRAM and the MILAO conditions may name nothing else, and `*`
// needs every column.
void Engine::checkSelectAccess(const ast::Select& stmt, const PrivScope* outer) {
    PrivScope scope;
    scope.outer = outer;
    auto addSource = [&](const std::string& table, const std::string& alias, const ast::Select* derived) {
        PrivScope::Source s;
        if (derived != nullptr) {
            checkSelectAccess(*derived, outer);  // a derived table cannot see the query around it
            s.alias = alias;
            s.unknown = true;
            s.access.full = true;
            scope.sources.push_back(std::move(s));
            return;
        }
        if (table.empty()) return;  // DIKHAO 1 + 1 has no table at all
        s.alias = alias.empty() ? table : alias;
        s.table = table;
        s.access = requireAccess("DIKHAO", table);
        if (const TableSchema* schema = catalog().find(table)) s.columns = schema->columnNames();
        else s.unknown = true;  // a view
        scope.sources.push_back(std::move(s));
    };
    addSource(stmt.table, stmt.alias.value_or(stmt.table), stmt.derived.get());
    for (const auto& join : stmt.joins) addSource(join.table, join.alias, join.derived.get());

    RefCollector strict, lenient;  // lenient: JINKA / KRAM may name an output alias (KAHO)
    for (const auto& c : stmt.columns) strict.add(c.get());
    strict.add(stmt.where.get());
    for (const auto& g : stmt.groupBy) strict.add(g.get());
    for (const auto& join : stmt.joins) strict.add(join.on.get());
    lenient.add(stmt.having.get());
    for (const auto& o : stmt.orderBy) lenient.add(o.expr.get());
    for (const ast::Select* sub : strict.subs) checkSelectAccess(*sub, &scope);
    for (const ast::Select* sub : lenient.subs) checkSelectAccess(*sub, &scope);

    bool restricted = false;  // the query around this one counts too: a correlated sub-select reads its row
    for (const PrivScope* sc = &scope; sc != nullptr; sc = sc->outer)
        for (const auto& s : sc->sources)
            if (!s.access.full) restricted = true;
    if (!restricted) return;

    std::set<std::string> outputNames;
    for (const auto& a : stmt.aliases)
        if (a) outputNames.insert(*a);
    auto resolve = [&](const ast::ColumnRef& ref) {
        for (const PrivScope* sc = &scope; sc != nullptr; sc = sc->outer) {
            std::vector<const PrivScope::Source*> hits;
            for (const auto& s : sc->sources) {
                if (ref.table) {
                    if (s.alias == *ref.table || (!s.table.empty() && s.table == *ref.table)) hits.push_back(&s);
                } else if (!s.unknown && std::find(s.columns.begin(), s.columns.end(), ref.name) != s.columns.end()) {
                    hits.push_back(&s);
                }
            }
            if (hits.empty() && !ref.table)
                for (const auto& s : sc->sources)
                    if (s.unknown) hits.push_back(&s);
            if (hits.empty()) continue;  // maybe a column of the query around this one
            for (const auto* h : hits)
                if (!h->access.covers(ref.name)) denyColumn("DIKHAO", h->table.empty() ? h->alias : h->table, ref.name);
            return;
        }
    };
    for (const ast::Star* star : strict.stars) {
        for (const auto& s : scope.sources) {
            if (star->table && s.alias != *star->table && s.table != *star->table) continue;
            if (s.access.full) continue;
            const std::string shown = s.table.empty() ? s.alias : s.table;
            if (s.unknown) denyColumn("DIKHAO", shown, "*");
            for (const auto& c : s.columns)
                if (!s.access.covers(c)) denyColumn("DIKHAO", shown, c);
        }
    }
    for (const ast::ColumnRef* ref : strict.refs) resolve(*ref);
    // An output alias only excuses a name that is not also a real column: KRAM prefers the real column, so
    // `DIKHAO id KAHO secret SE t KRAM secret` would reveal the order of the hidden values.
    auto isRealColumn = [&](const std::string& name) {
        for (const auto& s : scope.sources)
            if (std::find(s.columns.begin(), s.columns.end(), name) != s.columns.end()) return true;
        return false;
    };
    for (const ast::ColumnRef* ref : lenient.refs)
        if (ref->table || !outputNames.count(ref->name) || isRealColumn(ref->name)) resolve(*ref);
}

// Sub-selects hidden inside an expression (JAHAN x MEIN (DIKHAO ...), a value, ...) read tables too.
void Engine::checkExprSubqueries(const ast::Expr* expr) {
    if (expr == nullptr) return;
    RefCollector found;
    found.add(expr);
    for (const ast::Select* sub : found.subs) checkSelectAccess(*sub, nullptr);
}

// A session that is not a superuser may grant a privilege only if it holds it WITH GRANT OPTION -- and, for a column
// grant, on at least those columns.
void Engine::checkGrantAllowed(const ast::Grant& stmt) {
    // holding no grant option at all is the plain "superuser only" refusal; holding some but not enough says what is missing
    bool holdsAny = false;
    for (const auto& priv : stmt.privileges)
        if (instance_->users().access(*user, activeRoles, currentDb, stmt.table, priv, true).any()) holdsAny = true;
    if (!holdsAny)
        throw ExecutionError("'" + *user + "' superuser nahi hai -- '" + astClassName(stmt) +
                             "' jaisa DDL/admin command sirf superuser (bina username connect kiya session) chala sakta hai");
    for (const auto& priv : stmt.privileges) {
        Access held = instance_->users().access(*user, activeRoles, currentDb, stmt.table, priv, true);
        auto limited = stmt.columns.find(priv);
        bool allowed = held.full;
        if (!allowed && limited != stmt.columns.end() && !limited->second.empty()) {
            allowed = true;
            for (const auto& c : limited->second)
                if (!held.covers(c)) allowed = false;
        }
        if (!allowed)
            throw ExecutionError("'" + *user + "' ke paas '" + stmt.table + "' par " + priv +
                                 " ka WITH GRANT OPTION nahi hai -- jo adhikar apne paas grant option ke saath nahi, wo kisi ko de nahi sakte");
    }
}

void Engine::checkPrivileges(const ast::Statement& stmt) {
    if (!user) return;  // superuser: checking is skipped entirely
    using namespace ast;
    // BADLO / MITAO expressions read the row they change. A user whose DIKHAO is limited to some columns must not get at
    // the others through them: `RAKHO naam = CONCAT('', secret)` copies a hidden value somewhere readable, and
    // `JAHAN secret > 5` tells it apart by the rows it touches. Holding no DIKHAO at all stays allowed: nothing is readable.
    auto checkColumnsReadable = [this](const std::string& table, const Expr* expr) {
        Access read = accessOf("DIKHAO", table);
        if (read.full || !read.any() || expr == nullptr) return;
        PrivScope scope;
        PrivScope::Source source;
        source.alias = source.table = table;
        source.access = read;
        if (const TableSchema* schema = catalog().find(table)) source.columns = schema->columnNames();
        else source.unknown = true;
        scope.sources.push_back(std::move(source));
        RefCollector found;
        found.add(expr);
        for (const ColumnRef* ref : found.refs)
            if (!ref->table || *ref->table == table)
                if (!read.covers(ref->name)) denyColumn("DIKHAO", table, ref->name);
        for (const Select* sub : found.subs) checkSelectAccess(*sub, &scope);  // a correlated sub-select reads the row too
    };
    if (auto* ex = dynamic_cast<const Explain*>(&stmt)) {
        // A plan never runs the query, so nothing re-checks the sides of a set operation later: check them here.
        auto checkExplained = [this](auto& self, const Statement& inner) -> void {
            if (auto* setOp = dynamic_cast<const SetOp*>(&inner)) {
                self(self, *setOp->left);
                self(self, *setOp->right);
            } else {
                checkPrivileges(inner);
            }
        };
        checkExplained(checkExplained, *ex->statement);
        return;
    }
    if (dynamic_cast<const SetOp*>(&stmt)) return;  // execSetOp re-enters executeStatement for each side
    if (auto* sel = dynamic_cast<const Select*>(&stmt)) {
        checkSelectAccess(*sel, nullptr);
        return;
    }
    if (auto* ins = dynamic_cast<const Insert*>(&stmt)) {
        Access access = requireAccess("DAALO", ins->table);
        if (!access.full) {  // column-level INSERT: every column being filled must be granted
            std::vector<std::string> cols;
            if (ins->columns.has_value() && !ins->columns->empty()) cols = *ins->columns;
            else if (const TableSchema* schema = catalog().find(ins->table)) cols = schema->columnNames();
            else cols = {"*"};
            for (const auto& c : cols)
                if (!access.covers(c)) denyColumn("DAALO", ins->table, c);
        }
        if (ins->replace) requirePrivilege("MITAO", ins->table);  // REPLACE deletes the rows it collides with
        if (ins->select) checkSelectAccess(*ins->select, nullptr);
        for (const auto& row : ins->rows)
            for (const auto& e : row) checkExprSubqueries(e.get());
        if (ins->onConflictUpdate) {  // TAKRAAV PAR BADLO changes an existing row: that is a BADLO, not a DAALO
            Access update = requireAccess("BADLO", ins->table);
            for (const auto& assignment : *ins->onConflictUpdate) {
                if (!update.covers(assignment.first)) denyColumn("BADLO", ins->table, assignment.first);
                checkExprSubqueries(assignment.second.get());
                checkColumnsReadable(ins->table, assignment.second.get());
            }
        }
        return;
    }
    if (auto* upd = dynamic_cast<const Update*>(&stmt)) {
        Access access = requireAccess("BADLO", upd->table);
        if (!access.full)
            for (const auto& assignment : upd->assignments)
                if (!access.covers(assignment.first)) denyColumn("BADLO", upd->table, assignment.first);
        for (const auto& assignment : upd->assignments) {
            checkExprSubqueries(assignment.second.get());
            checkColumnsReadable(upd->table, assignment.second.get());
        }
        checkExprSubqueries(upd->where.get());
        checkColumnsReadable(upd->table, upd->where.get());
        return;
    }
    if (auto* del = dynamic_cast<const Delete*>(&stmt)) {
        requirePrivilege("MITAO", del->table);
        checkExprSubqueries(del->where.get());
        checkColumnsReadable(del->table, del->where.get());
        return;
    }
    // Open to every session: choosing the database (grants are per database, so nothing is exposed), SET ROLE, and
    // SHOW GRANTS for yourself.
    if (dynamic_cast<const UseDatabase*>(&stmt) || dynamic_cast<const SetRole*>(&stmt) || dynamic_cast<const SetIsolation*>(&stmt)) return;
    // Session variables and prepared statements are open too: EXECUTE re-enters executeStatement, so the statement it
    // runs is checked like any other. SET autocommit stays a transaction command (superuser only).
    if (dynamic_cast<const ShowVariables*>(&stmt) || dynamic_cast<const Prepare*>(&stmt) ||
        dynamic_cast<const ExecutePrepared*>(&stmt) || dynamic_cast<const Deallocate*>(&stmt))
        return;
    if (auto* set = dynamic_cast<const SetVariable*>(&stmt)) {
        bool onlyUserVariables = true;
        for (const auto& assignment : set->assignments)
            if (assignment.first.empty() || assignment.first[0] != '@') onlyUserVariables = false;
        if (onlyUserVariables) return;
    }
    if (auto* shown = dynamic_cast<const ShowGrants*>(&stmt)) {
        if (!shown->user.has_value() || *shown->user == *user) return;
    }
    // Passing a privilege on needs WITH GRANT OPTION (checked here, executed by execGrant)
    if (auto* grant = dynamic_cast<const Grant*>(&stmt)) {
        checkGrantAllowed(*grant);
        return;
    }
    // Everything else: DDL, VIEW/TRIGGER/PROCEDURE management, users, roles and revokes, transactions.
    throw ExecutionError("'" + *user + "' superuser nahi hai -- '" + astClassName(stmt) +
                         "' jaisa DDL/admin command sirf superuser (bina username connect kiya session) chala sakta hai");
}

Result Engine::execCreateUser(const ast::CreateUser& stmt) {
    if (stmt.ifNotExists && instance_->users().exists(stmt.name))
        return messageResult("User '" + stmt.name + "' pehle se hai -- kuch nahi badla");
    instance_->users().create(stmt.name, stmt.password);
    return messageResult("User '" + stmt.name + "' ban gaya");
}

Result Engine::execDropUser(const ast::DropUser& stmt) {
    if (stmt.ifExists && !instance_->users().exists(stmt.name))
        return messageResult("User '" + stmt.name + "' nahi hai -- kuch nahi badla");
    instance_->users().drop(stmt.name);
    return messageResult("User '" + stmt.name + "' hata diya");
}

namespace {
// "DIKHAO (a, b), DAALO": the privileges of a GRANT / REVOKE with their column lists
std::string privilegeText(const std::vector<std::string>& privileges,
                          const std::map<std::string, std::vector<std::string>>& columns) {
    std::string out;
    for (const auto& p : privileges) {
        if (!out.empty()) out += ", ";
        out += p;
        auto limited = columns.find(p);
        if (limited != columns.end() && !limited->second.empty()) out += " (" + joinStrs(limited->second, ", ") + ")";
    }
    return out;
}
}  // namespace

Result Engine::execGrant(const ast::Grant& stmt) {
    if (const TableSchema* schema = catalog().find(stmt.table))
        for (const auto& entry : stmt.columns)
            for (const auto& c : entry.second) schema->indexOf(c);  // raises if the column does not exist
    instance_->users().grant(stmt.user, currentDb, stmt.table, stmt.privileges, stmt.columns, stmt.withGrantOption);
    return messageResult("'" + stmt.user + "' ko '" + currentDb + "." + stmt.table + "' par " +
                         privilegeText(stmt.privileges, stmt.columns) + " ka adhikar mil gaya" +
                         (stmt.withGrantOption ? " (WITH GRANT OPTION)" : ""));
}

Result Engine::execRevoke(const ast::Revoke& stmt) {
    instance_->users().revoke(stmt.user, currentDb, stmt.table, stmt.privileges, stmt.columns);
    return messageResult("'" + stmt.user + "' se '" + currentDb + "." + stmt.table + "' par " +
                         privilegeText(stmt.privileges, stmt.columns) + " ka adhikar wapas le liya");
}

// ---- roles ----

Result Engine::execCreateRole(const ast::CreateRole& stmt) {
    if (stmt.name == "none" || stmt.name == "all" || stmt.name == "default")
        throw ExecutionError("'" + stmt.name + "' role ka naam nahi ho sakta (SET ROLE mein iska alag matlab hai)");
    try {
        instance_->users().createRole(stmt.name);
    } catch (const ExecutionError& e) {
        if (!stmt.ifNotExists || std::string(e.what()).find("Role '") == std::string::npos) throw;
        return messageResult("Role '" + stmt.name + "' pehle se hai -- kuch nahi badla");
    }
    return messageResult("Role '" + stmt.name + "' ban gaya");
}

Result Engine::execDropRole(const ast::DropRole& stmt) {
    try {
        instance_->users().dropRole(stmt.name);
    } catch (const ExecutionError& e) {
        if (!stmt.ifExists) throw;
        return messageResult("Role '" + stmt.name + "' nahi hai -- kuch nahi badla");
    }
    if (activeRoles) activeRoles->erase(stmt.name);
    return messageResult("Role '" + stmt.name + "' hata diya");
}

Result Engine::execGrantRole(const ast::GrantRole& stmt) {
    for (const auto& role : stmt.roles) instance_->users().grantRole(role, stmt.user);
    return messageResult("Role " + joinStrs(stmt.roles, ", ") + " ab '" + stmt.user + "' ko mil gaya");
}

Result Engine::execRevokeRole(const ast::RevokeRole& stmt) {
    for (const auto& role : stmt.roles) instance_->users().revokeRole(role, stmt.user);
    return messageResult("Role " + joinStrs(stmt.roles, ", ") + " '" + stmt.user + "' se wapas le liya");
}

// SET DEFAULT ROLE ALL | NONE | r [, r] TO user: kept in users.json; a session of that user starts with them active.
Result Engine::execSetDefaultRole(const ast::SetDefaultRole& stmt) {
    if (stmt.all) {
        instance_->users().setDefaultRoles(stmt.user, std::nullopt);
        return messageResult("'" + stmt.user + "' ke saare granted roles ab shuru se active honge");
    }
    std::vector<std::string> chosen = stmt.none ? std::vector<std::string>() : stmt.roles;
    std::vector<std::string> granted = instance_->users().rolesOf(stmt.user);
    for (const auto& role : chosen)
        if (std::find(granted.begin(), granted.end(), role) == granted.end())
            throw ExecutionError("Role '" + role + "' '" + stmt.user + "' ko diya hi nahi gaya");
    instance_->users().setDefaultRoles(stmt.user, chosen);
    return messageResult(stmt.none ? "'" + stmt.user + "' ke liye shuru mein koi role active nahi hoga"
                                   : "'" + stmt.user + "' ka default role ab " + joinStrs(chosen, ", ") + " hai");
}

// SET TRANSACTION ISOLATION LEVEL: remembered and shown by SHOW VARIABLES. MeraDB runs a transaction while holding the
// whole instance lock, so whatever is asked for, a transaction behaves as SERIALIZABLE: no other session can read or
// write in between (no dirty, non-repeatable or phantom reads).
Result Engine::execSetIsolation(const ast::SetIsolation& stmt) {
    isolation_ = stmt.level;
    return messageResult("isolation level " + stmt.level + " likh liya. Dhyan do: MeraDB transaction ke dauraan poora database lock rakhta hai, "
                         "isliye asal mein har transaction SERIALIZABLE chalta hai");
}

// SET ROLE r [, r] | SAB | NONE: picks which of the user's granted roles count for this session.
Result Engine::execSetRole(const ast::SetRole& stmt) {
    if (!user) return messageResult("SET ROLE ka koi asar nahi -- bina username ka session superuser hai, sab adhikar pehle se hain");
    if (stmt.all) {
        activeRoles.reset();
        return messageResult("Ab '" + *user + "' ke saare granted roles active hain");
    }
    if (stmt.none) {
        activeRoles = std::set<std::string>();
        return messageResult("Ab koi role active nahi hai -- sirf '" + *user + "' ke apne adhikar chalenge");
    }
    std::vector<std::string> granted = instance_->users().rolesOf(*user);
    std::set<std::string> chosen;
    for (const auto& role : stmt.roles) {
        if (std::find(granted.begin(), granted.end(), role) == granted.end())
            throw ExecutionError("Role '" + role + "' '" + *user + "' ko diya hi nahi gaya");
        chosen.insert(role);
    }
    activeRoles = std::move(chosen);
    return messageResult("Ab role " + joinStrs(stmt.roles, ", ") + " active hai");
}

// ============================================================================
// triggers (fire once per affected row on DAALO/BADLO/MITAO)
// ============================================================================

Result Engine::execCreateTrigger(const ast::CreateTrigger& stmt) {
    Catalog& cat = catalog();
    if (cat.hasTrigger(stmt.name)) {
        if (stmt.ifNotExists) return messageResult("Trigger '" + stmt.name + "' pehle se hai -- kuch nahi badla (IF NOT EXISTS)");
        throw ExecutionError("Trigger '" + stmt.name + "' pehle se hai");
    }
    if (cat.find(stmt.table) == nullptr)
        throw ExecutionError("Table '" + stmt.table + "' exist nahi karta -- trigger sirf ek REAL table par lag sakta hai");
    checkBodyAllowed(parseProcBody(stmt.bodyText), "TRIGGER", stmt.timing, stmt.event);  // sanity check: the body must parse cleanly
    cat.addTrigger(stmt.name, stmt.timing, stmt.event, stmt.table, stmt.bodyText);
    return messageResult("Trigger '" + stmt.name + "' ban gaya (" + stmt.timing + " " + stmt.event + " PAR " +
                         stmt.table + ")");
}

Result Engine::execDropTrigger(const ast::DropTrigger& stmt) {
    Catalog& cat = catalog();
    if (!cat.hasTrigger(stmt.name)) {
        if (stmt.ifExists) return messageResult("Trigger '" + stmt.name + "' exist nahi karta -- kuch nahi hatana (IF EXISTS)");
        throw ExecutionError("Trigger '" + stmt.name + "' exist nahi karta");
    }
    cat.removeTrigger(stmt.name);
    return messageResult("Trigger '" + stmt.name + "' hata diya");
}

Row Engine::rowDict(const TableSchema& schema, const std::vector<Value>& values) {
    Row row;
    for (size_t i = 0; i < schema.columns.size() && i < values.size(); ++i) row[schema.columns[i].name] = values[i];
    return row;
}

// A trigger body is run by the procedural interpreter (procedural.cpp): NAYA / PURANA are the frame's rows.
void Engine::fireTriggers(const std::string& timing, const std::string& event, const std::string& table,
                          const Row* newRow, const Row* oldRow) {
    fireTriggersImpl(timing, event, table, newRow, nullptr, oldRow,
                     timing == "PEHLE" ? "is statement (REPLACE / TAKRAAV PAR BADLO) mein NAYA badalna supported nahi hai"
                                       : "NAYA sirf PEHLE DAALO / PEHLE BADLO trigger mein badla ja sakta hai");
}

bool Engine::fireBeforeTriggers(const std::string& event, const std::string& table, Row& newRow, const Row* oldRow) {
    return fireTriggersImpl("PEHLE", event, table, &newRow, &newRow, oldRow, "");
}

bool Engine::fireTriggersImpl(const std::string& timing, const std::string& event, const std::string& table,
                              const Row* newView, Row* newWritable, const Row* oldRow, const std::string& cannotWrite) {
    bool changed = false;
    // Copies (Catalog::triggersFor): a body may run DDL that changes the catalog.
    for (const auto& trigger : catalog().triggersFor(timing, event, table)) {
        ProcFrame frame;
        frame.newView = newView;
        frame.newRow = newWritable;
        frame.oldRow = oldRow;
        frame.newError = cannotWrite;
        runBodyTree(trigger.bodyText, frame);
        if (frame.newChanged) changed = true;
    }
    return changed;
}

std::vector<Value> Engine::rowFromDict(const TableSchema& schema, const Row& dict) const {
    std::vector<Value> values;
    for (const auto& col : schema.columns) {
        auto it = dict.find(col.name);
        values.push_back(it == dict.end() ? Value() : it->second);
    }
    return values;
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
    checkBodyAllowed(parseProcBody(stmt.bodyText), "PROCEDURE", "", "");  // sanity check: the body must parse cleanly
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<std::string> modes;
    for (const auto& p : stmt.params) {
        params.emplace_back(p.name, p.typeName);
        modes.push_back(p.mode);
    }
    cat.addProcedure(stmt.name, params, stmt.bodyText, nowText(), modes);
    return messageResult("Procedure '" + stmt.name + "' ban gaya (" + std::to_string(stmt.params.size()) +
                         " parameter(s))");
}

std::string Engine::nowText() {
    std::time_t t = std::time(nullptr);
    char buffer[32] = "";
    if (const std::tm* local = std::localtime(&t)) std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M:%S", local);
    return buffer;
}

Result Engine::execDropProcedure(const ast::DropProcedure& stmt) {
    Catalog& cat = catalog();
    if (!cat.hasProcedure(stmt.name)) {
        if (stmt.ifExists) return messageResult("Procedure '" + stmt.name + "' exist nahi karta -- kuch nahi hatana (IF EXISTS)");
        throw ExecutionError("Procedure '" + stmt.name + "' exist nahi karta");
    }
    cat.removeProcedure(stmt.name);
    return messageResult("Procedure '" + stmt.name + "' hata diya");
}

Result Engine::execCallProcedure(const ast::CallProcedure& stmt) {
    std::vector<const ast::Expr*> args;
    for (const auto& a : stmt.args) args.push_back(a.get());
    return callProcedure(stmt.name, args, nullptr);
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
    if (isReservedDeviceName(stmt.name)) throw ExecutionError("'" + stmt.name + "' Windows ka reserved naam hai -- koi aur naam chuno");
    fs::path path = pathOf(instance_->dbDir(stmt.name));
    if (fs::is_directory(path)) {
        if (stmt.ifNotExists) return messageResult("Database '" + stmt.name + "' pehle se hai -- kuch nahi badla");
        throw ExecutionError("Database '" + stmt.name + "' pehle se hai");
    }
    fs::create_directories(path);
    instance_->durability().markFolder(instance_->dataDir());
    return messageResult("Database '" + stmt.name + "' ban gaya");
}

Result Engine::execDropDatabase(const ast::DropDatabase& stmt) {
    noTransaction("HATAO DATABASE");
    if (stmt.name == DEFAULT_DATABASE)
        throw ExecutionError(std::string("'") + DEFAULT_DATABASE + "' default database hai, use hata nahi sakte");
    fs::path path = pathOf(instance_->dbDir(stmt.name));
    if (!fs::is_directory(path)) {
        if (stmt.ifExists) return messageResult("Database '" + stmt.name + "' nahi hai -- kuch nahi badla");
        throw ExecutionError("Database '" + stmt.name + "' exist nahi karta");
    }
    // Out of sight in ONE rename, then deleted: a crash half-way through a delete must not leave half a database.
    // Recovery deletes a leftover .done.
    fs::path gone = pathOf(instance_->dataDir()) / SNAPSHOT_DIR / pathOf(stmt.name + ".done");
    std::error_code ec;
    fs::create_directories(gone.parent_path(), ec);
    fs::remove_all(gone, ec);
    fs::rename(path, gone, ec);
    if (ec) throw StorageError("Database '" + stmt.name + "' hata nahi paaye: " + ec.message());
    instance_->durability().markFolder(instance_->dataDir());
    crashPoint("drop-database-after-rename");
    fs::remove_all(gone, ec);
    if (ec) throw StorageError("Database '" + stmt.name + "' hata nahi paaye: " + ec.message());
    instance_->forget(stmt.name);
    if (currentDb == stmt.name) currentDb = DEFAULT_DATABASE;
    return messageResult("Database '" + stmt.name + "' hata diya");
}

Result Engine::execUseDatabase(const ast::UseDatabase& stmt) {
    noTransaction("ISTEMAL");
    if (!fs::is_directory(pathOf(instance_->dbDir(stmt.name))))
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
        if (!c.enumValues.empty()) {
            typeDisplay = "ENUM(";
            for (size_t i = 0; i < c.enumValues.size(); ++i) typeDisplay += (i ? "," : "") + pyRepr(c.enumValues[i]);
            typeDisplay += ")";
        }
        std::vector<std::string> flags;
        if (c.primaryKey) flags.push_back("MUKHYA KUNJI");
        if (c.notNull) flags.push_back("ZAROORI");
        if (c.unique) flags.push_back("ANOKHA");
        if (c.autoIncrement) flags.push_back("AUTO_INCREMENT");
        if (c.defaultValue.has_value() && !c.defaultValue->isNull())
            flags.push_back("WARNA " + exprLabel(ast::Literal(*c.defaultValue)));
        if (nonEmpty(c.refTable)) {
            std::string fk = "SANDARBH " + *c.refTable + "(" + c.refColumn.value_or("") + ")";
            // only a chosen name is shown: automatic <table>_ibfk_<n> names (which stay as they are when the table is
            // renamed) are left out, so BATAO stays identical to the Python engine
            size_t ibfk = c.fkName.rfind("_ibfk_");
            bool autoName = ibfk != std::string::npos && ibfk + 6 < c.fkName.size() &&
                            c.fkName.find_first_not_of("0123456789", ibfk + 6) == std::string::npos;
            if (!c.fkName.empty() && !autoName) fk += " [" + c.fkName + "]";
            if (!c.onDelete.empty()) fk += " ON DELETE " + c.onDelete;
            if (!c.onUpdate.empty()) fk += " ON UPDATE " + c.onUpdate;
            flags.push_back(fk);
        }
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

void Engine::beginTransaction() {
    instance_->takeSnapshot(currentDb);
    // Take the lock ONE EXTRA time and keep it until PAKKA/WAPAS: other
    // sessions now wait, so nobody sees our half-finished changes.
    instance_->lock.lock();
    txnThread_ = std::this_thread::get_id();
    txnDb = currentDb;
    implicitTxn_ = false;
}

// PAKKA / WAPAS: every savepoint copy goes with the transaction.
void Engine::finishTransaction(bool commit) {
    clearSavepoints();
    if (commit) instance_->discardSnapshot(*txnDb);
    else instance_->restoreSnapshot(*txnDb);
    txnDb.reset();
    implicitTxn_ = false;
    instance_->lock.unlock();
}

void Engine::clearSavepoints() {
    if (txnDb) {
        for (const auto& sp : savepoints_) instance_->dropCopy(*txnDb, sp.tag);
    }
    savepoints_.clear();
}

Result Engine::execBegin(const ast::Begin&) {
    if (inTransaction()) {
        // an autocommit = 0 transaction is closed by SHURU (committed), like MySQL's START TRANSACTION
        if (!implicitTxn_ || txnThread_ != std::this_thread::get_id())
            throw ExecutionError("Transaction pehle se chal raha hai (PAKKA ya WAPAS karo)");
        finishTransaction(true);
    }
    beginTransaction();
    return messageResult("Transaction SHURU. PAKKA se save karo, WAPAS se sab undo.");
}

Result Engine::execCommit(const ast::Commit&) {
    if (!inTransaction()) throw ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)");
    if (txnThread_ != std::this_thread::get_id())
        throw ExecutionError("Transaction jis thread ne SHURU kiya, PAKKA bhi wahi kar sakta hai");
    finishTransaction(true);
    return messageResult("Transaction PAKKA -- saare changes save ho gaye");
}

Result Engine::execRollback(const ast::Rollback&) {
    if (!inTransaction()) throw ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)");
    if (txnThread_ != std::this_thread::get_id())
        throw ExecutionError("Transaction jis thread ne SHURU kiya, WAPAS bhi wahi kar sakta hai");
    finishTransaction(false);
    return messageResult("Transaction WAPAS -- saare changes undo ho gaye");
}

// SET autocommit = 1 commits whatever transaction is open (as in MySQL).
void Engine::setAutocommit(bool on) {
    if (on == autocommit_) return;
    autocommit_ = on;
    if (on && inTransaction()) {
        if (txnThread_ != std::this_thread::get_id())
            throw ExecutionError("Transaction jis thread ne SHURU kiya, PAKKA bhi wahi kar sakta hai");
        finishTransaction(true);
    }
}

// The statements that open a transaction under autocommit = 0: everything that writes (data, schema, procedures).
// Reads, SHOW, SET, transaction control and the database-level commands (which refuse to run inside a transaction) do not.
bool Engine::startsTransaction(const ast::Statement& stmt) {
    using namespace ast;
    return dynamic_cast<const Insert*>(&stmt) || dynamic_cast<const Update*>(&stmt) || dynamic_cast<const Delete*>(&stmt) ||
           dynamic_cast<const CreateTable*>(&stmt) || dynamic_cast<const DropTable*>(&stmt) ||
           dynamic_cast<const TruncateTable*>(&stmt) || dynamic_cast<const CompactTable*>(&stmt) ||
           dynamic_cast<const AlterAddComposite*>(&stmt) || dynamic_cast<const AlterAddColumn*>(&stmt) ||
           dynamic_cast<const AlterModifyColumn*>(&stmt) || dynamic_cast<const AlterAddForeignKey*>(&stmt) ||
           dynamic_cast<const AlterDropForeignKey*>(&stmt) || dynamic_cast<const AlterDropColumn*>(&stmt) ||
           dynamic_cast<const RenameTable*>(&stmt) || dynamic_cast<const RenameColumn*>(&stmt) ||
           dynamic_cast<const CreateView*>(&stmt) || dynamic_cast<const DropView*>(&stmt) ||
           dynamic_cast<const CreateIndex*>(&stmt) || dynamic_cast<const DropIndex*>(&stmt) ||
           dynamic_cast<const CreateTrigger*>(&stmt) || dynamic_cast<const DropTrigger*>(&stmt) ||
           dynamic_cast<const CreateProcedure*>(&stmt) || dynamic_cast<const DropProcedure*>(&stmt) ||
           dynamic_cast<const CreateFunction*>(&stmt) || dynamic_cast<const DropFunction*>(&stmt) ||
           dynamic_cast<const CallProcedure*>(&stmt) || dynamic_cast<const ExecutePrepared*>(&stmt) ||
           dynamic_cast<const Savepoint*>(&stmt);
}

bool Engine::isReadOnlyStatement(const ast::Statement& stmt) {
    using namespace ast;
    return dynamic_cast<const Select*>(&stmt) || dynamic_cast<const SetOp*>(&stmt) || dynamic_cast<const Explain*>(&stmt) ||
           dynamic_cast<const ShowTables*>(&stmt) || dynamic_cast<const Describe*>(&stmt) ||
           dynamic_cast<const ShowViews*>(&stmt) || dynamic_cast<const ShowDatabases*>(&stmt) ||
           dynamic_cast<const ShowTriggers*>(&stmt) || dynamic_cast<const ShowProcedures*>(&stmt) ||
           dynamic_cast<const ShowFunctions*>(&stmt) || dynamic_cast<const ShowGrants*>(&stmt) ||
           dynamic_cast<const ShowCreateTable*>(&stmt) || dynamic_cast<const ShowCreateView*>(&stmt) ||
           dynamic_cast<const ShowVariables*>(&stmt) || dynamic_cast<const ShowIndex*>(&stmt);
}

// ---- savepoints: copies of the database folder, taken inside the open transaction
Result Engine::execSavepoint(const ast::Savepoint& stmt) {
    if (!inTransaction()) throw ExecutionError("SAVEPOINT ke liye transaction chahiye -- pehle SHURU karo (ya SET autocommit = 0)");
    for (auto it = savepoints_.begin(); it != savepoints_.end(); ++it) {
        if (it->name == stmt.name) {  // the same name again replaces the old savepoint (it moves to the end)
            instance_->dropCopy(*txnDb, it->tag);
            savepoints_.erase(it);
            break;
        }
    }
    std::string tag = "sv" + std::to_string(++savepointSeq_);
    instance_->takeCopy(*txnDb, tag);
    savepoints_.push_back({stmt.name, tag});
    return messageResult("Savepoint '" + stmt.name + "' set ho gaya");
}

Result Engine::execRollbackToSavepoint(const ast::RollbackToSavepoint& stmt) {
    if (!inTransaction()) throw ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)");
    size_t index = savepoints_.size();
    for (size_t i = 0; i < savepoints_.size(); ++i)
        if (savepoints_[i].name == stmt.name) index = i;
    if (index == savepoints_.size()) throw ExecutionError("Savepoint '" + stmt.name + "' exist nahi karta");
    for (size_t i = index + 1; i < savepoints_.size(); ++i) instance_->dropCopy(*txnDb, savepoints_[i].tag);
    savepoints_.resize(index + 1);  // later savepoints are gone; this one stays
    instance_->restoreCopy(*txnDb, savepoints_[index].tag);
    return messageResult("Savepoint '" + stmt.name + "' tak WAPAS -- uske baad ke changes undo ho gaye (transaction abhi khula hai)");
}

Result Engine::execReleaseSavepoint(const ast::ReleaseSavepoint& stmt) {
    if (!inTransaction()) throw ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)");
    size_t index = savepoints_.size();
    for (size_t i = 0; i < savepoints_.size(); ++i)
        if (savepoints_[i].name == stmt.name) index = i;
    if (index == savepoints_.size()) throw ExecutionError("Savepoint '" + stmt.name + "' exist nahi karta");
    for (size_t i = index; i < savepoints_.size(); ++i) instance_->dropCopy(*txnDb, savepoints_[i].tag);
    savepoints_.resize(index);  // the savepoint and every one set after it
    return messageResult("Savepoint '" + stmt.name + "' hata diya (changes jaise ke taise rahe)");
}

// A statement that fires triggers is all or nothing: ANY error that stops it (a SIGNAL, a duplicate key or a division by
// zero inside a body, the depth limit...) undoes its rows and whatever the bodies wrote. The statement runs between two
// copies of the database folder, and the error puts the copy back. Only the outermost statement does this (a nested one would
// pull the files out from under the statements around it), and only when this statement can fire one: a trigger on its
// table (FK cascades fire none). A view stands for a base table not known here, and a stored function called by the
// statement may write anywhere, so with either of those any trigger in the database counts.
Result Engine::guardSignal(const std::string& table, const std::function<Result()>& run) {
    if (bodyDepth_ > 0 || functionDepth_ > 0) return run();
    Catalog& cat = catalog();
    const bool anyTriggerCounts = cat.find(table) == nullptr || !cat.functions.empty();
    bool mayFire = false;
    for (auto it = cat.triggers.begin(); it != cat.triggers.end() && !mayFire; ++it)
        mayFire = anyTriggerCounts || it.value().value("table", "") == table;
    if (!mayFire) return run();
    const std::string db = currentDb;
    instance_->takeCopy(db, "stmt");
    try {
        Result r = run();
        instance_->dropCopy(db, "stmt");
        return r;
    } catch (...) {
        try {
            instance_->restoreCopy(db, "stmt");
        } catch (const std::exception&) {
        }
        instance_->dropCopy(db, "stmt");
        throw;
    }
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
    column.autoIncrement = def.autoIncrement;
    column.enumValues = def.enumValues;
    column.onDelete = def.onDelete;
    column.onUpdate = def.onUpdate;
    if (column.autoIncrement) {
        if (column.typeName != "INT")
            throw ExecutionError("Column '" + column.name + "': AUTO_INCREMENT sirf INT column par ho sakta hai");
        column.notNull = true;  // a generated number is never KHALI
    }
    // check the WARNA value fits the column type (and VARCHAR(n) length) now, not at the first INSERT
    if (def.defaultValue.has_value() && !def.defaultValue->isNull()) {
        if (column.autoIncrement)
            throw ExecutionError("Column '" + column.name + "': AUTO_INCREMENT ke saath WARNA nahi chalta");
        Value coerced = coerce(*def.defaultValue, column.typeName, column.name);
        checkLength(column, coerced);
        checkEnum(column, coerced);
        column.defaultValue = coerced;
    }
    return column;
}

// One AUTO_INCREMENT per table; ON DELETE / ON UPDATE SET NULL needs a column that may be KHALI.
void Engine::checkSchemaExtras(const TableSchema& schema) const {
    int autos = 0;
    for (const auto& c : schema.columns) {
        if (c.autoIncrement) ++autos;
        if ((c.onDelete == "SET NULL" || c.onUpdate == "SET NULL") && c.isRequired())
            throw ExecutionError("Column '" + c.name +
                                 "': SET NULL ke liye column KHALI ho sakna chahiye (ZAROORI / MUKHYA KUNJI nahi)");
    }
    if (autos > 1) throw ExecutionError("Ek table mein sirf ek AUTO_INCREMENT column ho sakta hai");
}

// Constraint names are unique across the whole database (MySQL's rule).
void Engine::checkFkNameFree(const std::string& name, const std::string& exceptTable) {
    for (const auto& [tableName, other] : catalog().tables) {
        if (tableName == exceptTable) continue;
        for (const auto& c : other.columns)
            if (c.fkName == name)
                throw ExecutionError("Constraint naam '" + name + "' table '" + tableName + "' mein pehle se hai");
    }
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
    if (isReservedDeviceName(stmt.name)) throw ExecutionError("'" + stmt.name + "' Windows ka reserved naam hai -- koi aur naam chuno");
    Catalog& cat = catalog();
    if (cat.find(stmt.name)) {
        if (stmt.ifNotExists) return messageResult("Table '" + stmt.name + "' pehle se hai -- kuch nahi badla");
        throw ExecutionError("Table '" + stmt.name + "' pehle se hai");
    }
    if (cat.views.count(stmt.name))
        throw ExecutionError("'" + stmt.name + "' ek VIEW hai -- table usi naam se nahi ban sakti");
    if (!stmt.likeTable.empty()) return execCreateTableLike(stmt);
    if (stmt.asQuery) return execCreateTableAs(stmt);
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
    // table-level [CONSTRAINT name] FOREIGN KEY (col) SANDARBH parent(col): set on the column, like the inline form
    for (const auto& fk : stmt.foreignKeys) {
        Column& col = schema.columns.at(schema.indexOf(fk.column));
        if (nonEmpty(col.refTable)) throw ExecutionError("Column '" + col.name + "' par pehle se SANDARBH hai");
        col.refTable = fk.refTable;
        col.refColumn = fk.refColumn;
        col.onDelete = fk.onDelete;
        col.onUpdate = fk.onUpdate;
        col.fkName = fk.name;
    }
    std::set<std::string> fkNames;
    for (const auto& col : schema.columns)
        if (!col.fkName.empty()) {
            if (!fkNames.insert(col.fkName).second)
                throw ExecutionError("Constraint naam '" + col.fkName + "' do baar diya hai");
            checkFkNameFree(col.fkName, stmt.name);
        }
    for (const auto& col : schema.columns) {
        if (nonEmpty(col.refTable)) checkFkTarget(col, schema);
        if (nonEmpty(col.check)) checkShartExpr(col, schema);
    }
    checkSchemaExtras(schema);
    schema.assignFkNames();
    HeapFile(cat.tablePath(stmt.name)).create();
    cat.add(schema);
    return messageResult("Table '" + stmt.name + "' ban gaya (" + std::to_string(schema.columns.size()) + " columns)");
}

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

// BANAO TABLE t2 LIKE t1: the same columns and constraints (PK / ANOKHA / ZAROORI / WARNA / SHART / ENUM /
// AUTO_INCREMENT / SANDARBH as written), no rows.
Result Engine::execCreateTableLike(const ast::CreateTable& stmt) {
    Catalog& cat = catalog();
    const TableSchema* source = cat.find(stmt.likeTable);
    if (source == nullptr) {
        if (cat.views.count(stmt.likeTable))
            throw ExecutionError("'" + stmt.likeTable + "' ek VIEW hai -- LIKE sirf table ke saath chalta hai");
        throw ExecutionError("Table '" + stmt.likeTable + "' exist nahi karta");
    }
    TableSchema schema = *source;
    schema.name = stmt.name;
    for (auto& col : schema.columns) col.fkName.clear();  // constraint names are per database: the copy gets its own
    schema.assignFkNames();
    HeapFile(cat.tablePath(stmt.name)).create();
    cat.add(schema);
    return messageResult("Table '" + stmt.name + "' ban gaya ('" + stmt.likeTable + "' jaisa, " +
                         std::to_string(schema.columns.size()) + " columns, 0 rows)");
}

namespace {

bool isPlainIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

// The column type for one output column of a query: a plain column keeps its table's type, NOW() is a DATETIME,
// CAST(..) its target; anything else is read off the values (all numbers -> INT, or FLOAT if any has a fraction).
std::string outputColumnType(const ast::Expr* expr, const Scope* scope, const std::vector<std::vector<Value>>& rows,
                             size_t position, const std::string& label) {
    if (auto* ref = dynamic_cast<const ast::ColumnRef*>(expr); ref && ref->bound && ref->table && scope) {
        for (const auto& [alias, schema] : scope->sources())
            if (alias == *ref->table)
                for (const auto& c : schema.columns)
                    if (c.name == ref->name) return c.typeName;
    }
    if (auto* fn = dynamic_cast<const ast::ScalarFunc*>(expr)) {
        if (fn->name == "NOW") return "DATETIME";
        if (fn->name == "CAST") return fn->castType;
    }
    bool hasInt = false, hasDouble = false, hasBool = false, hasText = false, hasDate = false;
    for (const auto& row : rows) {
        const Value& v = row[position];
        if (v.isNull()) continue;
        if (isBoolValue(v)) hasBool = true;
        else if (isIntValue(v)) hasInt = true;
        else if (isDoubleValue(v)) hasDouble = true;
        else if (isDateValue(v)) hasDate = true;
        else hasText = true;
    }
    int kinds = (hasInt || hasDouble ? 1 : 0) + (hasBool ? 1 : 0) + (hasText ? 1 : 0) + (hasDate ? 1 : 0);
    if (kinds > 1)
        throw ExecutionError("Column '" + label + "' mein alag-alag types ki values hain -- CAST se ek type karo");
    if (hasBool) return "BOOL";
    if (hasDouble) return "FLOAT";
    if (hasInt) return "INT";
    if (hasDate) return "DATE";
    return "TEXT";
}

}  // namespace

// BANAO TABLE t KAHO DIKHAO ...: columns named and typed after the query's output, then its rows; no constraints.
Result Engine::execCreateTableAs(const ast::CreateTable& stmt) {
    Catalog& cat = catalog();
    std::unique_ptr<SelectPlan> plan;
    Result query;
    if (auto* select = dynamic_cast<const ast::Select*>(stmt.asQuery.get())) {
        plan = planSelect(*select);
        query = execSelect(*select);
    } else if (auto* setOp = dynamic_cast<const ast::SetOp*>(stmt.asQuery.get())) {
        query = execSetOp(*setOp);
    } else {
        throw ExecutionError("BANAO TABLE ... KAHO ke baad sirf DIKHAO query aa sakti hai");
    }

    TableSchema schema;
    schema.name = stmt.name;
    std::set<std::string> seen;
    for (size_t i = 0; i < query.columns.size(); ++i) {
        std::string label = query.columns[i];
        auto dot = label.find('.');  // `s.naam` -> naam
        if (dot != std::string::npos && isPlainIdentifier(label.substr(0, dot)) && isPlainIdentifier(label.substr(dot + 1)))
            label = label.substr(dot + 1);
        if (!isPlainIdentifier(label))
            throw ExecutionError("Query ke column '" + query.columns[i] +
                                 "' ko table ka column naam nahi bana sakte -- KAHO se alias do (jaise ... KAHO total)");
        if (!seen.insert(label).second)
            throw ExecutionError("Query mein column '" + label + "' do baar hai -- KAHO se alag naam do");
        Column c;
        c.name = label;
        const ast::Expr* expr = plan && i < plan->outputs.size() ? plan->outputs[i].get() : nullptr;
        c.typeName = outputColumnType(expr, plan ? plan->scope.get() : nullptr, query.rows, i, label);
        schema.columns.push_back(std::move(c));
    }
    // convert every value up front, so a failure leaves nothing behind
    std::vector<std::vector<Value>> rows;
    for (const auto& row : query.rows) {
        std::vector<Value> out;
        for (size_t i = 0; i < row.size(); ++i) out.push_back(coerce(row[i], schema.columns[i].typeName, schema.columns[i].name));
        rows.push_back(std::move(out));
    }
    HeapFile(cat.tablePath(stmt.name)).create();
    try {
        Table created(schema, cat.tablePath(stmt.name), instance_->indexCache(currentDb, stmt.name));
        created.insertMany(rows);
    } catch (...) {
        HeapFile(cat.tablePath(stmt.name)).destroy();
        throw;
    }
    cat.add(schema);
    return messageResult("Table '" + stmt.name + "' ban gaya (" + std::to_string(schema.columns.size()) + " columns, " +
                         std::to_string(rows.size()) + " row(s) daali)");
}

Result Engine::execDropTable(const ast::DropTable& stmt) {
    Catalog& cat = catalog();
    if (stmt.ifExists && !cat.find(stmt.name)) return messageResult("Table '" + stmt.name + "' nahi hai -- kuch nahi badla");
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
    auto before = static_cast<int64_t>(fs::file_size(pathOf(t->heap().path())));
    t->heap().compact();
    t->invalidateIndexes();  // compaction moves rows, so every row id changed
    auto after = static_cast<int64_t>(fs::file_size(pathOf(t->heap().path())));
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
    // where the column goes: last (default), FIRST, or AFTER another column
    size_t position = schema.columns.size();
    if (stmt.position == "FIRST") position = 0;
    else if (stmt.position == "AFTER") position = schema.indexOf(stmt.afterColumn) + 1;
    if (newCol.autoIncrement &&
        std::any_of(schema.columns.begin(), schema.columns.end(), [](const Column& c) { return c.autoIncrement; }))
        throw ExecutionError("Ek table mein sirf ek AUTO_INCREMENT column ho sakta hai");
    if (oldRows.size() > 1 && newCol.isUnique() && hasDefault)
        // every existing row would get the same WARNA value -> duplicates
        throw ExecutionError("Naya ANOKHA column '" + newCol.name + "' sab rows mein ek hi WARNA value nahi le sakta");
    if (!oldRows.empty() && newCol.isRequired() && !hasDefault && !newCol.autoIncrement)
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

    // table-level composite constraints are carried over (they name columns, not positions)
    TableSchema newSchema;
    newSchema.name = schema.name;
    newSchema.columns = schema.columns;
    newSchema.compositeUnique = schema.compositeUnique;
    newSchema.compositePk = schema.compositePk;
    newSchema.columns.insert(newSchema.columns.begin() + static_cast<std::ptrdiff_t>(position), newCol);
    checkSchemaExtras(newSchema);
    if (!newCol.fkName.empty()) checkFkNameFree(newCol.fkName, schema.name);
    newSchema.assignFkNames();
    // existing rows get the WARNA value (or KHALI if there is none); an AUTO_INCREMENT column numbers them 1, 2, 3...
    auto withNew = [&](std::vector<Value> v, size_t rowNumber) {
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(position), newCol.autoIncrement ? Value(static_cast<int64_t>(rowNumber + 1)) : defaultValue);
        return v;
    };
    if (nonEmpty(newCol.check)) {
        checkShartExpr(newCol, newSchema);
        auto expr = parseExpression(*newCol.check);
        auto newNames = newSchema.columnNames();
        for (size_t r = 0; r < oldRows.size(); ++r) {
            auto v = withNew(oldRows[r], r);
            Row row;
            for (size_t i = 0; i < newNames.size(); ++i) row[newNames[i]] = v[i];
            Value result = evaluate(*expr, row);
            if (isBoolValue(result) && !std::get<bool>(result.data))
                throw ExecutionError("SHART toot gayi: (" + *newCol.check + ") -- column '" + newCol.name +
                                     "' (table ki maujooda rows ke liye, WARNA value ke saath)");
        }
    }

    auto types = newSchema.types();
    std::vector<std::vector<uint8_t>> payloads;
    for (size_t r = 0; r < oldRows.size(); ++r) payloads.push_back(encodeRow(withNew(oldRows[r], r), types));
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
    for (const auto& ix : schema.indexes)  // a plain index survives unless it names the dropped column
        if (!ix.unique && std::find(ix.columns.begin(), ix.columns.end(), stmt.column) == ix.columns.end())
            newSchema.indexes.push_back(ix);
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

// SUDHARO TABLE t CHANGE old new type ...  |  MODIFY col type ...
// The new definition replaces the column's type, ZAROORI, WARNA, length, ENUM and AUTO_INCREMENT. MUKHYA KUNJI, ANOKHA,
// SANDARBH and SHART are kept unless the new definition gives its own (SANDARBH / SHART). Existing values are converted
// to the new type (like CAST) and the whole table is re-checked against the new column; any failure changes nothing.
Result Engine::execAlterModifyColumn(const ast::AlterModifyColumn& stmt) {
    auto t = table(stmt.table);
    Catalog& cat = catalog();
    const TableSchema schema = t->schema();
    size_t idx = schema.indexOf(stmt.oldName);
    const Column old = schema.columns[idx];
    Column col = makeColumn(stmt.column);
    bool renamed = col.name != old.name;
    auto names = schema.columnNames();
    if (renamed && std::find(names.begin(), names.end(), col.name) != names.end())
        throw ExecutionError("Column '" + col.name + "' pehle se hai");
    if (renamed)
        for (const auto& other : schema.columns) {
            if (!nonEmpty(other.check)) continue;
            auto refs = columnRefKeys(parseExpression(*other.check).get());
            if (std::find(refs.begin(), refs.end(), old.name) != refs.end())
                throw ExecutionError("Column '" + old.name + "' ka naam badal nahi sakte -- column '" + other.name +
                                     "' ka SHART '(" + *other.check + ")' ise use karta hai. Pehle wo SHART hatao");
        }

    // what the new definition does not say stays as it was
    if (col.primaryKey && !old.primaryKey &&
        (schema.compositePk.has_value() ||
         std::any_of(schema.columns.begin(), schema.columns.end(), [](const Column& c) { return c.primaryKey; })))
        throw ExecutionError("Table mein pehle se MUKHYA KUNJI hai");
    col.primaryKey = col.primaryKey || old.primaryKey;
    col.unique = col.unique || old.unique;
    if (!nonEmpty(col.refTable)) {
        col.refTable = old.refTable;
        col.refColumn = old.refColumn;
        col.fkName = old.fkName;
        col.onDelete = old.onDelete;
        col.onUpdate = old.onUpdate;
    }
    if (!nonEmpty(col.check)) col.check = old.check;
    if (schema.compositePk.has_value() &&
        std::find(schema.compositePk->begin(), schema.compositePk->end(), old.name) != schema.compositePk->end())
        col.notNull = true;

    TableSchema newSchema = schema;
    newSchema.columns[idx] = col;
    if (renamed) {  // composite ANOKHA / MUKHYA KUNJI groups name columns, so they follow the rename
        for (auto& ix : newSchema.indexes)
            for (auto& n : ix.columns)
                if (n == old.name) n = col.name;
        for (auto& group : newSchema.compositeUnique)
            for (auto& n : group)
                if (n == old.name) n = col.name;
        if (newSchema.compositePk)
            for (auto& n : *newSchema.compositePk)
                if (n == old.name) n = col.name;
    }
    bool typeChanged = col.typeName != old.typeName;
    if (typeChanged) {  // a column other columns point at must keep its type
        for (const auto& [otherName, other] : cat.tables)
            for (const auto& c : (otherName == schema.name ? newSchema : other).columns)
                if (c.refTable.has_value() && *c.refTable == schema.name && c.refColumn.has_value() && *c.refColumn == old.name)
                    throw ExecutionError("Column '" + old.name + "' ka type nahi badal sakte -- " + otherName + "." + c.name +
                                         " (SANDARBH) ise use karta hai");
    }
    if (!col.fkName.empty() && col.fkName != old.fkName) checkFkNameFree(col.fkName, schema.name);
    newSchema.assignFkNames();
    col = newSchema.columns[idx];
    if (nonEmpty(col.refTable)) checkFkTarget(col, newSchema);
    if (nonEmpty(col.check)) checkShartExpr(col, newSchema);
    checkSchemaExtras(newSchema);

    // convert and re-check every existing row against the new schema
    std::vector<std::vector<Value>> rows;
    for (auto& [id, values] : t->rows()) {
        (void)id;
        Value& cell = values[idx];
        if (typeChanged && !cell.isNull()) {
            try {
                cell = castValue(cell, col.typeName);
            } catch (const ExecutionError& e) {
                throw ExecutionError("Column '" + old.name + "' ki value " + pyRepr(formatValue(cell)) + " ko " +
                                     col.typeName + " mein badal nahi sakte (" + e.message() + ")");
            }
        }
        try {
            rows.push_back(validateRow(newSchema, values));
        } catch (const ExecutionError& e) {
            throw ExecutionError(std::string("Maujooda data naye column ke liye theek nahi: ") + e.message());
        }
    }
    std::vector<std::vector<size_t>> groups;
    for (size_t i = 0; i < newSchema.columns.size(); ++i)
        if (newSchema.columns[i].isUnique()) groups.push_back({i});
    for (const auto& g : newSchema.compositeUnique) {
        groups.emplace_back();
        for (const auto& n : g) groups.back().push_back(newSchema.indexOf(n));
    }
    if (newSchema.compositePk) {
        groups.emplace_back();
        for (const auto& n : *newSchema.compositePk) groups.back().push_back(newSchema.indexOf(n));
    }
    for (const auto& g : groups) {
        std::unordered_set<std::vector<Value>, ValueVecHash, ValueVecEq> seen;
        for (const auto& values : rows) {
            std::vector<Value> key;
            bool hasNull = false;
            for (size_t p : g) {
                key.push_back(values[p]);
                hasNull = hasNull || values[p].isNull();
            }
            if (hasNull) continue;
            if (!seen.insert(key).second)
                throw ExecutionError("Duplicate value " + pyReprTuple(key) + " column '" + newSchema.columns[g[0]].name +
                                     "' mein -- naye column definition ke baad ye ANOKHA/MUKHYA KUNJI toot jaati hai");
        }
    }

    auto types = newSchema.types();
    std::vector<std::vector<uint8_t>> payloads;
    for (const auto& values : rows) payloads.push_back(encodeRow(values, types));
    t->heap().rewrite(payloads);
    t->invalidateIndexes();
    if (renamed) {  // every SANDARBH pointing at the old name follows it (a self-reference included)
        for (auto& entry : cat.tables)
            for (auto& c : (entry.first == schema.name ? newSchema : entry.second).columns)
                if (c.refTable.has_value() && *c.refTable == schema.name && c.refColumn.has_value() && *c.refColumn == old.name)
                    c.refColumn = col.name;
    }
    cat.add(newSchema);
    return messageResult("Column '" + old.name + "' ab " + (renamed ? "'" + col.name + "' " : std::string()) +
                         (col.enumValues.empty() ? col.typeName : "ENUM") + " hai (" + std::to_string(rows.size()) +
                         " row(s) check ki)");
}

// SUDHARO TABLE t JODO [CONSTRAINT name] FOREIGN KEY (col) SANDARBH parent(col) [ON DELETE ..] [ON UPDATE ..]
Result Engine::execAlterAddForeignKey(const ast::AlterAddForeignKey& stmt) {
    auto t = table(stmt.table);
    Catalog& cat = catalog();
    TableSchema schema = t->schema();
    const ast::ForeignKeyDef& fk = stmt.foreignKey;
    size_t pos = schema.indexOf(fk.column);
    Column& col = schema.columns[pos];
    if (nonEmpty(col.refTable))
        throw ExecutionError("Column '" + col.name + "' par pehle se SANDARBH hai (" + col.fkName + ") -- pehle use hatao");
    col.refTable = fk.refTable;
    col.refColumn = fk.refColumn;
    col.onDelete = fk.onDelete;
    col.onUpdate = fk.onUpdate;
    col.fkName = fk.name;
    if (!fk.name.empty()) checkFkNameFree(fk.name, schema.name);
    for (size_t i = 0; i < schema.columns.size() && !fk.name.empty(); ++i)
        if (i != pos && schema.columns[i].fkName == fk.name)
            throw ExecutionError("Constraint naam '" + fk.name + "' is table mein pehle se hai");
    checkFkTarget(col, schema);
    checkSchemaExtras(schema);
    schema.assignFkNames();
    // every existing value must already be in the parent
    Table withFk(schema, cat.tablePath(stmt.table), instance_->indexCache(currentDb, stmt.table));
    std::vector<std::vector<Value>> rows;
    for (auto& [id, values] : t->rows()) {
        (void)id;
        rows.push_back(std::move(values));
    }
    checkFk(withFk, rows);
    cat.add(schema);
    return messageResult("Table '" + stmt.table + "' mein FOREIGN KEY '" + schema.columns[pos].fkName + "' (" + fk.column +
                         " -> " + fk.refTable + "." + fk.refColumn + ") jod di");
}

// SUDHARO TABLE t HATAO FOREIGN KEY name  |  HATAO CONSTRAINT name
Result Engine::execAlterDropForeignKey(const ast::AlterDropForeignKey& stmt) {
    Catalog& cat = catalog();
    TableSchema schema = cat.get(stmt.table);
    for (auto& col : schema.columns) {
        if (col.fkName != stmt.name || !nonEmpty(col.refTable)) continue;
        col.refTable.reset();
        col.refColumn.reset();
        col.fkName.clear();
        col.onDelete.clear();
        col.onUpdate.clear();
        cat.add(schema);
        return messageResult("Table '" + stmt.table + "' ka FOREIGN KEY '" + stmt.name + "' hata diya");
    }
    throw ExecutionError("Table '" + stmt.table + "' mein FOREIGN KEY '" + stmt.name + "' nahi hai");
}

Result Engine::execRenameTable(const ast::RenameTable& stmt) {
    if (isReservedDeviceName(stmt.newName)) throw ExecutionError("'" + stmt.newName + "' Windows ka reserved naam hai -- koi aur naam chuno");
    Catalog& cat = catalog();
    TableSchema schema = cat.get(stmt.table);  // raises if it doesn't exist
    if (stmt.newName == stmt.table) throw ExecutionError("Naya naam purane naam '" + stmt.table + "' jaisa hi hai");
    if (cat.find(stmt.newName) != nullptr) throw ExecutionError("Table '" + stmt.newName + "' pehle se hai");

    // the row DATA doesn't change, only the file's name -- a rename is atomic
    std::error_code ec;
    fs::rename(pathOf(cat.tablePath(stmt.table)), pathOf(cat.tablePath(stmt.newName)), ec);
    if (ec) throw StorageError("Table file ka naam badal nahi paaye: " + ec.message());
    durable::renamed(cat.tablePath(stmt.newName));
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
    for (auto& ix : schema.indexes)
        for (auto& n : ix.columns)
            if (n == stmt.column) n = stmt.newName;
    for (auto& group : schema.compositeUnique)  // table-level ANOKHA / MUKHYA KUNJI groups name columns too
        for (auto& n : group)
            if (n == stmt.column) n = stmt.newName;
    if (schema.compositePk)
        for (auto& n : *schema.compositePk)
            if (n == stmt.column) n = stmt.newName;
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
        checkEnum(col, value);
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

// SANDARBH, parent side. A DELETE / UPDATE of rows in one table may hit rows in others that point at them; each
// foreign key says what happens (ON DELETE / ON UPDATE): RESTRICT (the default; refuse), CASCADE (delete / follow the
// new value) or SET NULL. The knock-on rows are collected in memory in FkWork -- recursively, so a cascade can cascade
// again -- and nothing is written until every check has passed (fkApply). Triggers do not fire for knock-on rows.
struct Engine::FkWork {
    struct Entry {
        std::unique_ptr<Table> owned;
        Table* table = nullptr;
        std::map<int64_t, std::vector<Value>> original;                  // row id -> values, as on disk
        bool loaded = false;                                             // `original` holds every row
        std::map<int64_t, std::optional<std::vector<Value>>> changes;    // row id -> new values; nullopt = deleted
        std::set<int64_t> cascaded;                                      // rows changed by a cascade (need re-validating)
    };
    std::map<std::string, Entry> tables;
    int depth = 0;
    FkWork(Table* primary, const std::string& name) { tables[name].table = primary; }
};


void Engine::fkDelete(FkWork& work, const std::string& tableName, const std::vector<StoredRow>& rows) {
    requireStack();
    using FkEntry = FkWork::Entry;
    if (++work.depth > kMaxFkDepth)
        throw ExecutionError("SANDARBH CASCADE bahut gehra chal raha hai (limit " + std::to_string(kMaxFkDepth) + ")");
    const TableSchema parent = catalog().get(tableName);
    auto entryFor = [&](const std::string& name) -> FkEntry& {
        FkEntry& e = work.tables[name];
        if (e.table == nullptr) {
            e.owned = table(name);
            e.table = e.owned.get();
        }
        return e;
    };
    FkEntry& pe = entryFor(tableName);
    for (const auto& [id, values] : rows) {
        pe.original.emplace(id, values);
        pe.changes[id] = std::nullopt;
    }
    std::vector<std::string> otherNames;
    for (const auto& entry : catalog().tables) otherNames.push_back(entry.first);
    for (const auto& otherName : otherNames) {
        const TableSchema other = catalog().get(otherName);
        for (const auto& childCol : other.columns) {
            if (!childCol.refTable.has_value() || *childCol.refTable != tableName) continue;
            size_t parentPos = parent.indexOf(childCol.refColumn.value_or(""));
            size_t childPos = other.indexOf(childCol.name);
            ValueSet removed;
            for (const auto& [id, values] : rows) {
                (void)id;
                if (!values[parentPos].isNull()) removed.insert(values[parentPos]);
            }
            if (removed.empty()) continue;
            FkEntry& ce = entryFor(otherName);
            if (!ce.loaded) {
                for (auto& [id, values] : ce.table->rows()) ce.original.emplace(id, std::move(values));
                ce.loaded = true;
            }
            std::vector<StoredRow> hits;
            for (const auto& [id, values] : ce.original) {
                auto change = ce.changes.find(id);
                if (change != ce.changes.end() && !change->second) continue;  // already going away
                const std::vector<Value>& current = change != ce.changes.end() ? *change->second : values;
                if (!current[childPos].isNull() && removed.count(current[childPos])) hits.emplace_back(id, current);
            }
            if (hits.empty()) continue;
            if (childCol.onDelete == "CASCADE") {
                fkDelete(work, otherName, hits);
            } else if (childCol.onDelete == "SET NULL") {
                std::vector<FkChange> changes;
                for (const auto& [id, current] : hits) {
                    auto next = current;
                    next[childPos] = Value();
                    changes.push_back({id, current, std::move(next)});
                }
                fkUpdate(work, otherName, changes, true);
            } else {  // RESTRICT / NO ACTION / default
                throw ExecutionError("Table '" + tableName + "' mein ye value(s) hata/badal nahi sakte -- table '" +
                                     otherName + "' ka column '" + childCol.name + "' (SANDARBH " + tableName + "." +
                                     childCol.refColumn.value_or("") + ") abhi bhi inhe use karta hai");
            }
        }
    }
    --work.depth;
}

void Engine::fkUpdate(FkWork& work, const std::string& tableName, const std::vector<FkChange>& changes, bool cascaded) {
    requireStack();
    using FkEntry = FkWork::Entry;
    if (++work.depth > kMaxFkDepth)
        throw ExecutionError("SANDARBH CASCADE bahut gehra chal raha hai (limit " + std::to_string(kMaxFkDepth) + ")");
    const TableSchema parent = catalog().get(tableName);
    auto entryFor = [&](const std::string& name) -> FkEntry& {
        FkEntry& e = work.tables[name];
        if (e.table == nullptr) {
            e.owned = table(name);
            e.table = e.owned.get();
        }
        return e;
    };
    FkEntry& pe = entryFor(tableName);
    for (const auto& change : changes) {
        pe.original.emplace(change.rowId, change.oldValues);
        pe.changes[change.rowId] = change.newValues;
        if (cascaded) pe.cascaded.insert(change.rowId);
    }
    std::vector<std::string> otherNames;
    for (const auto& entry : catalog().tables) otherNames.push_back(entry.first);
    for (const auto& otherName : otherNames) {
        const TableSchema other = catalog().get(otherName);
        for (const auto& childCol : other.columns) {
            if (!childCol.refTable.has_value() || *childCol.refTable != tableName) continue;
            size_t parentPos = parent.indexOf(childCol.refColumn.value_or(""));
            size_t childPos = other.indexOf(childCol.name);
            std::unordered_map<Value, Value, PyValueHash, PyValueEq> moved;  // old parent value -> new one
            for (const auto& change : changes) {
                const Value& before = change.oldValues[parentPos];
                if (!before.isNull() && !pyEquals(before, change.newValues[parentPos])) moved[before] = change.newValues[parentPos];
            }
            if (moved.empty()) continue;
            FkEntry& ce = entryFor(otherName);
            if (!ce.loaded) {
                for (auto& [id, values] : ce.table->rows()) ce.original.emplace(id, std::move(values));
                ce.loaded = true;
            }
            std::vector<FkChange> next;
            for (const auto& [id, values] : ce.original) {
                auto change = ce.changes.find(id);
                if (change != ce.changes.end() && !change->second) continue;  // already going away
                const std::vector<Value>& current = change != ce.changes.end() ? *change->second : values;
                if (current[childPos].isNull()) continue;
                auto hit = moved.find(current[childPos]);
                if (hit == moved.end()) continue;
                if (childCol.onUpdate != "CASCADE" && childCol.onUpdate != "SET NULL")  // RESTRICT / NO ACTION / default
                    throw ExecutionError("Table '" + tableName + "' mein ye value(s) hata/badal nahi sakte -- table '" +
                                         otherName + "' ka column '" + childCol.name + "' (SANDARBH " + tableName + "." +
                                         childCol.refColumn.value_or("") + ") abhi bhi inhe use karta hai");
                auto updated = current;
                updated[childPos] = childCol.onUpdate == "CASCADE" ? hit->second : Value();
                next.push_back({id, current, std::move(updated)});
            }
            if (!next.empty()) fkUpdate(work, otherName, next, true);
        }
    }
    --work.depth;
}

// Rows changed by a cascade / SET NULL must still satisfy their own table's rules (types, SHART, ANOKHA).
void Engine::fkValidate(FkWork& work) {
    for (auto& [name, entry] : work.tables) {
        (void)name;
        if (entry.cascaded.empty()) continue;
        std::vector<std::vector<Value>> rows;
        for (int64_t id : entry.cascaded) {
            auto change = entry.changes.find(id);
            if (change != entry.changes.end() && change->second) rows.push_back(validateRow(entry.table->schema(), *change->second));
        }
        std::set<int64_t> ignore;
        for (const auto& [id, change] : entry.changes) {
            (void)change;
            ignore.insert(id);
        }
        checkUnique(*entry.table, rows, ignore);
    }
}

void Engine::fkApply(FkWork& work) {
    for (auto& [name, entry] : work.tables) {
        (void)name;
        std::vector<StoredRow> removals;
        std::vector<std::vector<Value>> additions;
        for (const auto& [id, change] : entry.changes) {
            removals.emplace_back(id, entry.original.at(id));
            if (change) additions.push_back(*change);
        }
        if (removals.empty()) continue;
        entry.table->deleteMany(removals);  // all deletes first, so an index entry that moves between rows survives
        if (!additions.empty()) entry.table->insertMany(additions);
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

Result Engine::execInsert(const ast::Insert& stmt, const RowCheck* check) {
    if (catalog().find(stmt.table) == nullptr && catalog().views.count(stmt.table)) return execInsertViaView(stmt);
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

    // AUTO_INCREMENT: a KHALI (or omitted) value becomes max + 1; an explicit value moves the counter up to it
    std::optional<size_t> autoPos;
    for (size_t i = 0; i < schema.columns.size(); ++i)
        if (schema.columns[i].autoIncrement) autoPos = i;
    int64_t autoNext = 0;
    bool autoReady = false;
    auto fillAuto = [&](std::vector<Value>& values) {
        if (!autoPos) return;
        if (!autoReady) {
            if (schema.columns[*autoPos].isUnique()) {  // the hash index already holds every value
                for (const auto& [key, rowId] : t->indexes().at(std::vector<size_t>{*autoPos})) {
                    (void)rowId;
                    if (isIntValue(key[0])) autoNext = std::max(autoNext, std::get<int64_t>(key[0].data));
                }
            } else {
                for (const auto& [id, rowValues] : t->rows()) {
                    (void)id;
                    if (isIntValue(rowValues[*autoPos])) autoNext = std::max(autoNext, std::get<int64_t>(rowValues[*autoPos].data));
                }
            }
            autoReady = true;
        }
        Value& v = values[*autoPos];
        if (v.isNull()) {
            if (autoNext == INT_MAX_VALUE) throw ExecutionError("Column '" + schema.columns[*autoPos].name + "': AUTO_INCREMENT ki limit poori ho gayi");
            v = Value(++autoNext);
        } else if (isIntValue(v)) {
            autoNext = std::max(autoNext, std::get<int64_t>(v.data));
        }
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
            fillAuto(values);
            newRows.push_back(validateRow(schema, values));
        }
    } else {
        for (const auto& tuple : stmt.rows) {
            if (tuple.size() != targetCols.size())
                throw ExecutionError(std::to_string(targetCols.size()) + " values chahiye thi, " +
                                     std::to_string(tuple.size()) + " mili");
            auto values = defaults();
            for (size_t i = 0; i < targetCols.size(); ++i) values[schema.indexOf(targetCols[i])] = evaluate(*tuple[i], Row{});
            fillAuto(values);
            newRows.push_back(validateRow(schema, values));
        }
    }

    if (check != nullptr && !stmt.onConflictUpdate.has_value()) (*check)(schema, newRows);  // WITH CHECK OPTION
    if (stmt.replace) return execReplace(*t, stmt, std::move(newRows));

    if (!stmt.onConflictUpdate.has_value()) {
        checkUnique(*t, newRows);
        checkFk(*t, newRows);
        std::vector<Row> newDicts;
        for (const auto& r : newRows) newDicts.push_back(rowDict(schema, r));
        bool changedByTrigger = false;
        for (auto& nr : newDicts) changedByTrigger |= fireBeforeTriggers("DAALO", stmt.table, nr, nullptr);
        if (changedByTrigger) {  // RAKHO NAYA.col = ...: the rows are checked again before they are written
            for (size_t i = 0; i < newRows.size(); ++i) newRows[i] = validateRow(schema, rowFromDict(schema, newDicts[i]));
            checkUnique(*t, newRows);
            checkFk(*t, newRows);
            newDicts.clear();
            for (const auto& r : newRows) newDicts.push_back(rowDict(schema, r));
        }
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
    if (check != nullptr) (*check)(schema, toInsert);
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
    if (check != nullptr) (*check)(schema, updatedNewRows);
    if (!updatedNewRows.empty()) {
        std::set<int64_t> ignore;
        for (const auto& [rowId, oldValues] : updatedTargets) {
            (void)oldValues;
            ignore.insert(rowId);
        }
        // the updated rows and the rows going in fresh end up in the same table: they must not collide with each other
        std::vector<std::vector<Value>> finalRows = updatedNewRows;
        finalRows.insert(finalRows.end(), toInsert.begin(), toInsert.end());
        checkUnique(*t, finalRows, ignore);
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

// REPLACE INTO / BADAL DAALO MEIN: every new row first removes the existing row(s) it collides with on MUKHYA KUNJI /
// ANOKHA (single or composite) -- through the SANDARBH rules, so a child row can block it or follow it -- then goes in.
// Within one statement a later row replaces an earlier one with the same key. Like MySQL, a replaced row counts twice.
Result Engine::execReplace(Table& t, const ast::Insert& stmt, std::vector<std::vector<Value>> newRows) {
    const TableSchema schema = t.schema();
    IndexMap& indexes = t.indexes();
    using KeyMap = std::unordered_map<std::vector<Value>, size_t, ValueVecHash, ValueVecEq>;

    std::vector<bool> dropped(newRows.size(), false);  // replaced by a later row of this same statement
    size_t droppedCount = 0;
    std::vector<KeyMap> batchKeys(indexes.size());
    for (size_t i = 0; i < newRows.size(); ++i) {
        size_t k = 0;
        for (auto& [positions, index] : indexes) {
            (void)index;
            KeyMap& seen = batchKeys[k++];
            auto key = indexKey(positions, newRows[i]);
            if (!key) continue;
            auto it = seen.find(*key);
            if (it != seen.end() && !dropped[it->second]) {
                dropped[it->second] = true;
                ++droppedCount;
            }
            seen[*key] = i;
        }
    }
    std::vector<std::vector<Value>> kept;
    for (size_t i = 0; i < newRows.size(); ++i)
        if (!dropped[i]) kept.push_back(std::move(newRows[i]));

    // the existing rows in the way, found through every unique / composite index
    std::set<int64_t> doomedIds;
    for (const auto& row : kept)
        for (auto& [positions, index] : indexes) {
            auto key = indexKey(positions, row);
            if (!key) continue;
            auto it = index.find(*key);
            if (it != index.end()) doomedIds.insert(it->second);
        }
    std::vector<StoredRow> doomed;
    for (int64_t id : doomedIds) {
        auto values = t.get(id);
        if (!values) throw ExecutionError("Row #" + std::to_string(id) + " nahi mili");
        doomed.emplace_back(id, std::move(*values));
    }

    FkWork work(&t, schema.name);
    if (!doomed.empty()) fkDelete(work, schema.name, doomed);
    fkValidate(work);
    std::set<int64_t> ignore;
    for (const auto& [id, change] : work.tables.at(schema.name).changes) {
        (void)change;
        ignore.insert(id);
    }
    checkUnique(t, kept, ignore);
    checkFk(t, kept);

    std::vector<Row> oldDicts, newDicts;
    for (const auto& d : doomed) oldDicts.push_back(rowDict(schema, d.second));
    for (const auto& r : kept) newDicts.push_back(rowDict(schema, r));
    for (const auto& od : oldDicts) fireTriggers("PEHLE", "MITAO", stmt.table, nullptr, &od);
    for (const auto& nr : newDicts) fireTriggers("PEHLE", "DAALO", stmt.table, &nr, nullptr);
    fkApply(work);
    t.insertMany(kept);
    for (const auto& od : oldDicts) fireTriggers("BAAD", "MITAO", stmt.table, nullptr, &od);
    for (const auto& nr : newDicts) fireTriggers("BAAD", "DAALO", stmt.table, &nr, nullptr);

    size_t inserted = kept.size() + droppedCount, removed = doomed.size() + droppedCount;
    return messageResult(std::to_string(inserted + removed) + " row(s) affected (" + std::to_string(inserted) +
                         " daali, " + std::to_string(removed) + " purani row(s) replace hui)");
}

Result Engine::execUpdate(const ast::Update& stmt, const RowCheck* check) {
    if (catalog().find(stmt.table) == nullptr && catalog().views.count(stmt.table)) return execUpdateViaView(stmt);
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
    if (check != nullptr) (*check)(schema, newRows);  // WITH CHECK OPTION

    std::set<int64_t> ignore;
    for (const auto& target : targets) ignore.insert(target.first);
    checkUnique(*t, newRows, ignore);
    checkFk(*t, newRows);  // child side: new FK values must exist in the parent

    // parent side: if a referenced column's value is CHANGING, child rows still pointing at the old value are
    // refused (RESTRICT, the default), follow it (ON UPDATE CASCADE) or are set to KHALI (SET NULL)
    auto buildFkWork = [&]() {
        auto w = std::make_unique<FkWork>(t.get(), stmt.table);
        std::vector<FkChange> fkChanges;
        for (size_t i = 0; i < targets.size(); ++i) fkChanges.push_back({targets[i].first, targets[i].second, newRows[i]});
        fkUpdate(*w, stmt.table, fkChanges, false);
        fkValidate(*w);
        return w;
    };
    auto work = buildFkWork();

    // An update = delete old versions + insert new versions. All deletes happen
    // first, so an index entry moved from one row to another (e.g. swapping two
    // ids) is never removed by mistake.
    // Plain {column: value} dicts for trigger NAYA/PURANA substitution --
    // independent of Scope's "table.col" aliasing, which triggers don't use.
    std::vector<Row> oldDicts, newDicts;
    for (const auto& target : targets) oldDicts.push_back(rowDict(schema, target.second));
    for (const auto& nr : newRows) newDicts.push_back(rowDict(schema, nr));
    bool changedByTrigger = false;
    for (size_t i = 0; i < oldDicts.size(); ++i)
        changedByTrigger |= fireBeforeTriggers("BADLO", stmt.table, newDicts[i], &oldDicts[i]);
    if (changedByTrigger) {  // RAKHO NAYA.col = ...: the rows and their SANDARBH effects are checked again
        for (size_t i = 0; i < newRows.size(); ++i) newRows[i] = validateRow(schema, rowFromDict(schema, newDicts[i]));
        checkUnique(*t, newRows, ignore);
        checkFk(*t, newRows);
        work = buildFkWork();
        newDicts.clear();
        for (const auto& nr : newRows) newDicts.push_back(rowDict(schema, nr));
    }
    fkApply(*work);
    for (size_t i = 0; i < oldDicts.size(); ++i) fireTriggers("BAAD", "BADLO", stmt.table, &newDicts[i], &oldDicts[i]);
    return messageResult(std::to_string(newRows.size()) + " row(s) badal di");
}

Result Engine::execDelete(const ast::Delete& stmt) {
    if (catalog().find(stmt.table) == nullptr && catalog().views.count(stmt.table)) return execDeleteViaView(stmt);
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

    // child rows still referencing a value about to be deleted: refused (RESTRICT, the default), deleted too
    // (ON DELETE CASCADE) or set to KHALI (SET NULL)
    FkWork work(t.get(), stmt.table);
    fkDelete(work, stmt.table, doomed);
    fkValidate(work);

    std::vector<Row> oldDicts;
    for (const auto& d : doomed) oldDicts.push_back(rowDict(schema, d.second));
    for (const auto& od : oldDicts) fireTriggers("PEHLE", "MITAO", stmt.table, nullptr, &od);
    fkApply(work);
    for (const auto& od : oldDicts) fireTriggers("BAAD", "MITAO", stmt.table, nullptr, &od);
    return messageResult(std::to_string(doomed.size()) + " row(s) mita di");
}

// ============================================================================
// SELECT
// ============================================================================


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
    requireStack();
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
        out->distinct = f->distinct;
        return out;
    }
    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) {
        auto out = std::make_unique<Coalesce>();
        for (const auto& a : co->args) out->args.push_back(rec(a.get()));
        return out;
    }
    if (auto* sf = dynamic_cast<const ScalarFunc*>(&expr)) {
        auto out = std::make_unique<ScalarFunc>();
        out->name = sf->name;
        out->castType = sf->castType;
        out->user = sf->user;
        for (const auto& a : sf->args) out->args.push_back(rec(a.get()));
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

// Replaces the column references `pick` chooses with a copy of the expression it returns. Does not enter subqueries:
// they have names of their own. Used for KAHO aliases in JINKA.
void replaceRefs(std::unique_ptr<ast::Expr>& expr, const std::function<const ast::Expr*(const ast::ColumnRef&)>& pick) {
    using namespace ast;
    if (!expr) return;
    requireStack();
    Expr* node = expr.get();
    if (auto* ref = dynamic_cast<ColumnRef*>(node)) {
        if (const Expr* replacement = pick(*ref)) expr = cloneExpr(*replacement);
    } else if (auto* b = dynamic_cast<BinaryOp*>(node)) {
        replaceRefs(b->left, pick);
        replaceRefs(b->right, pick);
    } else if (auto* u = dynamic_cast<UnaryOp*>(node)) {
        replaceRefs(u->operand, pick);
    } else if (auto* isn = dynamic_cast<IsNull*>(node)) {
        replaceRefs(isn->expr, pick);
    } else if (auto* co = dynamic_cast<Coalesce*>(node)) {
        for (auto& e : co->args) replaceRefs(e, pick);
    } else if (auto* sf = dynamic_cast<ScalarFunc*>(node)) {
        for (auto& e : sf->args) replaceRefs(e, pick);
    } else if (auto* cw = dynamic_cast<CaseWhen*>(node)) {
        for (auto& branch : cw->branches) {
            replaceRefs(branch.first, pick);
            replaceRefs(branch.second, pick);
        }
        replaceRefs(cw->elseExpr, pick);
    } else if (auto* in = dynamic_cast<InSubquery*>(node)) {
        replaceRefs(in->left, pick);
    }
}

// KRAM <whole number>: the 0-based output column it names; nullopt for any other expression.
std::optional<size_t> orderPosition(const ast::Expr& e, size_t outputCount) {
    auto* lit = dynamic_cast<const ast::Literal*>(&e);
    if (lit == nullptr || !isIntValue(lit->value)) return std::nullopt;
    int64_t n = std::get<int64_t>(lit->value.data);
    if (n < 1 || static_cast<uint64_t>(n) > outputCount)
        throw ExecutionError("KRAM " + std::to_string(n) + ": is query mein " + std::to_string(outputCount) +
                             " output column(s) hain -- 1 se " + std::to_string(outputCount) + " tak ka number likho");
    return static_cast<size_t>(n - 1);
}

// Scalar context (Subquery): exactly 1 column, 0 or 1 row. List context
// (InSubquery): exactly 1 column, any number of rows.
SubqueryResult reduceSubqueryResult(const ast::Expr& node, const Result& result) {
    if (auto* in = dynamic_cast<const ast::InSubquery*>(&node); in && in->kind == "EXISTS") {
        SubqueryResult exists;  // any number of columns: only "is there a row" matters
        exists.scalar = Value(!result.rows.empty());
        return exists;
    }
    if (result.columns.size() != 1) throw ExecutionError("Subquery sirf 1 column return kar sakti hai is jagah");
    SubqueryResult out;
    if (dynamic_cast<const ast::Subquery*>(&node)) {
        if (result.rows.size() > 1)
            throw ExecutionError("Subquery ek se zyada rows return kar rahi hai -- sirf 1 row honi chahiye");
        if (!result.rows.empty()) out.scalar = result.rows[0][0];
    } else {
        for (const auto& r : result.rows) out.values.push_back(r[0]);
        auto* in = dynamic_cast<const ast::InSubquery*>(&node);
        if (in != nullptr && in->kind == "IN") {  // the membership test is a hash lookup, not a scan for every outer row
            auto members = std::make_shared<std::unordered_set<Value, PyValueHash, PyValueEq>>();
            members->reserve(out.values.size());
            for (const auto& v : out.values) {
                if (v.isNull()) out.sawNull = true;
                else members->insert(v);
            }
            out.members = std::move(members);
        }
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
        struct ViewDepth {
            Engine& engine;
            ViewDepth(Engine& e, const std::string& view) : engine(e) {
                if (e.viewDepth_ >= kMaxViewDepth)
                    throw ExecutionError("View '" + view + "' bahut gehri nested hai (limit " +
                                         std::to_string(kMaxViewDepth) + " views ek ke andar ek)");
                ++e.viewDepth_;
            }
            ~ViewDepth() { --engine.viewDepth_; }
        } viewDepth(*this, name);
        requireStack();
        auto parsed = parseScript(cat.views.at(name));
        auto* viewStmt = parsed.empty() ? nullptr : dynamic_cast<const ast::Select*>(parsed[0].get());
        if (viewStmt == nullptr) throw ExecutionError("View '" + name + "' ki definition DIKHAO nahi hai");
        return materialize(name, execSelect(*viewStmt));
    }
    throw ExecutionError("Table '" + name + "' exist nahi karta");
}

// A query result as a read-only table named `name` (a VIEW's rows, or a derived table's).
std::unique_ptr<Table> Engine::materialize(const std::string& name, const Result& result) {
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

Scope Engine::selectSourcesScope(const ast::Select& stmt, std::vector<std::unique_ptr<Table>>* tablesOut) {
    struct Source {
        std::string alias, table;
        const ast::Select* derived;  // a (DIKHAO ...) alias source, else nullptr
    };
    std::vector<Source> sources;
    // DIKHAO 1 + 1 (no SE): a one-row, no-column stand-in table called `dual`
    const bool noFrom = stmt.table.empty() && !stmt.derived;
    sources.push_back({stmt.alias.has_value() && !stmt.alias->empty() ? *stmt.alias : noFrom ? "dual" : stmt.table, stmt.table, stmt.derived.get()});
    for (const auto& j : stmt.joins) sources.push_back({j.alias, j.table, j.derived.get()});
    std::vector<std::unique_ptr<Table>> tables;
    for (const auto& s : sources) {
        if (s.derived == nullptr && s.table.empty()) {
            TableSchema dual;
            dual.name = "dual";
            tables.push_back(std::make_unique<MaterializedTable>(dual, std::vector<RowValues>{RowValues{}}));
            continue;
        }
        if (s.derived == nullptr) {
            tables.push_back(resolveSource(s.table));
            continue;
        }
        // a derived table runs its query first; its output names are its columns
        Result inner = execSelect(*s.derived);
        std::set<std::string> seen;
        for (const auto& column : inner.columns)
            if (!seen.insert(column).second)
                throw ExecutionError("Derived table '" + s.alias + "' mein column '" + column +
                                     "' do baar hai -- KAHO se alag naam do");
        tables.push_back(materialize(s.alias, inner));
    }
    std::vector<std::pair<std::string, TableSchema>> withSchemas;
    for (size_t i = 0; i < sources.size(); ++i) withSchemas.emplace_back(sources[i].alias, tables[i]->schema());
    Scope scope(std::move(withSchemas));
    if (tablesOut) *tablesOut = std::move(tables);
    return scope;
}

std::unique_ptr<Engine::SelectPlan> Engine::planSelect(const ast::Select& stmt) {
    auto plan = std::make_unique<SelectPlan>();
    if (stmt.table.empty() && !stmt.derived) {  // DIKHAO without SE: there is no table to read a column from
        std::vector<const ast::ColumnRef*> refs;
        for (const auto& c : stmt.columns) columnRefNodes(*c, refs);
        for (const ast::Expr* e : {stmt.where.get(), stmt.having.get()})
            if (e) columnRefNodes(*e, refs);
        for (const auto& g : stmt.groupBy) columnRefNodes(*g, refs);
        for (const auto& o : stmt.orderBy) columnRefNodes(*o.expr, refs);
        if (!refs.empty())
            throw ExecutionError("Column '" + refs[0]->name + "' ke liye table chahiye -- SE table likho (bina SE ke DIKHAO sirf "
                                 "fixed values / functions ke liye hai, jaise DIKHAO NOW(), 1 + 1)");
    }
    plan->scope = std::make_unique<Scope>(selectSourcesScope(stmt, &plan->tables));
    const Scope& scope = *plan->scope;

    // KAHO: an output alias becomes the column header, and KRAM, SAMOOH and JINKA may refer back to it by name -- JAHAN
    // does not (it runs before the output list exists). We collect {alias: unbound expr} here so they can be rewritten
    // to that expr BEFORE binding.
    std::unordered_map<std::string, const ast::Expr*> aliasExprs;

    // MILAO ... USING (c): an unqualified c means the shared column (see Scope::declareUsing), and an unqualified
    // star shows it ONCE -- the first table's copy (for a RIGHT / FULL join: whichever side has a value). The
    // later copies are hidden; t.* still shows its own.
    std::set<std::string> usingHidden;  // later copies, "alias.col"
    for (size_t i = 0; i < stmt.joins.size(); ++i) {
        if (stmt.joins[i].usingColumns.empty()) continue;
        bool merge = stmt.joins[i].kind == "RIGHT" || stmt.joins[i].kind == "FULL";
        for (const auto& [leftKey, rightKey] : usingColumnPairs(*plan->scope, i + 1, stmt.joins[i].usingColumns)) {
            plan->scope->declareUsing(leftKey.substr(leftKey.find('.') + 1), leftKey, rightKey, merge);
            usingHidden.insert(rightKey);
        }
    }

    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        const ast::Expr& e = *stmt.columns[i];
        std::optional<std::string> alias = i < stmt.aliases.size() ? stmt.aliases[i] : std::nullopt;
        bool hasAlias = alias.has_value() && !alias->empty();
        if (auto* star = dynamic_cast<const ast::Star*>(&e)) {
            for (const auto& [label, ref] : scope.expandStar(*star)) {
                std::string key = *ref.table + "." + ref.name;
                if (!star->table.has_value()) {
                    if (usingHidden.count(key)) continue;
                    const Scope::UsingColumn* shared = scope.usingColumn(ref.name);
                    if (shared && key == shared->keys.front()) {
                        plan->labels.push_back(ref.name);  // the merged copy: a bare label even if both tables have it
                        plan->outputs.push_back(bind(ast::ColumnRef(ref.name), scope));  // bare => merged if RIGHT / FULL
                        continue;
                    }
                }
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
    // A bare name in KRAM that matches an alias means the output expression, even when a column has that name too
    // (standard SQL: ORDER BY looks at the output columns first). SAMOOH and JINKA look at the table's columns first and
    // fall back to an alias, like MySQL.
    auto aliasOf = [&](const ast::Expr& e, bool columnsFirst) -> const ast::Expr* {
        auto* ref = dynamic_cast<const ast::ColumnRef*>(&e);
        if (ref == nullptr || ref->table.has_value()) return nullptr;
        auto it = aliasExprs.find(ref->name);
        if (it == aliasExprs.end() || (columnsFirst && isRealColumn(ref->name))) return nullptr;
        return it->second;
    };

    plan->where = bind(stmt.where.get(), scope);
    for (const auto& g : stmt.groupBy) {
        const ast::Expr* alias = aliasOf(*g, true);
        plan->groupBy.push_back(bind(alias ? *alias : *g, scope));
    }
    if (stmt.having) {
        auto having = cloneExpr(*stmt.having);
        replaceRefs(having, [&](const ast::ColumnRef& ref) { return aliasOf(ref, true); });
        plan->having = bind(having.get(), scope);
    }
    for (const auto& o : stmt.orderBy) {
        // KRAM 2: the second output column. Only a bare whole number is a position; KRAM 1 + 1 stays an expression.
        if (auto position = orderPosition(*o.expr, plan->outputs.size())) {
            plan->orderBy.emplace_back(cloneExpr(*plan->outputs[*position]), o.descending);
            continue;
        }
        const ast::Expr* alias = aliasOf(*o.expr, false);
        plan->orderBy.emplace_back(bind(alias ? *alias : *o.expr, scope), o.descending);
    }

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
        } else if (!join.usingColumns.empty()) {
            step.on = usingJoinCondition(scope, sourceIndex, join.usingColumns);
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
    if (stmt.derived) out->derived = cloneSelect(*stmt.derived);  // a derived table is not correlated
    for (const auto& j : stmt.joins) {
        ast::Join nj;
        nj.table = j.table;
        nj.alias = j.alias;
        nj.on = corr(j.on.get());
        nj.kind = j.kind;
        nj.usingColumns = j.usingColumns;
        if (j.derived) nj.derived = cloneSelect(*j.derived);
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
    out->offset = stmt.offset;
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
    requireStack();
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

    // 8. OFFSET (skip the first m rows), then LIMIT (SIRF)  (Python's `rows[:n]`)
    if (stmt.offset.has_value()) {
        size_t skip = static_cast<size_t>(std::max(0, *stmt.offset));
        if (skip >= outRows.size()) outRows.clear();
        else outRows.erase(outRows.begin(), outRows.begin() + static_cast<std::ptrdiff_t>(skip));
    }
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
    // KRAM / SIRF after the last side belong to the whole result. Only what it shows can sort it, so each key is an output
    // column: its name (the first side's names count) or its position.
    std::vector<std::pair<size_t, bool>> sortColumns;
    for (const auto& o : stmt.orderBy) {
        std::optional<size_t> position = orderPosition(*o.expr, left.columns.size());
        if (!position) {
            auto* ref = dynamic_cast<const ast::ColumnRef*>(o.expr.get());
            const std::string name = ref ? ref->name : exprLabel(*o.expr);
            auto it = std::find(left.columns.begin(), left.columns.end(), name);
            if (it == left.columns.end())
                throw ExecutionError("KRAM '" + name + "': " + stmt.op + " ka result sirf apne output columns se sort ho sakta hai (naam ya number)");
            position = static_cast<size_t>(it - left.columns.begin());
        }
        sortColumns.emplace_back(*position, o.descending);
    }
    if (!sortColumns.empty())
        std::stable_sort(out.begin(), out.end(), [&](const std::vector<Value>& a, const std::vector<Value>& b) {
            for (const auto& [column, descending] : sortColumns) {
                if (sortLess(a[column], b[column])) return !descending;
                if (sortLess(b[column], a[column])) return descending;
            }
            return false;
        });
    if (stmt.offset.has_value()) {
        if (static_cast<size_t>(*stmt.offset) >= out.size()) out.clear();
        else out.erase(out.begin(), out.begin() + *stmt.offset);
    }
    if (stmt.limit.has_value() && static_cast<size_t>(*stmt.limit) < out.size()) out.resize(static_cast<size_t>(*stmt.limit));
    Result result;
    result.columns = left.columns;
    result.message = std::to_string(out.size()) + " row(s)";
    result.rows = std::move(out);
    return result;
}

// ============================================================================
// views
// ============================================================================

// ---- SHOW family ----

Result Engine::execShowDatabases(const ast::ShowDatabases&) {
    Result r;
    r.columns = {"database"};
    auto names = instance_->databases();
    for (auto& n : names) r.rows.push_back({textValue(n)});
    r.message = std::to_string(names.size()) + " database(s)";
    return r;
}

Result Engine::execShowTriggers(const ast::ShowTriggers&) {
    Result r;
    r.columns = {"trigger", "timing", "event", "table", "body"};
    for (auto it = catalog().triggers.begin(); it != catalog().triggers.end(); ++it) {
        const auto& t = it.value();
        r.rows.push_back({textValue(it.key()), textValue(t.value("timing", "")), textValue(t.value("event", "")),
                          textValue(t.value("table", "")), textValue(t.value("body_text", ""))});
    }
    r.message = std::to_string(r.rows.size()) + " trigger(s) in '" + currentDb + "'";
    return r;
}

// every database's procedures: name, database, created
Result Engine::execShowProcedures(const ast::ShowProcedures&) {
    Result r;
    r.columns = {"name", "database", "created"};
    for (const auto& db : instance_->databases()) {
        Catalog& cat = instance_->catalog(db);
        for (auto it = cat.procedures.begin(); it != cat.procedures.end(); ++it) {
            auto info = cat.findProcedure(it.key());
            std::string created = info ? info->created : std::string();
            r.rows.push_back({textValue(it.key()), textValue(db), created.empty() ? Value() : textValue(created)});
        }
    }
    r.message = std::to_string(r.rows.size()) + " procedure(s)";
    return r;
}

Result Engine::execShowGrants(const ast::ShowGrants& stmt) {
    Result r;
    if (!stmt.user.has_value() && !user) {
        r.columns = {"Grants for superuser"};
        r.rows.push_back({textValue("GRANT ALL PRIVILEGES ON *.* TO superuser  -- bina username ka session")});
        r.message = "1 grant(s)";
        return r;
    }
    const std::string who = stmt.user.has_value() ? *stmt.user : *user;
    auto lines = instance_->users().grantLines(who);
    r.columns = {"Grants for " + who};
    for (auto& line : lines) r.rows.push_back({textValue(line)});
    r.message = std::to_string(lines.size()) + " grant(s)";
    return r;
}

namespace {
std::string sqlQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out + "'";
}

// A stored value as a MeraDB literal (for WARNA)
std::string sqlLiteral(const Value& v) {
    if (v.isNull()) return "KHALI";
    if (isBoolValue(v)) return std::get<bool>(v.data) ? "SACH" : "JHOOTH";
    if (isIntValue(v)) return std::to_string(std::get<int64_t>(v.data));
    if (isDateValue(v)) return sqlQuote(std::get<Date>(v.data).isoFormat());
    if (isTextValue(v)) return sqlQuote(std::get<std::string>(v.data));
    return formatValue(v);
}

bool isAutoFkName(const std::string& name) {  // <table>_ibfk_<n>
    size_t at = name.rfind("_ibfk_");
    return at != std::string::npos && at + 6 < name.size() && name.find_first_not_of("0123456789", at + 6) == std::string::npos;
}
}  // namespace

// A BANAO TABLE statement that rebuilds the table's structure (no rows)
Result Engine::execShowCreateTable(const ast::ShowCreateTable& stmt) {
    Catalog& cat = catalog();
    if (cat.find(stmt.name) == nullptr && cat.views.count(stmt.name))
        throw ExecutionError("'" + stmt.name + "' ek VIEW hai -- SHOW CREATE VIEW " + stmt.name + " use karo");
    const TableSchema& schema = cat.get(stmt.name);
    std::set<std::string> inCompositePk;
    if (schema.compositePk.has_value()) inCompositePk.insert(schema.compositePk->begin(), schema.compositePk->end());
    std::vector<std::string> items, constraints;
    for (const auto& c : schema.columns) {
        std::string line = c.name + " ";
        if (!c.enumValues.empty()) {
            line += "ENUM(";
            for (size_t i = 0; i < c.enumValues.size(); ++i) line += (i ? ", " : "") + sqlQuote(c.enumValues[i]);
            line += ")";
        } else {
            line += c.typeName;
            if (c.maxLength.has_value()) line += "(" + std::to_string(*c.maxLength) + ")";
        }
        if (c.autoIncrement) line += " AUTO_INCREMENT";
        if (c.primaryKey) line += " MUKHYA KUNJI";
        else if (c.notNull && !c.autoIncrement && !inCompositePk.count(c.name)) line += " ZAROORI";
        if (c.unique && !c.primaryKey) line += " ANOKHA";
        if (c.defaultValue.has_value() && !c.defaultValue->isNull()) line += " WARNA " + sqlLiteral(*c.defaultValue);
        if (nonEmpty(c.refTable)) {
            std::string fk = "SANDARBH " + *c.refTable + "(" + c.refColumn.value_or("") + ")";
            if (!c.onDelete.empty()) fk += " ON DELETE " + c.onDelete;
            if (!c.onUpdate.empty()) fk += " ON UPDATE " + c.onUpdate;
            if (c.fkName.empty() || isAutoFkName(c.fkName)) line += " " + fk;
            else constraints.push_back("CONSTRAINT " + c.fkName + " FOREIGN KEY (" + c.name + ") " + fk);
        }
        if (nonEmpty(c.check)) line += " SHART (" + *c.check + ")";
        items.push_back(line);
    }
    for (const auto& group : schema.compositeUnique) items.push_back("ANOKHA (" + joinStrs(group, ", ") + ")");
    if (schema.compositePk.has_value() && !schema.compositePk->empty())
        items.push_back("MUKHYA KUNJI (" + joinStrs(*schema.compositePk, ", ") + ")");
    for (auto& c : constraints) items.push_back(std::move(c));

    std::string text = "BANAO TABLE " + schema.name + " (\n";
    for (size_t i = 0; i < items.size(); ++i) text += "    " + items[i] + (i + 1 < items.size() ? ",\n" : "\n");
    text += ");";
    Result r;
    r.columns = {"table", "create_table"};
    r.rows.push_back({textValue(schema.name), textValue(text)});
    r.message = "Table '" + schema.name + "'";
    return r;
}

Result Engine::execShowCreateView(const ast::ShowCreateView& stmt) {
    Catalog& cat = catalog();
    auto it = cat.views.find(stmt.name);
    if (it == cat.views.end()) {
        if (cat.find(stmt.name) != nullptr)
            throw ExecutionError("'" + stmt.name + "' ek table hai, VIEW nahi -- SHOW CREATE TABLE " + stmt.name + " use karo");
        throw ExecutionError("View '" + stmt.name + "' exist nahi karta");
    }
    std::string text = "BANAO VIEW " + stmt.name + " KAHO " + it->second;
    if (cat.viewCheckOption.count(stmt.name)) text += " WITH CHECK OPTION";
    text += ";";
    Result r;
    r.columns = {"view", "create_view"};
    r.rows.push_back({textValue(stmt.name), textValue(text)});
    r.message = "VIEW '" + stmt.name + "'";
    return r;
}

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

Result Engine::execCreateIndex(const ast::CreateIndex& stmt) {
    auto t = table(stmt.table);
    TableSchema schema = t->schema();
    for (const auto& ix : schema.indexes)
        if (ix.name == stmt.name) {
            if (stmt.ifNotExists) return messageResult("Index '" + stmt.name + "' pehle se hai -- kuch nahi badla");
            throw ExecutionError("Index '" + stmt.name + "' table '" + stmt.table + "' par pehle se hai");
        }
    if (stmt.name == "PRIMARY") throw ExecutionError("'PRIMARY' naam MUKHYA KUNJI ka hai -- koi aur naam chuno");
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        schema.indexOf(stmt.columns[i]);  // throws if the column is missing
        for (size_t k = 0; k < i; ++k)
            if (stmt.columns[k] == stmt.columns[i])
                throw ExecutionError("Column '" + stmt.columns[i] + "' index mein do baar likha hai");
    }
    if (stmt.unique) {
        // the uniqueness itself is a table-level ANOKHA group; adding it checks the rows already there
        ast::AlterAddComposite add;
        add.table = stmt.table;
        add.kind = "ANOKHA";
        add.columns = stmt.columns;
        execAlterAddComposite(add);
        schema = table(stmt.table)->schema();
    }
    schema.indexes.push_back(IndexDef{stmt.name, stmt.columns, stmt.unique});
    catalog().add(schema);
    return messageResult(std::string(stmt.unique ? "ANOKHA index '" : "Index '") + stmt.name + "' table '" + stmt.table +
                         "' par (" + joinStrs(stmt.columns, ", ") + ") ke liye ban gaya");
}

Result Engine::execDropIndex(const ast::DropIndex& stmt) {
    auto t = table(stmt.table);
    TableSchema schema = t->schema();
    auto it = std::find_if(schema.indexes.begin(), schema.indexes.end(), [&](const IndexDef& ix) { return ix.name == stmt.name; });
    if (it == schema.indexes.end()) {
        if (stmt.ifExists) return messageResult("Index '" + stmt.name + "' nahi hai -- kuch nahi badla");
        throw ExecutionError("Index '" + stmt.name + "' table '" + stmt.table + "' par nahi hai (SHOW INDEX FROM " +
                             stmt.table + " se dekho)");
    }
    if (it->unique) {  // dropping a unique index lifts the ANOKHA group it added
        auto g = std::find(schema.compositeUnique.begin(), schema.compositeUnique.end(), it->columns);
        if (g != schema.compositeUnique.end()) schema.compositeUnique.erase(g);
    }
    schema.indexes.erase(it);
    t->invalidateIndexes();
    catalog().add(schema);
    return messageResult("Index '" + stmt.name + "' hata diya");
}

Result Engine::execShowIndex(const ast::ShowIndex& stmt) {
    auto t = table(stmt.table);
    const TableSchema& schema = t->schema();
    Result r;
    r.columns = {"Table", "Non_unique", "Key_name", "Seq_in_index", "Column_name"};
    auto add = [&](const std::string& key, bool unique, const std::vector<std::string>& cols) {
        for (size_t i = 0; i < cols.size(); ++i)
            r.rows.push_back({textValue(schema.name), intValue(unique ? 0 : 1), textValue(key),
                              intValue(static_cast<int64_t>(i + 1)), textValue(cols[i])});
    };
    std::vector<std::string> pk;
    for (const auto& c : schema.columns)
        if (c.primaryKey) pk.push_back(c.name);
    if (schema.compositePk) pk = *schema.compositePk;
    if (!pk.empty()) add("PRIMARY", true, pk);
    for (const auto& c : schema.columns)
        if (c.unique && !c.primaryKey) add(c.name, true, {c.name});
    for (const auto& g : schema.compositeUnique) {
        auto named = std::find_if(schema.indexes.begin(), schema.indexes.end(),
                                  [&](const IndexDef& ix) { return ix.unique && ix.columns == g; });
        if (named == schema.indexes.end()) add(joinStrs(g, "_"), true, g);
    }
    for (const auto& ix : schema.indexes) add(ix.name, ix.unique, ix.columns);
    r.message = std::to_string(r.rows.size()) + " index entry(ies) on '" + schema.name + "'";
    return r;
}

Result Engine::execCreateView(const ast::CreateView& stmt) {
    Catalog& cat = catalog();
    if (cat.find(stmt.name) != nullptr)
        throw ExecutionError("Table '" + stmt.name + "' pehle se hai -- VIEW usi naam se nahi ban sakti");
    const bool exists = cat.views.count(stmt.name) > 0;
    if (stmt.alter && !exists)
        throw ExecutionError("View '" + stmt.name + "' exist nahi karta -- pehle BANAO VIEW se banao");
    if (exists && !stmt.alter && !stmt.orReplace) throw ExecutionError("View '" + stmt.name + "' pehle se hai");
    auto parsed = parseScript(stmt.queryText);
    auto* select = parsed.empty() ? nullptr : dynamic_cast<const ast::Select*>(parsed[0].get());
    if (select == nullptr) throw ExecutionError("BANAO VIEW ke baad sirf ek DIKHAO query aa sakti hai");
    for (const auto& t : tablesRead(*select))
        if (t == stmt.name) throw ExecutionError("View '" + stmt.name + "' apne aap ko refer nahi kar sakti");
    execSelect(*select);  // sanity check: must run cleanly against the CURRENT schema
    if (stmt.checkOption) {
        std::string reason;
        if (!viewTargetOf(stmt.name, stmt.queryText, reason))
            throw ExecutionError("WITH CHECK OPTION sirf simple view par lag sakta hai (ek table, seedhe columns): " + reason);
    }
    cat.addView(stmt.name, stmt.queryText, stmt.checkOption);
    if (exists) return messageResult("View '" + stmt.name + "' badal diya");
    return messageResult("View '" + stmt.name + "' ban gaya");
}

// ---- updatable views -----------------------------------------------------------------------------------------------
// A view over ONE real table whose select list is plain columns (or `*`), with no DISTINCT / SAMOOH / JINKA / SIRF,
// accepts DAALO / BADLO / MITAO: the statement is rewritten onto the base table (the view's JAHAN is ANDed into BADLO and
// MITAO; view column names and aliases are mapped back to base columns). WITH CHECK OPTION refuses a row that the view
// would not show afterwards.

struct Engine::ViewTarget {
    std::string name, baseTable;
    std::vector<std::string> names;     // the view's columns, in order (aliases applied)
    std::vector<std::string> baseCols;  // the base-table column behind each
    std::shared_ptr<ast::Expr> where;   // the view's JAHAN with table qualifiers removed (nullptr = none)
    bool checkOption = false;

    const std::string& baseCol(const std::string& viewCol) const {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == viewCol) return baseCols[i];
        throw ExecutionError("Column '" + viewCol + "' view '" + name + "' mein nahi hai");
    }
};

std::unique_ptr<Engine::ViewTarget> Engine::viewTargetOf(const std::string& name, const std::string& queryText,
                                                         std::string& reason) {
    auto parsed = parseScript(queryText);
    auto* sel = parsed.empty() ? nullptr : dynamic_cast<ast::Select*>(parsed[0].get());
    if (sel == nullptr) { reason = "definition DIKHAO nahi hai"; return nullptr; }
    if (sel->derived || !sel->joins.empty()) { reason = "isme JOIN ya derived table hai"; return nullptr; }
    if (sel->table.empty()) { reason = "isme koi table nahi hai"; return nullptr; }
    if (sel->distinct || !sel->groupBy.empty() || sel->having || sel->limit.has_value() || sel->offset.has_value()) {
        reason = "isme ALAG / SAMOOH / JINKA / SIRF hai";
        return nullptr;
    }
    Catalog& cat = catalog();
    const TableSchema* base = cat.find(sel->table);
    if (base == nullptr) {
        reason = cat.views.count(sel->table) ? "ye kisi doosri view par bani hai" : "base table nahi mili";
        return nullptr;
    }
    auto target = std::make_unique<ViewTarget>();
    target->name = name;
    target->baseTable = sel->table;
    const std::string alias = sel->alias.value_or(sel->table);
    auto own = [&](const std::optional<std::string>& qualifier) { return !qualifier || *qualifier == alias || *qualifier == sel->table; };
    auto add = [&](const std::string& viewName, const std::string& baseName) {
        for (const auto& existing : target->names)
            if (existing == viewName) {
                reason = "column '" + viewName + "' do baar aaya hai";
                return false;
            }
        target->names.push_back(viewName);
        target->baseCols.push_back(baseName);
        return true;
    };
    for (size_t i = 0; i < sel->columns.size(); ++i) {
        const ast::Expr& item = *sel->columns[i];
        if (auto* star = dynamic_cast<const ast::Star*>(&item)) {
            if (!own(star->table)) { reason = "select list mein doosri table ka '*' hai"; return nullptr; }
            for (const auto& c : base->columns)
                if (!add(c.name, c.name)) return nullptr;
            continue;
        }
        auto* ref = dynamic_cast<const ast::ColumnRef*>(&item);
        if (ref == nullptr || !own(ref->table)) {
            reason = "'" + exprLabel(item) + "' seedha column nahi hai (function / hisaab / aggregate)";
            return nullptr;
        }
        try {
            base->indexOf(ref->name);
        } catch (const MeraDBError&) {
            reason = "column '" + ref->name + "' base table mein nahi hai";
            return nullptr;
        }
        std::optional<std::string> as = i < sel->aliases.size() ? sel->aliases[i] : std::nullopt;
        if (!add(as.value_or(ref->name), ref->name)) return nullptr;
    }
    if (sel->where) {
        std::unique_ptr<ast::Expr> copy = cloneExpr(*sel->where);
        forEachRef(copy.get(), [](ast::ColumnRef& ref) { ref.table.reset(); });
        target->where = std::move(copy);
    }
    target->checkOption = cat.viewCheckOption.count(name) > 0;
    return target;
}

// The target for DAALO / BADLO / MITAO on `name`; the classic "ek VIEW hai" error when the view is not simple enough.
std::unique_ptr<Engine::ViewTarget> Engine::updatableView(const std::string& name) {
    std::string reason;
    auto target = viewTargetOf(name, catalog().views.at(name), reason);
    if (!target)
        throw ExecutionError("'" + name + "' ek VIEW hai, table nahi -- isme DAALO/BADLO/MITAO nahi kar sakte");
    return target;
}

Engine::RowCheck Engine::viewCheck(const ViewTarget& target) {
    std::shared_ptr<ast::Expr> where = target.where;
    std::string viewName = target.name;
    return [this, where, viewName](const TableSchema& schema, const std::vector<std::vector<Value>>& rows) {
        if (!where) return;
        Scope scope({{schema.name, schema}});
        auto bound = bind(*where, scope);
        std::vector<Row> dicts;
        for (const auto& r : rows) dicts.push_back(scope.row(0, r));
        auto subq = precomputeSubqueries({bound.get()}, dicts, scope.allKeys());
        for (size_t i = 0; i < dicts.size(); ++i)
            if (!isTrue(evaluate(*bound, dicts[i], subq.at(i))))
                throw ExecutionError("View '" + viewName +
                                     "' ki WITH CHECK OPTION ke karan row reject: ye row view ki JAHAN shart poori nahi karti");
    };
}

namespace {
// The user's expression is written in the view's column names: turn them into the base table's.
void renameToBase(ast::Expr& e, const std::function<const std::string&(const std::string&)>& baseCol) {
    forEachRef(&e, [&](ast::ColumnRef& ref) {
        ref.table.reset();
        ref.name = baseCol(ref.name);
    });
}

std::unique_ptr<ast::Expr> andExpr(std::unique_ptr<ast::Expr> a, std::unique_ptr<ast::Expr> b) {
    if (!a) return b;
    if (!b) return a;
    return std::make_unique<ast::BinaryOp>("AUR", std::move(a), std::move(b));
}
}  // namespace

Result Engine::execInsertViaView(const ast::Insert& stmt) {
    auto vt = updatableView(stmt.table);
    auto baseCol = [&](const std::string& c) -> const std::string& { return vt->baseCol(c); };
    ast::Insert base;
    base.table = vt->baseTable;
    base.replace = stmt.replace;
    if (stmt.columns.has_value() && !stmt.columns->empty()) {
        std::vector<std::string> cols;
        for (const auto& c : *stmt.columns) cols.push_back(vt->baseCol(c));
        base.columns = std::move(cols);
    } else {
        base.columns = vt->baseCols;  // all of the view's columns; the others get their defaults
    }
    for (const auto& tuple : stmt.rows) {
        std::vector<std::unique_ptr<ast::Expr>> row;
        for (const auto& e : tuple) row.push_back(cloneExpr(*e));
        base.rows.push_back(std::move(row));
    }
    if (stmt.select) base.select = cloneSelect(*stmt.select);
    if (stmt.onConflictUpdate.has_value()) {
        std::vector<std::pair<std::string, std::unique_ptr<ast::Expr>>> list;
        for (const auto& [col, expr] : *stmt.onConflictUpdate) {
            auto copy = cloneExpr(*expr);
            renameToBase(*copy, baseCol);
            list.emplace_back(vt->baseCol(col), std::move(copy));
        }
        base.onConflictUpdate = std::move(list);
    }
    RowCheck check = vt->checkOption ? viewCheck(*vt) : RowCheck();
    return execInsert(base, check ? &check : nullptr);
}

Result Engine::execUpdateViaView(const ast::Update& stmt) {
    auto vt = updatableView(stmt.table);
    auto baseCol = [&](const std::string& c) -> const std::string& { return vt->baseCol(c); };
    ast::Update base;
    base.table = vt->baseTable;
    for (const auto& [col, expr] : stmt.assignments) {
        auto copy = cloneExpr(*expr);
        renameToBase(*copy, baseCol);
        base.assignments.emplace_back(vt->baseCol(col), std::move(copy));
    }
    std::unique_ptr<ast::Expr> own;
    if (stmt.where) {
        own = cloneExpr(*stmt.where);
        renameToBase(*own, baseCol);
    }
    base.where = andExpr(vt->where ? cloneExpr(*vt->where) : nullptr, std::move(own));
    RowCheck check = vt->checkOption ? viewCheck(*vt) : RowCheck();
    return execUpdate(base, check ? &check : nullptr);
}

Result Engine::execDeleteViaView(const ast::Delete& stmt) {
    auto vt = updatableView(stmt.table);
    auto baseCol = [&](const std::string& c) -> const std::string& { return vt->baseCol(c); };
    ast::Delete base;
    base.table = vt->baseTable;
    std::unique_ptr<ast::Expr> own;
    if (stmt.where) {
        own = cloneExpr(*stmt.where);
        renameToBase(*own, baseCol);
    }
    base.where = andExpr(vt->where ? cloneExpr(*vt->where) : nullptr, std::move(own));
    return execDelete(base);
}

Result Engine::execDropView(const ast::DropView& stmt) {
    Catalog& cat = catalog();
    if (!cat.views.count(stmt.name)) {
        if (stmt.ifExists) return messageResult("View '" + stmt.name + "' nahi hai -- kuch nahi badla");
        throw ExecutionError("View '" + stmt.name + "' exist nahi karta");
    }
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
        if (!setOp->orderBy.empty()) {
            std::vector<std::string> keys;
            for (const auto& o : setOp->orderBy) keys.push_back(exprLabel(*o.expr) + (o.descending ? " ULTA" : ""));
            lines.push_back("SORT   KRAM " + joinStrs(keys, ", "));
        }
        if (setOp->limit.has_value())
            lines.push_back("LIMIT  SIRF " + std::to_string(*setOp->limit) +
                            (setOp->offset.has_value() ? " OFFSET " + std::to_string(*setOp->offset) : std::string()));
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
    requireStack();
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
    lines.push_back(plan.access ? plan.access->describe(*plan.tables[0]) : "FULL SCAN " + (stmt.derived ? "DERIVED TABLE" : stmt.table.empty() ? "(no table: one constant row)" : stmt.table) + alias);
    for (const auto& step : plan.joins) {
        std::string kind = joinLabel(step.join->kind) + (step.hashKeys ? "HASH JOIN" : "NESTED LOOP JOIN");
        std::string name = step.join->derived ? "DERIVED TABLE " + step.join->alias
                           : step.join->alias == step.join->table ? step.join->table
                                                                    : step.join->table + " " + step.join->alias;
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
    if (stmt.limit.has_value())
        lines.push_back("LIMIT  SIRF " + std::to_string(*stmt.limit) +
                        (stmt.offset.has_value() ? " OFFSET " + std::to_string(*stmt.offset) : std::string()));
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
