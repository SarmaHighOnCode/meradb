// cpp/src/substitute.cpp
#include "meradb/substitute.h"
#include "meradb/errors.h"
#include "meradb/stack_guard.h"

namespace meradb {

using namespace ast;

void substituteInPlace(std::unique_ptr<Expr>& expr, const RefReplacer& replace) {
    if (!expr) return;
    requireStack();
    Expr* node = expr.get();
    if (dynamic_cast<Literal*>(node) || dynamic_cast<Star*>(node)) return;
    if (auto* ref = dynamic_cast<ColumnRef*>(node)) {
        if (auto value = replace(*ref)) expr = std::make_unique<Literal>(std::move(*value));  // `ref` dies here
        return;
    }
    if (auto* bin = dynamic_cast<BinaryOp*>(node)) {
        substituteInPlace(bin->left, replace);
        substituteInPlace(bin->right, replace);
        return;
    }
    if (auto* un = dynamic_cast<UnaryOp*>(node)) {
        substituteInPlace(un->operand, replace);
        return;
    }
    if (auto* isNull = dynamic_cast<IsNull*>(node)) {
        substituteInPlace(isNull->expr, replace);
        return;
    }
    if (auto* fn = dynamic_cast<FuncCall*>(node)) {
        substituteInPlace(fn->arg, replace);
        return;
    }
    if (auto* co = dynamic_cast<Coalesce*>(node)) {
        for (auto& arg : co->args) substituteInPlace(arg, replace);
        return;
    }
    if (auto* cw = dynamic_cast<CaseWhen*>(node)) {
        for (auto& branch : cw->branches) {
            substituteInPlace(branch.first, replace);
            substituteInPlace(branch.second, replace);
        }
        substituteInPlace(cw->elseExpr, replace);
        return;
    }
    if (dynamic_cast<Subquery*>(node) || dynamic_cast<InSubquery*>(node)) return;  // out of scope, like Python
    throw ExecutionError("Unknown expression");
}

void substituteStatementInPlace(Statement& stmt, const RefReplacer& replace) {
    if (auto* ins = dynamic_cast<Insert*>(&stmt)) {
        for (auto& row : ins->rows)
            for (auto& e : row) substituteInPlace(e, replace);
        if (ins->select) substituteStatementInPlace(*ins->select, replace);
        if (ins->onConflictUpdate)
            for (auto& assignment : *ins->onConflictUpdate) substituteInPlace(assignment.second, replace);
        return;
    }
    if (auto* upd = dynamic_cast<Update*>(&stmt)) {
        for (auto& assignment : upd->assignments) substituteInPlace(assignment.second, replace);
        substituteInPlace(upd->where, replace);
        return;
    }
    if (auto* del = dynamic_cast<Delete*>(&stmt)) {
        substituteInPlace(del->where, replace);
        return;
    }
    if (auto* sel = dynamic_cast<Select*>(&stmt)) {
        for (auto& c : sel->columns) substituteInPlace(c, replace);
        for (auto& j : sel->joins) substituteInPlace(j.on, replace);
        substituteInPlace(sel->where, replace);
        for (auto& g : sel->groupBy) substituteInPlace(g, replace);
        substituteInPlace(sel->having, replace);
        for (auto& o : sel->orderBy) substituteInPlace(o.expr, replace);
        return;
    }
    if (auto* call = dynamic_cast<CallProcedure*>(&stmt)) {
        for (auto& a : call->args) substituteInPlace(a, replace);
        return;
    }
    // every other statement kind: nothing a NAYA/PURANA/parameter substitution could mean
}

}  // namespace meradb
