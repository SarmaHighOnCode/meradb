// cpp/include/meradb/ast.h
#pragma once
#include "meradb/datatypes.h"
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
};

struct Select;  // forward decl — Subquery wraps a full Select statement

struct Subquery : Expr {
    std::unique_ptr<Select> statement;
};

struct InSubquery : Expr {
    std::unique_ptr<Expr> left;
    std::unique_ptr<Subquery> subquery;
    bool negated = false;
};

struct Coalesce : Expr {
    std::vector<std::unique_ptr<Expr>> args;
};

struct CaseWhen : Expr {
    std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> branches;
    std::unique_ptr<Expr> elseExpr;  // nullptr if absent
};

// ---- database-level statements ----
struct CreateDatabase : Statement { std::string name; };
struct DropDatabase : Statement { std::string name; };
struct UseDatabase : Statement { std::string name; };
struct ShowTables : Statement {};
struct Describe : Statement { std::string table; };
struct CreateView : Statement { std::string name; std::string queryText; };
struct DropView : Statement { std::string name; };
struct ShowViews : Statement {};

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
};

struct CreateTable : Statement {
    std::string name;
    std::vector<ColumnDef> columns;
    std::vector<std::vector<std::string>> compositeUnique;
    std::optional<std::vector<std::string>> compositePk;
};

struct AlterAddComposite : Statement {
    std::string table;
    std::string kind;  // "ANOKHA" | "MUKHYA"
    std::vector<std::string> columns;
};

struct DropTable : Statement { std::string name; };
struct AlterAddColumn : Statement { std::string table; ColumnDef column; };
struct AlterDropColumn : Statement { std::string table; std::string column; };
struct RenameTable : Statement { std::string table; std::string newName; };
struct RenameColumn : Statement { std::string table; std::string column; std::string newName; };
struct TruncateTable : Statement { std::string name; };
struct CompactTable : Statement { std::string name; };

// ---- transactions ----
struct Begin : Statement {};
struct Commit : Statement {};
struct Rollback : Statement {};
struct Explain : Statement { std::unique_ptr<Statement> statement; };

// ---- DML ----
struct Insert : Statement {
    std::string table;
    std::optional<std::vector<std::string>> columns;
    std::vector<std::vector<std::unique_ptr<Expr>>> rows;  // empty if `select` is used instead
    std::unique_ptr<Select> select;                         // nullptr if `rows` is used instead
    std::optional<std::vector<std::pair<std::string, std::unique_ptr<Expr>>>> onConflictUpdate;
};

struct OrderItem { std::unique_ptr<Expr> expr; bool descending = false; };

struct Join {
    std::string table;
    std::string alias;
    std::unique_ptr<Expr> on;  // nullptr for NATURAL (synthesized later by the planner)
    std::string kind = "INNER";  // INNER|LEFT|RIGHT|FULL|NATURAL
};

struct Select : Statement {
    std::vector<std::unique_ptr<Expr>> columns;
    std::string table;
    std::optional<std::string> alias;
    std::vector<Join> joins;
    std::unique_ptr<Expr> where;
    std::vector<std::unique_ptr<Expr>> groupBy;
    std::unique_ptr<Expr> having;
    std::vector<OrderItem> orderBy;
    std::optional<int> limit;
    bool distinct = false;
    std::vector<std::optional<std::string>> aliases;  // parallel to `columns`
};

struct SetOp : Statement {
    std::string op;  // SANYUKT | SAAJHA | CHHODKAR
    std::unique_ptr<Statement> left;
    std::unique_ptr<Statement> right;
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

}  // namespace meradb::ast
