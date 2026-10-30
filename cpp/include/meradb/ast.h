// cpp/include/meradb/ast.h
#pragma once
#include "meradb/datatypes.h"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace meradb::ast {

using meradb::Value;

// ---- base classes ----
struct Expr { virtual ~Expr() = default; };
struct Statement { virtual ~Statement() = default; };

// ---- expressions ----
struct Literal : Expr {
    Value value;
    explicit Literal(Value v) : value(std::move(v)) {}
};

struct ColumnRef : Expr {
    std::string name;
    std::optional<std::string> table;
    // Set by the planner's bind(): `table` is then the resolved source alias
    // and the row-map key is "table.name" (Python stores that whole string in
    // `name`). An UNBOUND qualified ref `t.col` is looked up by `col` alone,
    // like Python's evaluator does.
    bool bound = false;
    explicit ColumnRef(std::string n, std::optional<std::string> t = std::nullopt)
        : name(std::move(n)), table(std::move(t)) {}
};

struct Star : Expr {
    std::optional<std::string> table;
};

struct BinaryOp : Expr {
    std::string op;
    std::unique_ptr<Expr> left, right;
    BinaryOp(std::string o, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r)
        : op(std::move(o)), left(std::move(l)), right(std::move(r)) {}
};

struct UnaryOp : Expr {
    std::string op;  // "-" or "NAHI"
    std::unique_ptr<Expr> operand;
};

struct IsNull : Expr {
    std::unique_ptr<Expr> expr;
    bool negated = false;
};

struct FuncCall : Expr {
    std::string name;          // aggregate name, upper-case
    std::unique_ptr<Expr> arg; // Star for "*"
    bool distinct = false;     // GINO(ALAG x): each different value counts once
};

struct Select;  // forward decl — Subquery wraps a full Select statement

struct Subquery : Expr {
    std::unique_ptr<Select> statement;
};

struct InSubquery : Expr {
    std::unique_ptr<Expr> left;  // nullptr for EXISTS
    std::unique_ptr<Subquery> subquery;
    bool negated = false;
    // IN (x MEIN (...)) | EXISTS (EXISTS (...)) | ANY / ALL (x op ANY|SOME|ALL (...), `op` is the comparison).
    std::string kind = "IN";
    std::string op;
};

// A scalar (row-by-row) function such as UPPER(x) or DATEDIFF(a, b): `name` is the upper-case spelling the
// user wrote. CAST(x AS type) is the same node with name "CAST", one argument and `castType` set (INT, FLOAT,
// TEXT, DATE or BOOL).
struct ScalarFunc : Expr {
    std::string name;
    std::vector<std::unique_ptr<Expr>> args;
    std::string castType;
    // A call of a stored function (BANAO FUNCTION): `name` is then the identifier as written (lower-case) and the
    // engine, not the built-in table, evaluates it.
    bool user = false;
};

struct Coalesce : Expr {
    std::vector<std::unique_ptr<Expr>> args;
};

struct CaseWhen : Expr {
    std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> branches;
    std::unique_ptr<Expr> elseExpr;  // nullptr if absent
};

// ---- database-level statements ----
struct CreateDatabase : Statement { std::string name; bool ifNotExists = false; };
struct DropDatabase : Statement { std::string name; bool ifExists = false; };
struct UseDatabase : Statement { std::string name; };
struct ShowTables : Statement {};
struct Describe : Statement { std::string table; };
// BANAO [OR REPLACE | BADAL] VIEW / SUDHARO VIEW name KAHO DIKHAO ... [WITH [LOCAL|CASCADED] CHECK OPTION]
struct CreateView : Statement {
    std::string name;
    std::string queryText;     // the DIKHAO text only (WITH CHECK OPTION is not part of it)
    bool orReplace = false;    // BANAO OR REPLACE VIEW: an existing view of that name is replaced
    bool alter = false;        // SUDHARO VIEW: the view must exist already
    bool checkOption = false;  // WITH CHECK OPTION
};
struct DropView : Statement { std::string name; bool ifExists = false; };
struct ShowViews : Statement {};
// BANAO [ANOKHA] INDEX [IF NOT EXISTS] naam PAR table (a, b)   /   HATAO INDEX [IF EXISTS] naam PAR table   /   SHOW INDEX FROM table
struct CreateIndex : Statement {
    std::string name, table;
    std::vector<std::string> columns;
    bool unique = false, ifNotExists = false;
};
struct DropIndex : Statement { std::string name, table; bool ifExists = false; };
struct ShowIndex : Statement { std::string table; };
// SHOW family (English SHOW, or DIKHAO for the first four)
struct ShowDatabases : Statement {};
struct ShowTriggers : Statement {};
struct ShowProcedures : Statement {};
struct ShowGrants : Statement { std::optional<std::string> user; };
struct ShowCreateTable : Statement { std::string name; };
struct ShowCreateView : Statement { std::string name; };

// ---- DDL ----
struct ColumnDef {
    std::string name;
    std::string typeName;
    bool primaryKey = false;
    bool notNull = false;
    bool unique = false;
    std::optional<Value> defaultValue;
    std::optional<int> maxLength;
    std::optional<std::string> refTable;
    std::optional<std::string> refColumn;
    std::optional<std::string> check;  // CHECK source text
    bool autoIncrement = false;             // AUTO_INCREMENT / AUTO_ANK
    std::vector<std::string> enumValues;    // ENUM('a','b'): typeName is TEXT
    std::string onDelete, onUpdate;         // SANDARBH ... ON DELETE|UPDATE CASCADE | SET NULL | RESTRICT | NO ACTION
};

// A table-level `[CONSTRAINT name] FOREIGN KEY (column) SANDARBH parent(col) [ON ...]` (CREATE TABLE and ALTER).
struct ForeignKeyDef {
    std::string name;  // empty = auto-named
    std::string column;
    std::string refTable, refColumn;
    std::string onDelete, onUpdate;
};

struct CreateTable : Statement {
    std::string name;
    bool ifNotExists = false;  // BANAO TABLE IF NOT EXISTS: an existing table of that name is left alone
    std::vector<ColumnDef> columns;
    std::vector<std::vector<std::string>> compositeUnique;
    std::optional<std::vector<std::string>> compositePk;
    std::vector<ForeignKeyDef> foreignKeys;  // table-level FOREIGN KEY items
    std::unique_ptr<Statement> asQuery;      // BANAO TABLE t KAHO DIKHAO ... (a Select or SetOp); `columns` is empty then
    std::string likeTable;                   // BANAO TABLE t LIKE other; `columns` is empty then
};

struct AlterAddComposite : Statement {
    std::string table;
    std::string kind;  // "ANOKHA" | "MUKHYA"
    std::vector<std::string> columns;
};

struct DropTable : Statement { std::string name; bool ifExists = false; };
struct AlterAddColumn : Statement {
    std::string table;
    ColumnDef column;
    std::string position;     // "" (last) | "FIRST" | "AFTER"
    std::string afterColumn;  // for "AFTER"
};
// SUDHARO TABLE t CHANGE old coldef (rename + retype)  |  MODIFY coldef (retype, same name)
struct AlterModifyColumn : Statement { std::string table; std::string oldName; ColumnDef column; bool rename = false; };
struct AlterAddForeignKey : Statement { std::string table; ForeignKeyDef foreignKey; };
struct AlterDropForeignKey : Statement { std::string table; std::string name; };
struct AlterDropColumn : Statement { std::string table; std::string column; };
struct RenameTable : Statement { std::string table; std::string newName; };
struct RenameColumn : Statement { std::string table; std::string column; std::string newName; };
struct TruncateTable : Statement { std::string name; };
struct CompactTable : Statement { std::string name; };

// ---- transactions ----
struct Begin : Statement {};
struct Commit : Statement {};
struct Rollback : Statement {};
// SAVEPOINT name / ROLLBACK TO [SAVEPOINT] name (WAPAS TAK name) / RELEASE SAVEPOINT name: inside an open transaction
struct Savepoint : Statement { std::string name; };
struct RollbackToSavepoint : Statement { std::string name; };
struct ReleaseSavepoint : Statement { std::string name; };
// SET autocommit = 0|1|ON|OFF, SET @x = expr [, @y = expr ...]
struct SetVariable : Statement { std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments; };
// SHOW VARIABLES [LIKE 'pattern']
struct ShowVariables : Statement { std::string pattern; bool hasPattern = false; };
// PREPARE name FROM 'sql with ? marks' / EXECUTE name [USING a, b] / DEALLOCATE PREPARE name (HATAO PREPARE name)
struct Prepare : Statement { std::string name; std::unique_ptr<Expr> source; };
struct ExecutePrepared : Statement { std::string name; std::vector<std::unique_ptr<Expr>> args; };
struct Deallocate : Statement { std::string name; };
struct Explain : Statement { std::unique_ptr<Statement> statement; };

// ---- DML ----
struct Insert : Statement {
    std::string table;
    std::optional<std::vector<std::string>> columns;
    std::vector<std::vector<std::unique_ptr<Expr>>> rows;  // empty if `select` is used instead
    std::unique_ptr<Select> select;                         // nullptr if `rows` is used instead
    std::optional<std::vector<std::pair<std::string, std::unique_ptr<Expr>>>> onConflictUpdate;
    bool replace = false;  // REPLACE INTO / BADAL DAALO MEIN: colliding rows are deleted first
};

struct OrderItem { std::unique_ptr<Expr> expr; bool descending = false; };

struct Select;  // a derived table (subquery in FROM / MILAO) owns a full Select

struct Join {
    std::string table;  // empty for a derived table
    std::string alias;
    std::unique_ptr<Expr> on;  // nullptr for NATURAL / USING (synthesized later by the planner)
    std::string kind = "INNER";  // INNER|LEFT|RIGHT|FULL|NATURAL
    std::vector<std::string> usingColumns;  // USING (a, b): the planner turns them into `earlier.a = this.a AUR ...`
    std::unique_ptr<Select> derived;        // MILAO (DIKHAO ...) alias ...
};

struct Select : Statement {
    std::vector<std::unique_ptr<Expr>> columns;
    std::string table;  // empty when `derived` is the FROM source
    std::optional<std::string> alias;
    std::unique_ptr<Select> derived;  // SE (DIKHAO ...) alias
    std::vector<Join> joins;
    std::unique_ptr<Expr> where;
    std::vector<std::unique_ptr<Expr>> groupBy;
    std::unique_ptr<Expr> having;
    std::vector<OrderItem> orderBy;
    std::optional<int> limit;
    std::optional<int> offset;  // SIRF n OFFSET m  /  SIRF m, n
    bool distinct = false;
    std::string lockMode;  // FOR UPDATE / FOR SHARE / LOCK IN SHARE MODE: accepted, a transaction already locks the database
    std::vector<std::optional<std::string>> aliases;  // parallel to `columns`
    // A subquery or derived table that is a set operation: (DIKHAO a SE x SANYUKT DIKHAO b SE y). When set, it is the
    // whole query and every field above is empty.
    std::unique_ptr<struct SetOp> compound;
};

struct SetOp : Statement {
    std::string op;  // SANYUKT | SAAJHA | CHHODKAR
    bool all = false;  // SANYUKT ALL: keeps the copies
    std::unique_ptr<Statement> left;
    std::unique_ptr<Statement> right;
    // KRAM / SIRF written after the last side, when no earlier side has clauses of its own: they apply to the whole result
    std::vector<OrderItem> orderBy;
    std::optional<int> limit;
    std::optional<int> offset;
};

struct Update : Statement {
    std::string table;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments;
    std::unique_ptr<Expr> where;
};

struct Delete : Statement {
    std::string table;
    std::unique_ptr<Expr> where;
};

// ---- users & privileges (Phase 2) ----
struct CreateUser : Statement { std::string name; std::string password; bool ifNotExists = false; };
struct DropUser : Statement { std::string name; bool ifExists = false; };
struct Grant : Statement {
    std::vector<std::string> privileges;  // DIKHAO | DAALO | BADLO | MITAO (SAB is expanded by the parser)
    // privilege -> column list, for `DIKHAO (a, b)` style grants (DIKHAO / DAALO / BADLO only); absent = whole table
    std::map<std::string, std::vector<std::string>> columns;
    std::string table;                    // a table OR a view name
    std::string user;                     // the grantee: a user OR a role
    bool withGrantOption = false;         // WITH GRANT OPTION
};
struct Revoke : Statement {
    std::vector<std::string> privileges;
    std::map<std::string, std::vector<std::string>> columns;
    std::string table;
    std::string user;
};
// roles: BANAO ROLE r / HATAO ROLE r / ADHIKAR DO ROLE r KO user / ADHIKAR WAPAS ROLE r SE user / SET ROLE ...
struct CreateRole : Statement { std::string name; bool ifNotExists = false; };
struct DropRole : Statement { std::string name; bool ifExists = false; };
struct GrantRole : Statement { std::vector<std::string> roles; std::string user; };
struct RevokeRole : Statement { std::vector<std::string> roles; std::string user; };
// SET DEFAULT ROLE ALL | NONE | r [, r] TO user: the roles a user's session starts with
struct SetDefaultRole : Statement {
    std::vector<std::string> roles;
    bool all = false, none = false;
    std::string user;
};
// SET [SESSION | GLOBAL] TRANSACTION ISOLATION LEVEL READ UNCOMMITTED | READ COMMITTED | REPEATABLE READ | SERIALIZABLE
struct SetIsolation : Statement { std::string level; };
struct SetRole : Statement {
    std::vector<std::string> roles;
    bool all = false;   // SET ROLE SAB / ALL: every granted role (the default)
    bool none = false;  // SET ROLE NONE: no role
};

// ---- triggers & stored procedures (Phase 2) ----
struct CreateTrigger : Statement {
    std::string name;
    std::string timing;    // PEHLE | BAAD
    std::string event;     // DAALO | BADLO | MITAO
    std::string table;
    std::string bodyText;  // raw source between SHURU and KHATAM, ending with the last ';'
    bool ifNotExists = false;  // BANAO TRIGGER IF NOT EXISTS
};
struct DropTrigger : Statement { std::string name; bool ifExists = false; };

struct ProcParam {
    std::string name;
    std::string typeName;  // normalised (INT, TEXT, ...)
    std::string mode = "IN";  // IN | OUT | INOUT (procedures only)
};
struct CreateProcedure : Statement {
    std::string name;
    std::vector<ProcParam> params;
    std::string bodyText;
};
struct DropProcedure : Statement { std::string name; bool ifExists = false; };
// BANAO FUNCTION f(a INT, b INT) RETURNS INT SHURU ... RETURN expr; KHATAM
struct CreateFunction : Statement {
    std::string name;
    std::vector<ProcParam> params;
    std::string returnType;
    std::string bodyText;
};
struct DropFunction : Statement { std::string name; bool ifExists = false; };
struct ShowFunctions : Statement {};
struct CallProcedure : Statement {
    std::string name;
    std::vector<std::unique_ptr<Expr>> args;
};

}  // namespace meradb::ast
