// cpp/include/meradb/substitute.h
//
// Generic "replace column references by constants" rewrite, shared by triggers
// (NAYA.col / PURANA.col) and stored procedures (parameter names). Mirrors
// Engine._substitute / _substitute_statement in meradb/engine.py.
#pragma once
#include "meradb/ast.h"
#include <functional>
#include <memory>
#include <optional>

namespace meradb {

// Returns a value to substitute for this reference, or nullopt to leave it alone.
using RefReplacer = std::function<std::optional<Value>(const ast::ColumnRef&)>;

// Replaces matching ColumnRefs in the tree by Literals. A null slot is fine.
// Subquery / InSubquery nodes are deliberately not entered.
void substituteInPlace(std::unique_ptr<ast::Expr>& expr, const RefReplacer& replace);

// Rewrites the expressions of Insert / Update / Delete / Select / CallProcedure;
// any other statement kind is left untouched.
void substituteStatementInPlace(ast::Statement& stmt, const RefReplacer& replace);

}  // namespace meradb
