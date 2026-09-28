// cpp/include/meradb/planner.h
//
// The PLANNER: decides HOW to run a query, before a single row is read.
// Mirrors meradb/planner.py.
//
//     parser --> AST --> PLANNER --> plan --> engine executes it
//
// 1. BINDING (name resolution): every column reference is rewritten to its
//    fully-qualified "alias.column" key (`naam` -> `s.naam`), failing early
//    for unknown or AMBIGUOUS columns. A bound ColumnRef carries the alias in
//    `table` and the bare column in `name`; its row-map key is "alias.col".
// 2. ACCESS PATH: can a hash INDEX answer `JAHAN id = 5`, or FULL SCAN?
// 3. JOIN STRATEGY: equality PAR condition -> HASH JOIN, else NESTED LOOP.
//
// Every decision here is only a speed-up: the engine still re-checks the
// full condition on every row, so the planner can never change an answer.
#pragma once
#include "meradb/ast.h"
#include "meradb/catalog.h"
#include "meradb/datatypes.h"
#include "meradb/table.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace meradb {

// The tables visible in one query: [(alias, schema), ...] in SE/MILAO order.
class Scope {
public:
    // Throws ExecutionError if the same alias appears twice.
    explicit Scope(std::vector<std::pair<std::string, TableSchema>> sources);

    const std::vector<std::pair<std::string, TableSchema>>& sources() const { return sources_; }
    size_t size() const { return sources_.size(); }

    // ---- keys and rows ----
    std::vector<std::string> keys(size_t i) const;  // ["alias.col", ...] for source i
    std::unordered_map<std::string, Value> row(size_t i, const std::vector<Value>& values) const;
    // All-KHALI row for source i (used by BAAYAN MILAO when nothing matches).
    std::unordered_map<std::string, Value> nullRow(size_t i) const;
    std::vector<std::string> allKeys() const;
    size_t sourceIndex(const std::string& key) const;  // "alias.col" -> source position
    // `s.naam` -> `naam` when there is only one table (friendlier messages).
    std::string display(const std::string& key) const;

    // ---- resolution ----
    // "alias.col" for a (qualified or bare) column reference. Throws
    // ExecutionError for an unknown alias/column or an ambiguous bare name.
    std::string resolve(const ast::ColumnRef& ref) const;
    // `*` or `s.*` -> [(header label, ColumnRef(column, alias)), ...]. The
    // label is the bare column name unless that name exists in two tables.
    std::vector<std::pair<std::string, ast::ColumnRef>> expandStar(const ast::Star& star) const;

private:
    std::vector<std::pair<std::string, TableSchema>> sources_;
    std::unordered_map<std::string, size_t> aliases_;                    // alias -> source position
    std::unordered_map<std::string, std::vector<std::string>> byColumn_;  // "naam" -> ["s.naam"]
};

// A copy of `expr` where every ColumnRef is replaced by its bound form.
// Subquery bodies are NOT bound here (the engine binds them against their
// own scope); an InSubquery's left side is.
std::unique_ptr<ast::Expr> bind(const ast::Expr& expr, const Scope& scope);
// Same, but nullptr in -> nullptr out (Python's bind(None) -> None).
std::unique_ptr<ast::Expr> bind(const ast::Expr* expr, const Scope& scope);

// SAMAAN MILAO: `earlier.col = right.col` AUR-ed together for every column
// name the right source shares with any source joined before it (the
// leftmost earlier source wins). Throws if nothing is shared.
std::unique_ptr<ast::Expr> naturalJoinCondition(const Scope& scope, size_t rightIndex);

// Split `a AUR b AUR c` into [a, b, c]. nullptr -> [].
std::vector<const ast::Expr*> conjuncts(const ast::Expr& expr);
std::vector<const ast::Expr*> conjuncts(const ast::Expr* expr);

struct IndexLookup {
    size_t column;           // position of the column in the table
    std::string columnName;
    Value value;             // already coerced to the column's type

    // "INDEX LOOKUP students PAR id = 5  [hash index, MUKHYA KUNJI]"
    std::string describe(const Table& table) const;
};

// Looks for `unique_column = constant` among the AUR-ed parts of a BOUND
// `where` on the first source. nullopt = full scan (always for a VIEW).
std::optional<IndexLookup> chooseAccess(const Table& table, const Scope& scope, const ast::Expr* where);

// PAR (bound) may only use sources joined so far (0..rightIndex).
void checkJoinCondition(const ast::Expr* on, const Scope& scope, size_t rightIndex);

// If a BOUND PAR contains `earlier.x = right.y`, returns (leftKey, rightKey)
// for a HASH JOIN; nullopt means nested loop.
std::optional<std::pair<std::string, std::string>> chooseJoin(const ast::Expr* on, const Scope& scope,
                                                              size_t rightIndex);

}  // namespace meradb
