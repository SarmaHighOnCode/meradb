// cpp/include/meradb/ast_util.h
#pragma once
#include "meradb/ast.h"
#include <memory>
#include <string>

namespace meradb {

// Deep-copies an expression tree. Needed wherever a sub-expression must be
// referenced twice in an owned (unique_ptr) AST -- e.g. desugaring
// `x BEECH a AUR b` or `x MEIN (a, b)`, which both need two independent
// copies of `x`. Covers every meradb::ast::Expr node kind. A later planner
// task reuses this for expression-tree rewriting.
std::unique_ptr<ast::Expr> cloneExpr(const ast::Expr& e);

// Deep-copies a full SELECT statement (used by cloneExpr for Subquery /
// InSubquery nodes, which own a nested Select).
std::unique_ptr<ast::Select> cloneSelect(const ast::Select& s);

// The Python class name of a statement (type(stmt).__name__ in meradb/ast_nodes.py):
// used verbatim in the "superuser nahi hai" privilege error, so it must match
// for every statement kind. Returns "Statement" for an unknown subclass.
std::string astClassName(const ast::Statement& stmt);

}  // namespace meradb
