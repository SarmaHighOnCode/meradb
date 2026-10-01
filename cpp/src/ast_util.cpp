// cpp/src/ast_util.cpp
#include "meradb/ast_util.h"
#include "meradb/errors.h"
#include "meradb/stack_guard.h"

namespace meradb {

using namespace ast;

std::unique_ptr<Expr> cloneExpr(const Expr& e) {
    requireStack();
    if (const auto* lit = dynamic_cast<const Literal*>(&e)) {
        return std::make_unique<Literal>(lit->value);
    }
    if (const auto* col = dynamic_cast<const ColumnRef*>(&e)) {
        auto out = std::make_unique<ColumnRef>(col->name, col->table);
        out->bound = col->bound;
        return out;
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
    requireStack();
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

std::string astClassName(const ast::Statement& stmt) {
#define MERADB_CLASS_NAME(T) if (dynamic_cast<const T*>(&stmt)) return #T;
    MERADB_CLASS_NAME(CreateDatabase) MERADB_CLASS_NAME(DropDatabase) MERADB_CLASS_NAME(UseDatabase)
    MERADB_CLASS_NAME(ShowTables) MERADB_CLASS_NAME(Describe) MERADB_CLASS_NAME(CreateView)
    MERADB_CLASS_NAME(DropView) MERADB_CLASS_NAME(ShowViews) MERADB_CLASS_NAME(CreateTable)
    MERADB_CLASS_NAME(AlterAddComposite) MERADB_CLASS_NAME(DropTable) MERADB_CLASS_NAME(AlterAddColumn)
    MERADB_CLASS_NAME(AlterDropColumn) MERADB_CLASS_NAME(RenameTable) MERADB_CLASS_NAME(RenameColumn)
    MERADB_CLASS_NAME(TruncateTable) MERADB_CLASS_NAME(CompactTable) MERADB_CLASS_NAME(Begin)
    MERADB_CLASS_NAME(Commit) MERADB_CLASS_NAME(Rollback) MERADB_CLASS_NAME(Explain)
    MERADB_CLASS_NAME(Insert) MERADB_CLASS_NAME(Select) MERADB_CLASS_NAME(SetOp)
    MERADB_CLASS_NAME(Update) MERADB_CLASS_NAME(Delete) MERADB_CLASS_NAME(CreateUser)
    MERADB_CLASS_NAME(DropUser) MERADB_CLASS_NAME(Grant) MERADB_CLASS_NAME(Revoke)
    MERADB_CLASS_NAME(CreateTrigger) MERADB_CLASS_NAME(DropTrigger) MERADB_CLASS_NAME(CreateProcedure)
    MERADB_CLASS_NAME(DropProcedure) MERADB_CLASS_NAME(CallProcedure)
#undef MERADB_CLASS_NAME
    return "Statement";
}

}  // namespace meradb
