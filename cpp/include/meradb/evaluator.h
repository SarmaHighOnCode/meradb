// cpp/include/meradb/evaluator.h
//
// Evaluates an expression (from the AST) against ONE row. Mirrors
// meradb/evaluator.py.
//
// KHALI (NULL) follows SQL's THREE-VALUED LOGIC: a comparison with KHALI is
// "unknown" (a null Value), not SACH or JHOOTH.
//
//     KHALI = 5          -> KHALI
//     JHOOTH AUR KHALI   -> JHOOTH     (false AND anything is false)
//     SACH YA KHALI      -> SACH       (true OR anything is true)
//     SACH AUR KHALI     -> KHALI
//
// JAHAN keeps a row only when isTrue(condition), so unknown rows drop out.
#pragma once
#include "meradb/ast.h"
#include "meradb/datatypes.h"
#include "meradb/pyvalue.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace meradb {

// One row as the evaluator sees it: "alias.col" -> value (unbound / CHECK
// contexts use bare column names). A group's precomputed aggregate results
// live in the same map under aggKey(), which can never clash with a column
// key (it contains characters no identifier can).
using Row = std::unordered_map<std::string, Value>;

// A subquery's result, precomputed by the engine for the current outer row
// (the evaluator itself never runs a subquery). Keyed by node identity, like
// Python's id(expr):
//   Subquery   -> `scalar`: already reduced to one value
//   InSubquery -> `values`: the membership list; the evaluator does the
//                 `left MEIN (...)` test itself
struct SubqueryResult {
    Value scalar;
    std::vector<Value> values;
    // MEIN: the non-KHALI `values` as a hash set, built once by the engine and shared by the per-row copies of this
    // result, and whether a KHALI was among them. Without it the evaluator scans `values`.
    std::shared_ptr<const std::unordered_set<Value, PyValueHash, PyValueEq>> members;
    bool sawNull = false;
};
using SubqueryResults = std::unordered_map<const ast::Expr*, SubqueryResult>;

// What the evaluator cannot know by itself: the session's @user_variables and the stored functions. The Engine
// implements it and installs itself (ScopedSessionHost) while a statement runs; with no host, @x is KHALI and every
// function name that is not built in is unknown.
class SessionHost {
public:
    virtual ~SessionHost() = default;
    virtual Value userVariable(const std::string& name) = 0;  // name keeps its '@'; KHALI when never set
    virtual bool hasFunction(const std::string& name) = 0;
    virtual Value callFunction(const std::string& name, const std::vector<Value>& args) = 0;
};
SessionHost* currentSessionHost();
class ScopedSessionHost {
public:
    explicit ScopedSessionHost(SessionHost* host);
    ~ScopedSessionHost();
    ScopedSessionHost(const ScopedSessionHost&) = delete;
    ScopedSessionHost& operator=(const ScopedSessionHost&) = delete;

private:
    SessionHost* previous_;
};

Value evaluate(const ast::Expr& expr, const Row& row, const SubqueryResults* subqueries = nullptr);

// Used by JAHAN/JINKA: only exactly SACH counts. KHALI (unknown) does not.
bool isTrue(const Value& v);

// ---- utilities used by the planner and the engine ----

// Row-map key of a column reference -- the string Python keeps in
// ColumnRef.name: "alias.col" for a BOUND ref, else the bare column name
// (an unbound `t.col` is looked up as `col`, exactly like Python).
std::string refKey(const ast::ColumnRef& ref);

// Every ColumnRef inside an expression, for validation. With skipAggregates,
// columns inside GINO(...)/KUL(...) etc. are ignored. Does not look inside
// subqueries (they are bound and validated separately).
void columnRefs(const ast::Expr& expr, std::vector<const ast::ColumnRef*>& out, bool skipAggregates = false);
// Same walk, returning refKey() of each (Python's column_refs); nullptr -> [].
std::vector<std::string> columnRefKeys(const ast::Expr* expr, bool skipAggregates = false);
// Narrower walk (column/binary/unary/IS KHALI/aggregate only) returning the
// nodes themselves, so a caller can see whether each is qualified. Used to
// validate SHART (CHECK). Python's column_ref_nodes.
void columnRefNodes(const ast::Expr& expr, std::vector<const ast::ColumnRef*>& out);

// Every aggregate call inside an expression (not looking inside one).
void findAggregates(const ast::Expr& expr, std::vector<const ast::FuncCall*>& out);
// Every Subquery/InSubquery node inside an expression (not looking inside a
// subquery's own SELECT).
void findSubqueries(const ast::Expr& expr, std::vector<const ast::Expr*>& out);

// Where a group's aggregate result is stored in the Row map.
std::string aggKey(const ast::FuncCall& func);
// A readable column header for a SELECT expression, e.g. `umar + 1`.
std::string exprLabel(const ast::Expr& expr);

}  // namespace meradb
