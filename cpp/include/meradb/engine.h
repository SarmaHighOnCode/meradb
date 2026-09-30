// cpp/include/meradb/engine.h
//
// STAGE 3: the EXECUTOR. Takes a parsed statement and actually does it,
// using the planner (how to run it), the catalog (schemas) and storage
// (rows on disk). Mirrors meradb/engine.py.
//
//   text --tokenizer--> tokens --parser--> AST --planner--> plan --engine--> Result
//
// Two classes live here:
//
//   Instance -- the SHARED state for one data folder: cached catalogs, cached
//               indexes, and the LOCK. (Phase 2's server shares one Instance
//               across many sessions; Phase 1 has one per Engine.)
//   Engine   -- one SESSION: its current database and its open transaction.
#pragma once
#include "meradb/ast.h"
#include "meradb/catalog.h"
#include "meradb/evaluator.h"
#include "meradb/planner.h"
#include "meradb/pyvalue.h"
#include "meradb/substitute.h"
#include "meradb/table.h"
#include "meradb/users.h"
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace meradb {

constexpr const char* DEFAULT_DATABASE = "main";
constexpr const char* SNAPSHOT_DIR = ".wapas";  // where transactions keep their "before" copy

class Instance {
public:
    // `served` is true only inside the server process. Otherwise the constructor refuses a
    // folder that a running server is serving (two processes must never write the same files).
    explicit Instance(std::string dataDir, bool served = false);

    // Every statement runs while holding this; a transaction holds it once
    // more from SHURU until PAKKA/WAPAS (see engine.py's concurrency notes).
    std::recursive_timed_mutex lock;
    double lockTimeoutSeconds = 10.0;

    const std::string& dataDir() const { return dataDir_; }
    UserStore& users() { return users_; }
    std::string dbDir(const std::string& name) const;
    std::vector<std::string> databases() const;  // sorted, hidden (.wapas) excluded
    Catalog& catalog(const std::string& db);     // loaded + cached on first access
    // Drop everything cached for a database (after DROP DATABASE or ROLLBACK).
    void forget(const std::string& db);

    // The index slot for (db, table), shared by every Table object this
    // Instance builds for that table. Each Instance owns its own indexes.
    IndexCache indexCache(const std::string& db, const std::string& table);
    void dropIndexCache(const std::string& db, const std::string& table);

    // Transactions use SHADOW COPIES (every step ends in an atomic rename):
    //   takeSnapshot    copies data/<db> -> data/.wapas/<db>.tmp -> data/.wapas/<db>
    //   discardSnapshot renames data/.wapas/<db> -> <db>.done (COMMIT POINT), deletes it
    //   restoreSnapshot deletes data/<db>, renames data/.wapas/<db> back over it
    void takeSnapshot(const std::string& db);
    void discardSnapshot(const std::string& db);
    void restoreSnapshot(const std::string& db);

    // Databases rolled back by crash recovery when this Instance started.
    const std::vector<std::string>& recovered() const { return recovered_; }

private:
    std::string dataDir_;
    UserStore users_;  // declared after dataDir_: its constructor needs it
    std::unordered_map<std::string, std::unique_ptr<Catalog>> catalogs_;
    std::unordered_map<std::string, std::unordered_map<std::string, IndexCache>> indexes_;  // db -> table -> slot
    std::vector<std::string> recovered_;

    std::string snapshotPath(const std::string& db, const std::string& suffix = "") const;
    std::vector<std::string> recover();
};

// What every statement returns. SELECT fills columns+rows; others a
// message; a failure (runScript only) fills `error`.
struct Result {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    std::string message;
    std::string error;
};

class Engine {
public:
    explicit Engine(std::shared_ptr<Instance> instance);
    explicit Engine(const std::string& dataDir);  // embedded: owns a private Instance
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    std::string currentDb = DEFAULT_DATABASE;
    // The session's authenticated username. nullopt = SUPERUSER (unrestricted):
    // every embedded Engine and every server session that connected without a
    // username. See docs/SERVER.md "no username = superuser".
    std::optional<std::string> user;
    std::optional<std::string> txnDb;  // database of the open transaction, if any
    bool inTransaction() const { return txnDb.has_value(); }
    Catalog& catalog();
    Instance& instance() { return *instance_; }

    // Run `;`-separated statements. Throws on the first error.
    std::vector<Result> execute(const std::string& text);
    // Never throws a MeraDBError: a failing statement gets Result.error
    // ("[Stage Galti] message") and the following statements still run.
    std::vector<Result> runScript(const std::string& text);
    Result executeStatement(const ast::Statement& stmt);
    // End the session: an unfinished transaction is rolled back.
    void close();
    // Every database -> table -> column as plain JSON (what the server sends for the
    // workbench sidebar). Waits at most 2 s for the lock, like Python's schema_tree().
    nlohmann::ordered_json schemaTree();

    struct SelectPlan;  // everything the planner decided about one DIKHAO

private:
    std::shared_ptr<Instance> instance_;
    // The thread that ran SHURU: it owns the extra lock hold (a recursive
    // mutex may only be unlocked by its owner).
    std::thread::id txnThread_;

    // Per-row precomputed subquery results (see precomputeSubqueries).
    struct RowSubqueries {
        SubqueryResults shared;                  // uncorrelated: computed once
        std::vector<SubqueryResults> perRow;     // filled only when something is correlated
        const SubqueryResults* at(size_t i) const { return perRow.empty() ? &shared : &perRow[i]; }
    };

    // Waits up to `timeoutSeconds` for the instance lock; throws the "Database busy hai" error.
    std::unique_lock<std::recursive_timed_mutex> acquireLock(double timeoutSeconds);

    Result guarded(const ast::Statement& stmt);  // executeStatement + std::exception -> StorageError

    // ---- statements ----
    Result execCreateDatabase(const ast::CreateDatabase&);
    Result execDropDatabase(const ast::DropDatabase&);
    Result execUseDatabase(const ast::UseDatabase&);
    Result execShowTables(const ast::ShowTables&);
    Result execDescribe(const ast::Describe&);
    Result execCreateTable(const ast::CreateTable&);
    Result execDropTable(const ast::DropTable&);
    Result execTruncateTable(const ast::TruncateTable&);
    Result execCompactTable(const ast::CompactTable&);
    Result execAlterAddColumn(const ast::AlterAddColumn&);
    Result execAlterAddComposite(const ast::AlterAddComposite&);
    Result execAlterDropColumn(const ast::AlterDropColumn&);
    Result execRenameTable(const ast::RenameTable&);
    Result execRenameColumn(const ast::RenameColumn&);
    Result execBegin(const ast::Begin&);
    Result execCommit(const ast::Commit&);
    Result execRollback(const ast::Rollback&);
    Result execInsert(const ast::Insert&);
    Result execUpdate(const ast::Update&);
    Result execDelete(const ast::Delete&);
    Result execSelect(const ast::Select&);
    Result execSetOp(const ast::SetOp&);
    Result execCreateView(const ast::CreateView&);
    Result execDropView(const ast::DropView&);
    Result execShowViews(const ast::ShowViews&);
    Result execExplain(const ast::Explain&);
    Result execCreateUser(const ast::CreateUser&);
    Result execDropUser(const ast::DropUser&);
    Result execGrant(const ast::Grant&);
    Result execRevoke(const ast::Revoke&);
    Result execCreateTrigger(const ast::CreateTrigger&);
    Result execDropTrigger(const ast::DropTrigger&);
    Result execCreateProcedure(const ast::CreateProcedure&);
    Result execDropProcedure(const ast::DropProcedure&);
    Result execCallProcedure(const ast::CallProcedure&);

    // Trigger / procedure bodies (Design decisions D5). `newRow`/`oldRow` are plain
    // {column: value} dicts, independent of any Scope's "table.col" aliasing.
    void fireTriggers(const std::string& timing, const std::string& event, const std::string& table,
                      const Row* newRow, const Row* oldRow);
    // Parse `bodyText` fresh, substitute, run each statement through executeStatement
    // (so it takes the lock, re-checks privileges and can fire further triggers).
    void runBody(const std::string& bodyText, const RefReplacer& replace, std::vector<Result>* results);
    static Row rowDict(const TableSchema& schema, const std::vector<Value>& values);

    static constexpr int kMaxBodyDepth = 32;
    int bodyDepth_ = 0;  // trigger/procedure bodies currently executing on this session

    // ---- helpers ----
    void noTransaction(const std::string& command) const;
    void checkPrivileges(const ast::Statement& stmt);
    void requirePrivilege(const std::string& privilege, const std::string& table);
    static std::vector<std::string> tablesRead(const ast::Select& stmt);
    // A REAL table only (DAALO/BADLO/MITAO/SUDHARO/SAAF/SIKODO/HATAO TABLE targets).
    std::unique_ptr<Table> table(const std::string& name);
    // A SE/MILAO source: a real table, or a VIEW materialized fresh.
    std::unique_ptr<Table> resolveSource(const std::string& name);
    Column makeColumn(const ast::ColumnDef& def) const;
    const TableSchema& checkFkTarget(const Column& col, const TableSchema& selfSchema);
    void checkShartExpr(const Column& col, const TableSchema& schema) const;

    // DML validation
    std::vector<Value> validateRow(const TableSchema& schema, const std::vector<Value>& values) const;
    void checkUnique(Table& table, const std::vector<std::vector<Value>>& newRows,
                     const std::set<int64_t>& ignoreRowIds = {});
    void checkFk(Table& table, const std::vector<std::vector<Value>>& newRows);
    // Values are compared the way Python's set membership does (pyEquals).
    using ValueSet = std::unordered_set<Value, PyValueHash, PyValueEq>;
    void checkNoChildren(const TableSchema& schema, const std::unordered_map<size_t, ValueSet>& changedByColumn,
                         const std::set<int64_t>& exemptRowIds = {},
                         const std::unordered_map<int64_t, std::vector<Value>>* overrides = nullptr);
    std::optional<int64_t> findConflict(Table& table, const std::vector<Value>& values);
    std::vector<StoredRow> candidates(Table& table, const std::optional<IndexLookup>& access);

    // SELECT
    std::unique_ptr<SelectPlan> planSelect(const ast::Select& stmt);
    Scope selectSourcesScope(const ast::Select& stmt, std::vector<std::unique_ptr<Table>>* tablesOut);
    std::unique_ptr<ast::Select> correlateSelect(const ast::Select& stmt, const Scope& subqueryScope,
                                                 const Row& outerRow, const std::vector<std::string>& outerKeys,
                                                 bool& fired);
    Result runSubquery(const ast::Subquery& sub, const Row& outerRow, const std::vector<std::string>& outerKeys,
                       bool& fired);
    RowSubqueries precomputeSubqueries(const std::vector<const ast::Expr*>& exprs, const std::vector<Row>& rows,
                                       const std::vector<std::string>& outerKeys);
    std::vector<Row> group(const std::vector<Row>& rows, const SelectPlan& plan);

    // EXPLAIN
    std::vector<std::string> explainSelect(const ast::Select& stmt, const SelectPlan& plan);
    std::vector<std::string> explainOne(const ast::Statement& stmt);
    std::vector<std::string> explainSubqueries(const std::vector<const ast::Expr*>& exprs, const Scope& scope);
};

}  // namespace meradb
