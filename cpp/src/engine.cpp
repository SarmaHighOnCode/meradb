// cpp/src/engine.cpp -- mirrors meradb/engine.py
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
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

Instance::Instance(std::string dataDir) : dataDir_(fs::absolute(fs::path(dataDir)).string()) {
    fs::create_directories(dataDir_);
    recovered_ = recover();
    fs::create_directories(dbDir(DEFAULT_DATABASE));
}

std::string Instance::dbDir(const std::string& name) const { return (fs::path(dataDir_) / name).string(); }

std::vector<std::string> Instance::databases() const {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dataDir_)) {
        std::string name = entry.path().filename().string();
        if (!name.empty() && name[0] != '.' && entry.is_directory()) names.push_back(name);
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
    // taken by SHURU is released.
    if (inTransaction()) instance_->lock.unlock();
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

// Python's repr(float): shortest round-tripping digits, fixed notation for
// 1e-4 <= |x| < 1e16, exponent form otherwise.
std::string pyReprDouble(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    if (d == 0) return std::signbit(d) ? "-0.0" : "0.0";
    char buf[48];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*e", prec - 1, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::string s = buf;
    bool neg = s[0] == '-';
    if (neg) s.erase(0, 1);
    size_t e = s.find('e');
    int exp10 = std::atoi(s.c_str() + e + 1);
    std::string digits;
    for (size_t i = 0; i < e; ++i)
        if (s[i] != '.') digits += s[i];
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string out;
    if (exp10 >= -4 && exp10 < 16) {
        int decpt = exp10 + 1;
        if (decpt <= 0) out = "0." + std::string(static_cast<size_t>(-decpt), '0') + digits;
        else if (static_cast<size_t>(decpt) >= digits.size())
            out = digits + std::string(static_cast<size_t>(decpt) - digits.size(), '0') + ".0";
        else out = digits.substr(0, static_cast<size_t>(decpt)) + "." + digits.substr(static_cast<size_t>(decpt));
    } else {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        std::string ex = std::to_string(std::abs(exp10));
        if (ex.size() < 2) ex = "0" + ex;
        out += std::string("e") + (exp10 < 0 ? "-" : "+") + ex;
    }
    return neg ? "-" + out : out;
}

// Python's repr() of a stored value, for the `{v!r}` in error messages.
std::string pyReprValue(const Value& v) {
    if (v.isNull()) return "None";
    if (isBoolValue(v)) return std::get<bool>(v.data) ? "True" : "False";
    if (isIntValue(v)) return std::to_string(std::get<int64_t>(v.data));
    if (isDoubleValue(v)) return pyReprDouble(std::get<double>(v.data));
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
[[maybe_unused]] std::string inferColumnType(const std::vector<std::vector<Value>>& rows, size_t position) {
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

[[maybe_unused]] const char* setOpName(const std::string& op) {
    return op == "SANYUKT" ? "UNION" : op == "SAAJHA" ? "INTERSECT" : "EXCEPT";
}

}  // namespace

// ============================================================================
// public API
// ============================================================================

std::vector<Result> Engine::execute(const std::string& text) {
    auto statements = parseScript(text);
    std::vector<Result> results;
    for (const auto& stmt : statements) results.push_back(executeStatement(*stmt));
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
            results.push_back(executeStatement(*stmt));
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
    std::unique_lock<std::recursive_timed_mutex> guard(instance_->lock, std::defer_lock);
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(instance_->lockTimeoutSeconds));
    if (!guard.try_lock_for(wait))
        throw ExecutionError("Database busy hai -- kisi aur session ka transaction chal raha hai. Thodi der baad try karo.");
    if (!fs::is_directory(instance_->dbDir(currentDb))) {
        std::string gone = currentDb;
        currentDb = DEFAULT_DATABASE;
        throw ExecutionError("Database '" + gone + "' ab exist nahi karta. Ab '" + DEFAULT_DATABASE + "' use ho raha hai.");
    }
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
    throw ExecutionError("Ye statement abhi supported nahi hai");
}

void Engine::close() {
    if (inTransaction()) {
        try {
            executeStatement(ast::Rollback());
        } catch (const MeraDBError&) {
        }
    }
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
    txnDb = currentDb;
    return messageResult("Transaction SHURU. PAKKA se save karo, WAPAS se sab undo.");
}

Result Engine::execCommit(const ast::Commit&) {
    if (!inTransaction()) throw ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)");
    instance_->discardSnapshot(*txnDb);
    txnDb.reset();
    instance_->lock.unlock();
    return messageResult("Transaction PAKKA -- saare changes save ho gaye");
}

Result Engine::execRollback(const ast::Rollback&) {
    if (!inTransaction()) throw ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)");
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
    long pkCount = std::count_if(stmt.columns.begin(), stmt.columns.end(), [](const ast::ColumnDef& c) { return c.primaryKey; });
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
// Not yet implemented (later tasks)
// ============================================================================

namespace {
[[noreturn]] void notYet(const char* what) { throw ExecutionError(std::string(what) + " abhi supported nahi hai"); }
}  // namespace

Result Engine::execInsert(const ast::Insert&) { notYet("DAALO"); }
Result Engine::execUpdate(const ast::Update&) { notYet("BADLO"); }
Result Engine::execDelete(const ast::Delete&) { notYet("MITAO"); }
Result Engine::execSelect(const ast::Select&) { notYet("DIKHAO"); }
Result Engine::execSetOp(const ast::SetOp&) { notYet("SetOp"); }
Result Engine::execCreateView(const ast::CreateView&) { notYet("BANAO VIEW"); }
Result Engine::execDropView(const ast::DropView&) { notYet("HATAO VIEW"); }
Result Engine::execShowViews(const ast::ShowViews&) { notYet("DIKHAO VIEWS"); }
Result Engine::execExplain(const ast::Explain&) { notYet("SAMJHAO"); }

}  // namespace meradb
