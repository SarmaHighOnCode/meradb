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
#include "meradb/proc_ast.h"
#include "meradb/pyvalue.h"
#include "meradb/substitute.h"
#include "meradb/table.h"
#include "meradb/users.h"
#include <functional>
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

    // Extra COPIES of a database folder (data/.wapas/<db>.<tag>.sp) for SAVEPOINTs and for undoing one statement whose
    // trigger raised SIGNAL. Unlike the SHURU snapshot a copy survives being restored from, so ROLLBACK TO can be repeated.
    void takeCopy(const std::string& db, const std::string& tag);
    void restoreCopy(const std::string& db, const std::string& tag);
    void dropCopy(const std::string& db, const std::string& tag);

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

class Engine : private SessionHost {
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
    // SET ROLE: the roles in force for `user`. nullopt = every role granted to it (the default); empty = none.
    std::optional<std::set<std::string>> activeRoles;
    bool rolesInit_ = false;                     // the user's default roles are applied once, at the first statement
    std::string isolation_ = "REPEATABLE READ";  // what SET TRANSACTION ISOLATION LEVEL asked for (see execSetIsolation)
    std::optional<std::string> txnDb;  // database of the open transaction, if any
    bool inTransaction() const { return txnDb.has_value(); }
    // SET autocommit = 0: every statement that changes something opens a transaction (if none is open) that stays open until PAKKA / WAPAS.
    bool autocommit() const { return autocommit_; }
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
    Result execAlterModifyColumn(const ast::AlterModifyColumn&);
    Result execAlterAddForeignKey(const ast::AlterAddForeignKey&);
    Result execAlterDropForeignKey(const ast::AlterDropForeignKey&);
    Result execCreateTableLike(const ast::CreateTable&);
    Result execCreateTableAs(const ast::CreateTable&);
    Result execReplace(Table& table, const ast::Insert& stmt, std::vector<std::vector<Value>> newRows);
    Result execRenameTable(const ast::RenameTable&);
    Result execRenameColumn(const ast::RenameColumn&);
    Result execBegin(const ast::Begin&);
    Result execCommit(const ast::Commit&);
    Result execRollback(const ast::Rollback&);
    Result execSavepoint(const ast::Savepoint&);
    Result execRollbackToSavepoint(const ast::RollbackToSavepoint&);
    Result execReleaseSavepoint(const ast::ReleaseSavepoint&);
    Result execSetVariable(const ast::SetVariable&);
    Result execShowVariables(const ast::ShowVariables&);
    Result execPrepare(const ast::Prepare&);
    Result execExecutePrepared(const ast::ExecutePrepared&);
    Result execDeallocate(const ast::Deallocate&);
    Result execCreateFunction(const ast::CreateFunction&);
    Result execDropFunction(const ast::DropFunction&);
    Result execShowFunctions(const ast::ShowFunctions&);

    // ---- transactions, autocommit, savepoints ----
    bool autocommit_ = true;
    bool implicitTxn_ = false;  // the open transaction was started by autocommit = 0 (SHURU then commits it first)
    struct SavepointEntry { std::string name, tag; };
    std::vector<SavepointEntry> savepoints_;
    int savepointSeq_ = 0;
    void beginTransaction();
    void finishTransaction(bool commit);
    void clearSavepoints();
    void setAutocommit(bool on);
    static bool startsTransaction(const ast::Statement& stmt);
    static bool isReadOnlyStatement(const ast::Statement& stmt);
    // Runs a DAALO / BADLO / MITAO; if a trigger raises SIGNAL the whole statement is undone.
    Result guardSignal(const std::string& table, const std::function<Result()>& run);

    // ---- session variables and prepared statements ----
    std::unordered_map<std::string, Value> userVars_;        // "@name" -> value
    std::unordered_map<std::string, std::string> prepared_;  // name -> SQL text with ? marks
    // SessionHost (evaluator.h): what the evaluator asks the engine for
    Value userVariable(const std::string& name) override;
    bool hasFunction(const std::string& name) override;
    Value callFunction(const std::string& name, const std::vector<Value>& args) override;

    // ---- procedural bodies (procedural.cpp) ----
    // One run of a trigger / procedure / function body: its variables, cursors and the rows NAYA / PURANA stand for.
    struct ProcFrame {
        struct Var {
            std::string type;
            Value value;
        };
        struct Cursor {
            std::string selectText;
            bool open = false;
            std::vector<std::vector<Value>> rows;
            size_t position = 0;
        };
        std::unordered_map<std::string, Var> vars;
        std::unordered_map<std::string, Cursor> cursors;
        const ast::PDeclareHandler* notFound = nullptr;  // DECLARE CONTINUE HANDLER FOR NOT FOUND
        const Row* newView = nullptr;                    // NAYA (trigger)
        Row* newRow = nullptr;                           // the same row when this trigger may change it
        const Row* oldRow = nullptr;                     // PURANA (trigger)
        std::string newError;                            // why NAYA cannot be changed here
        bool newChanged = false;
        std::vector<Result>* results = nullptr;  // the results of the statements run (procedures)
        bool isFunction = false;
        std::string flowLabel;  // the label a LEAVE / ITERATE in flight is heading for
        Value returnValue;
        bool returned = false;
    };
    enum class ProcFlow { Next, Leave, Iterate, Return };
    static constexpr int64_t kMaxLoopIterations = 100000;
    static constexpr int kMaxFunctionDepth = 32;
    int functionDepth_ = 0;  // > 0: a stored function body is running (it may only read)
    void runBodyTree(const std::string& bodyText, ProcFrame& frame);
    ProcFlow runProcBlock(const ast::ProcBlock& block, ProcFrame& frame);
    ProcFlow runProcNode(const ast::ProcNode& node, ProcFrame& frame);
    ProcFlow runProcLoop(const ast::PLoop& loop, ProcFrame& frame);
    void runProcSql(const std::string& text, ProcFrame& frame);
    void runProcSelectInto(const ast::PSelectInto& node, ProcFrame& frame);
    void runProcCursorOp(const ast::PCursorOp& node, ProcFrame& frame);
    Result runProcQuery(const std::string& text, ProcFrame& frame);
    Value evalExprValue(const ast::Expr& expr, ProcFrame* frame);  // frame may be null (top level)
    RefReplacer frameReplacer(ProcFrame& frame);
    void assignVariable(ProcFrame* frame, const std::string& name, Value value);
    Result callProcedure(const std::string& name, const std::vector<const ast::Expr*>& args, ProcFrame* caller);
    void checkBodyAllowed(const ast::ProcBlock& block, const std::string& kind, const std::string& trigTiming,
                          const std::string& trigEvent);
    // WITH CHECK OPTION: called with the rows an INSERT / UPDATE is about to write (in the target table's column order);
    // throws when one of them would not be visible through the view.
    using RowCheck = std::function<void(const TableSchema&, const std::vector<std::vector<Value>>&)>;
    Result execInsert(const ast::Insert&, const RowCheck* check = nullptr);
    Result execUpdate(const ast::Update&, const RowCheck* check = nullptr);
    Result execDelete(const ast::Delete&);
    // DAALO / BADLO / MITAO on a simple view are rewritten onto its one base table (see "updatable views" in engine.cpp)
    struct ViewTarget;
    std::unique_ptr<ViewTarget> viewTargetOf(const std::string& name, const std::string& queryText, std::string& reason);
    std::unique_ptr<ViewTarget> updatableView(const std::string& name);  // throws the "ek VIEW hai" error if it is not
    RowCheck viewCheck(const ViewTarget& target);
    Result execInsertViaView(const ast::Insert&);
    Result execUpdateViaView(const ast::Update&);
    Result execDeleteViaView(const ast::Delete&);
    Result execSelect(const ast::Select&);
    Result execSetOp(const ast::SetOp&);
    Result execCreateView(const ast::CreateView&);
    Result execDropView(const ast::DropView&);
    Result execShowViews(const ast::ShowViews&);
    Result execCreateIndex(const ast::CreateIndex&);
    Result execDropIndex(const ast::DropIndex&);
    Result execShowIndex(const ast::ShowIndex&);
    Result execSetDefaultRole(const ast::SetDefaultRole&);
    Result execSetIsolation(const ast::SetIsolation&);
    Result execShowDatabases(const ast::ShowDatabases&);
    Result execShowTriggers(const ast::ShowTriggers&);
    Result execShowProcedures(const ast::ShowProcedures&);
    Result execShowGrants(const ast::ShowGrants&);
    Result execShowCreateTable(const ast::ShowCreateTable&);
    Result execShowCreateView(const ast::ShowCreateView&);
    Result execCreateRole(const ast::CreateRole&);
    Result execDropRole(const ast::DropRole&);
    Result execGrantRole(const ast::GrantRole&);
    Result execRevokeRole(const ast::RevokeRole&);
    Result execSetRole(const ast::SetRole&);
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

    // Trigger / procedure bodies (each firing parses the body fresh and substitutes NAYA/PURANA before running it). `newRow`/`oldRow` are plain
    // {column: value} dicts, independent of any Scope's "table.col" aliasing.
    // Read-only NAYA/PURANA (AFTER triggers, and the REPLACE / TAKRAAV paths where a changed NAYA cannot be written back).
    void fireTriggers(const std::string& timing, const std::string& event, const std::string& table,
                      const Row* newRow, const Row* oldRow);
    // A BEFORE DAALO / BADLO trigger may change the row about to be written (RAKHO NAYA.col = ...): true if one did.
    bool fireBeforeTriggers(const std::string& event, const std::string& table, Row& newRow, const Row* oldRow);
    bool fireTriggersImpl(const std::string& timing, const std::string& event, const std::string& table,
                          const Row* newView, Row* newWritable, const Row* oldRow, const std::string& cannotWrite);
    // Rebuilds a row's values (schema order) from a trigger-edited {column: value} dict, validated like any new row.
    std::vector<Value> rowFromDict(const TableSchema& schema, const Row& dict) const;
    static Row rowDict(const TableSchema& schema, const std::vector<Value>& values);

    static constexpr int kMaxBodyDepth = 32;
    int bodyDepth_ = 0;  // trigger/procedure bodies currently executing on this session

    // ---- helpers ----
    void noTransaction(const std::string& command) const;
    void checkPrivileges(const ast::Statement& stmt);
    void requirePrivilege(const std::string& privilege, const std::string& table);  // table-wide privilege
    Access accessOf(const std::string& privilege, const std::string& table);        // the session's own + active roles
    Access requireAccess(const std::string& privilege, const std::string& table);   // table-wide or some columns
    [[noreturn]] void denyColumn(const std::string& privilege, const std::string& table, const std::string& column) const;
    struct PrivScope;
    void checkSelectAccess(const ast::Select& stmt, const PrivScope* outer);
    void checkExprSubqueries(const ast::Expr* expr);
    void checkGrantAllowed(const ast::Grant& stmt);
    static std::string nowText();
    static std::vector<std::string> tablesRead(const ast::Select& stmt);
    // A REAL table only (DAALO/BADLO/MITAO/SUDHARO/SAAF/SIKODO/HATAO TABLE targets).
    std::unique_ptr<Table> table(const std::string& name);
    // A SE/MILAO source: a real table, or a VIEW materialized fresh.
    std::unique_ptr<Table> resolveSource(const std::string& name);
    // A query result as a read-only table (a VIEW's rows, a derived table's rows).
    static std::unique_ptr<Table> materialize(const std::string& name, const Result& result);
    static constexpr int kMaxViewDepth = 32;  // views defined over views (Python: none, RecursionError)
    int viewDepth_ = 0;
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

    // SANDARBH, parent side: what a DELETE / UPDATE does to the rows that point at it (RESTRICT / CASCADE / SET NULL).
    // The whole knock-on set (every table, recursively) is worked out in memory first -- fkDelete / fkUpdate throw on
    // the first RESTRICT -- checked by fkValidate, and only then written by fkApply, so a failure changes nothing.
    struct FkWork;
    struct FkChange {
        int64_t rowId;
        std::vector<Value> oldValues, newValues;
    };
    void fkDelete(FkWork& work, const std::string& tableName, const std::vector<StoredRow>& rows);
    void fkUpdate(FkWork& work, const std::string& tableName, const std::vector<FkChange>& changes, bool cascaded);
    void fkValidate(FkWork& work);
    void fkApply(FkWork& work);
    static constexpr int kMaxFkDepth = 64;  // nested cascades (a table cascading into itself through a chain)
    void checkSchemaExtras(const TableSchema& schema) const;  // one AUTO_INCREMENT, SET NULL only on nullable columns
    void checkFkNameFree(const std::string& name, const std::string& exceptTable);
    std::optional<int64_t> findConflict(Table& table, const std::vector<Value>& values);
    std::vector<StoredRow> candidates(Table& table, const std::optional<IndexLookup>& access);

    // SELECT
    std::unique_ptr<SelectPlan> planSelect(const ast::Select& stmt);
    Scope selectSourcesScope(const ast::Select& stmt, std::vector<std::unique_ptr<Table>>* tablesOut);
    std::unique_ptr<ast::Select> correlateSelect(const ast::Select& stmt, const Scope& subqueryScope,
                                                 const Row& outerRow, const std::vector<std::string>& outerKeys,
                                                 bool& fired);
    std::unique_ptr<ast::SetOp> correlateSetOp(const ast::SetOp& stmt, const Row& outerRow,
                                               const std::vector<std::string>& outerKeys, bool& fired);
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
