// cpp/include/meradb/ast_util.h
#pragma once
#include "meradb/ast.h"
#include <memory>

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

}  // namespace meradb
