// cpp/src/substitute.cpp
#include "meradb/substitute.h"
#include "meradb/errors.h"
#include "meradb/stack_guard.h"

namespace meradb {

using namespace ast;

void substituteInPlace(std::unique_ptr<Expr>& expr, const RefReplacer& replace, bool intoSubqueries) {
    if (!expr) return;
    requireStack();
    Expr* node = expr.get();
    if (dynamic_cast<Literal*>(node) || dynamic_cast<Star*>(node)) return;
    if (auto* ref = dynamic_cast<ColumnRef*>(node)) {
        if (auto value = replace(*ref)) expr = std::make_unique<Literal>(std::move(*value));  // `ref` dies here
        return;
    }
    if (auto* bin = dynamic_cast<BinaryOp*>(node)) {
        substituteInPlace(bin->left, replace, intoSubqueries);
        substituteInPlace(bin->right, replace, intoSubqueries);
        return;
    }
    if (auto* un = dynamic_cast<UnaryOp*>(node)) {
        substituteInPlace(un->operand, replace, intoSubqueries);
        return;
    }
    if (auto* isNull = dynamic_cast<IsNull*>(node)) {
        substituteInPlace(isNull->expr, replace, intoSubqueries);
        return;
    }
    if (auto* fn = dynamic_cast<FuncCall*>(node)) {
        substituteInPlace(fn->arg, replace, intoSubqueries);
        return;
    }
    if (auto* sf = dynamic_cast<ScalarFunc*>(node)) {
        for (auto& arg : sf->args) substituteInPlace(arg, replace, intoSubqueries);
        return;
    }
    if (auto* co = dynamic_cast<Coalesce*>(node)) {
        for (auto& arg : co->args) substituteInPlace(arg, replace, intoSubqueries);
        return;
    }
    if (auto* cw = dynamic_cast<CaseWhen*>(node)) {
        for (auto& branch : cw->branches) {
            substituteInPlace(branch.first, replace, intoSubqueries);
            substituteInPlace(branch.second, replace, intoSubqueries);
        }
        substituteInPlace(cw->elseExpr, replace, intoSubqueries);
        return;
    }
    if (auto* sub = dynamic_cast<Subquery*>(node)) {
        if (intoSubqueries && sub->statement) substituteStatementInPlace(*sub->statement, replace, true);
        return;  // otherwise out of scope, like Python
    }
    if (auto* in = dynamic_cast<InSubquery*>(node)) {
        if (intoSubqueries) {
            substituteInPlace(in->left, replace, true);
            if (in->subquery && in->subquery->statement) substituteStatementInPlace(*in->subquery->statement, replace, true);
        }
        return;
    }
    throw ExecutionError("Unknown expression");
}

void substituteStatementInPlace(Statement& stmt, const RefReplacer& replace, bool intoSubqueries) {
    if (auto* ins = dynamic_cast<Insert*>(&stmt)) {
        for (auto& row : ins->rows)
            for (auto& e : row) substituteInPlace(e, replace, intoSubqueries);
        if (ins->select) substituteStatementInPlace(*ins->select, replace, intoSubqueries);
        if (ins->onConflictUpdate)
            for (auto& assignment : *ins->onConflictUpdate) substituteInPlace(assignment.second, replace, intoSubqueries);
        return;
    }
    if (auto* upd = dynamic_cast<Update*>(&stmt)) {
        for (auto& assignment : upd->assignments) substituteInPlace(assignment.second, replace, intoSubqueries);
        substituteInPlace(upd->where, replace, intoSubqueries);
        return;
    }
    if (auto* del = dynamic_cast<Delete*>(&stmt)) {
        substituteInPlace(del->where, replace, intoSubqueries);
        return;
    }
    if (auto* sel = dynamic_cast<Select*>(&stmt)) {
        for (auto& c : sel->columns) substituteInPlace(c, replace, intoSubqueries);
        if (sel->derived) substituteStatementInPlace(*sel->derived, replace, intoSubqueries);
        for (auto& j : sel->joins) {
            substituteInPlace(j.on, replace, intoSubqueries);
            if (j.derived) substituteStatementInPlace(*j.derived, replace, intoSubqueries);
        }
        substituteInPlace(sel->where, replace, intoSubqueries);
        for (auto& g : sel->groupBy) substituteInPlace(g, replace, intoSubqueries);
        substituteInPlace(sel->having, replace, intoSubqueries);
        for (auto& o : sel->orderBy) substituteInPlace(o.expr, replace, intoSubqueries);
        return;
    }
    if (auto* call = dynamic_cast<CallProcedure*>(&stmt)) {
        for (auto& a : call->args) substituteInPlace(a, replace, intoSubqueries);
        return;
    }
    if (intoSubqueries) {  // the procedural bodies: the statement kinds that did not exist when this was written
        if (auto* setop = dynamic_cast<SetOp*>(&stmt)) {
            if (setop->left) substituteStatementInPlace(*setop->left, replace, true);
            if (setop->right) substituteStatementInPlace(*setop->right, replace, true);
            return;
        }
        if (auto* ex = dynamic_cast<Explain*>(&stmt)) {
            if (ex->statement) substituteStatementInPlace(*ex->statement, replace, true);
            return;
        }
        if (auto* prep = dynamic_cast<Prepare*>(&stmt)) {
            substituteInPlace(prep->source, replace, true);
            return;
        }
        if (auto* exec = dynamic_cast<ExecutePrepared*>(&stmt)) {
            for (auto& a : exec->args) substituteInPlace(a, replace, true);
            return;
        }
        if (auto* set = dynamic_cast<SetVariable*>(&stmt)) {
            for (auto& a : set->assignments) substituteInPlace(a.second, replace, true);
            return;
        }
        if (auto* ct = dynamic_cast<CreateTable*>(&stmt)) {
            if (ct->asQuery) substituteStatementInPlace(*ct->asQuery, replace, true);
            return;
        }
    }
    // every other statement kind: nothing a NAYA/PURANA/parameter substitution could mean
}

}  // namespace meradb
