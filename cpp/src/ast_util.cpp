// cpp/src/ast_util.cpp
#include "meradb/ast_util.h"
#include "meradb/errors.h"

namespace meradb {

using namespace ast;

std::unique_ptr<Expr> cloneExpr(const Expr& e) {
    if (const auto* lit = dynamic_cast<const Literal*>(&e)) {
        return std::make_unique<Literal>(lit->value);
    }
    if (const auto* col = dynamic_cast<const ColumnRef*>(&e)) {
        return std::make_unique<ColumnRef>(col->name, col->table);
    }
    if (const auto* star = dynamic_cast<const Star*>(&e)) {
        auto s = std::make_unique<Star>();
        s->table = star->table;
        return s;
    }
    if (const auto* bin = dynamic_cast<const BinaryOp*>(&e)) {
        return std::make_unique<BinaryOp>(bin->op, cloneExpr(*bin->left), cloneExpr(*bin->right));
    }
    if (const auto* un = dynamic_cast<const UnaryOp*>(&e)) {
        auto u = std::make_unique<UnaryOp>();
        u->op = un->op;
        u->operand = cloneExpr(*un->operand);
        return u;
    }
    if (const auto* isn = dynamic_cast<const IsNull*>(&e)) {
        auto i = std::make_unique<IsNull>();
        i->expr = cloneExpr(*isn->expr);
        i->negated = isn->negated;
        return i;
    }
    if (const auto* fc = dynamic_cast<const FuncCall*>(&e)) {
        auto f = std::make_unique<FuncCall>();
        f->name = fc->name;
        f->arg = cloneExpr(*fc->arg);
        return f;
    }
    if (const auto* sub = dynamic_cast<const Subquery*>(&e)) {
        auto s = std::make_unique<Subquery>();
        s->statement = cloneSelect(*sub->statement);
        return s;
    }
    if (const auto* insub = dynamic_cast<const InSubquery*>(&e)) {
        auto i = std::make_unique<InSubquery>();
        i->left = cloneExpr(*insub->left);
        auto subCopy = std::make_unique<Subquery>();
        subCopy->statement = cloneSelect(*insub->subquery->statement);
        i->subquery = std::move(subCopy);
        i->negated = insub->negated;
        return i;
    }
    if (const auto* co = dynamic_cast<const Coalesce*>(&e)) {
        auto c = std::make_unique<Coalesce>();
        for (const auto& arg : co->args) c->args.push_back(cloneExpr(*arg));
        return c;
    }
    if (const auto* cw = dynamic_cast<const CaseWhen*>(&e)) {
        auto c = std::make_unique<CaseWhen>();
        for (const auto& branch : cw->branches) {
            c->branches.emplace_back(cloneExpr(*branch.first), cloneExpr(*branch.second));
        }
        if (cw->elseExpr) c->elseExpr = cloneExpr(*cw->elseExpr);
        return c;
    }
    throw MeraDBError("cloneExpr: unhandled Expr node kind");
}

std::unique_ptr<Select> cloneSelect(const Select& s) {
    auto out = std::make_unique<Select>();
    for (const auto& c : s.columns) out->columns.push_back(cloneExpr(*c));
    out->table = s.table;
    out->alias = s.alias;
    for (const auto& j : s.joins) {
        Join jc;
        jc.table = j.table;
        jc.alias = j.alias;
        jc.on = j.on ? cloneExpr(*j.on) : nullptr;
        jc.kind = j.kind;
        out->joins.push_back(std::move(jc));
    }
    out->where = s.where ? cloneExpr(*s.where) : nullptr;
    for (const auto& g : s.groupBy) out->groupBy.push_back(cloneExpr(*g));
    out->having = s.having ? cloneExpr(*s.having) : nullptr;
    for (const auto& o : s.orderBy) {
        OrderItem oi;
        oi.expr = cloneExpr(*o.expr);
        oi.descending = o.descending;
        out->orderBy.push_back(std::move(oi));
    }
    out->limit = s.limit;
    out->distinct = s.distinct;
    out->aliases = s.aliases;
    return out;
}

}  // namespace meradb
