// cpp/src/planner.cpp -- mirrors meradb/planner.py
#include "meradb/planner.h"
#include "meradb/ast_util.h"
#include "meradb/errors.h"
#include "meradb/evaluator.h"

namespace meradb {
using namespace ast;

namespace {

// "alias.col" -> ColumnRef(col, alias)
std::unique_ptr<ColumnRef> boundRef(const std::string& key) {
    auto dot = key.find('.');
    return std::make_unique<ColumnRef>(key.substr(dot + 1), key.substr(0, dot));
}

}  // namespace

// ============================================================================
// 1. Binding
// ============================================================================

Scope::Scope(std::vector<std::pair<std::string, TableSchema>> sources) : sources_(std::move(sources)) {
    for (size_t i = 0; i < sources_.size(); ++i) {
        const auto& [alias, schema] = sources_[i];
        if (aliases_.count(alias))
            throw ExecutionError("'" + alias +
                                 "' query mein do baar hai -- alag alias do (jaise: SE students s MILAO students t ...)");
        aliases_[alias] = i;
        for (const auto& c : schema.columns) byColumn_[c.name].push_back(alias + "." + c.name);
    }
}

std::vector<std::string> Scope::keys(size_t i) const {
    std::vector<std::string> ks;
    const auto& [alias, schema] = sources_.at(i);
    for (const auto& c : schema.columns) ks.push_back(alias + "." + c.name);
    return ks;
}

std::unordered_map<std::string, Value> Scope::row(size_t i, const std::vector<Value>& values) const {
    std::unordered_map<std::string, Value> r;
    auto ks = keys(i);
    for (size_t j = 0; j < ks.size() && j < values.size(); ++j) r[ks[j]] = values[j];  // zip()
    return r;
}

std::unordered_map<std::string, Value> Scope::nullRow(size_t i) const {
    std::unordered_map<std::string, Value> r;
    for (auto& k : keys(i)) r[k] = Value();
    return r;
}

std::vector<std::string> Scope::allKeys() const {
    std::vector<std::string> all;
    for (size_t i = 0; i < sources_.size(); ++i)
        for (auto& k : keys(i)) all.push_back(k);
    return all;
}

size_t Scope::sourceIndex(const std::string& key) const {
    std::string alias = key.substr(0, key.find('.'));
    auto it = aliases_.find(alias);
    if (it == aliases_.end())
        throw ExecutionError("'" + alias + "' is query mein koi table ya alias nahi hai");
    return it->second;
}

std::string Scope::display(const std::string& key) const {
    auto dot = key.find('.');
    return (sources_.size() == 1 && dot != std::string::npos) ? key.substr(dot + 1) : key;
}

std::string Scope::resolve(const ColumnRef& ref) const {
    if (ref.table.has_value()) {
        auto it = aliases_.find(*ref.table);
        if (it == aliases_.end())
            throw ExecutionError("'" + *ref.table + "' is query mein koi table ya alias nahi hai");
        sources_[it->second].second.indexOf(ref.name);  // throws if the column doesn't exist
        return *ref.table + "." + ref.name;
    }

    auto it = byColumn_.find(ref.name);
    if (it == byColumn_.end() || it->second.empty()) {
        if (sources_.size() == 1) sources_[0].second.indexOf(ref.name);  // the usual "column nahi hai" error
        throw ExecutionError("Column '" + ref.name + "' kisi bhi table mein nahi hai");
    }
    const auto& keys = it->second;
    if (keys.size() > 1) {
        std::string joined;
        for (size_t i = 0; i < keys.size(); ++i) joined += (i ? " ya " : "") + keys[i];
        throw ExecutionError("Column '" + ref.name + "' ek se zyada tables mein hai -- " + joined + " likho");
    }
    return keys[0];
}

std::vector<std::pair<std::string, ColumnRef>> Scope::expandStar(const Star& star) const {
    std::vector<std::pair<std::string, ColumnRef>> out;
    for (const auto& [alias, schema] : sources_) {
        if (star.table.has_value() && alias != *star.table) continue;
        for (const auto& c : schema.columns) {
            std::string label = byColumn_.at(c.name).size() == 1 ? c.name : alias + "." + c.name;
            out.emplace_back(label, ColumnRef(c.name, alias));
        }
    }
    if (star.table.has_value() && out.empty())
        throw ExecutionError("'" + *star.table + "' is query mein koi table ya alias nahi hai");
    return out;
}

std::unique_ptr<Expr> bind(const Expr* expr, const Scope& scope) {
    return expr == nullptr ? nullptr : bind(*expr, scope);
}

std::unique_ptr<Expr> bind(const Expr& expr, const Scope& scope) {
    if (dynamic_cast<const Literal*>(&expr) || dynamic_cast<const Star*>(&expr)) return cloneExpr(expr);
    if (auto* c = dynamic_cast<const ColumnRef*>(&expr)) return boundRef(scope.resolve(*c));
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr))
        return std::make_unique<BinaryOp>(b->op, bind(*b->left, scope), bind(*b->right, scope));
    if (auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
        auto out = std::make_unique<UnaryOp>();
        out->op = u->op;
        out->operand = bind(*u->operand, scope);
        return out;
    }
    if (auto* isn = dynamic_cast<const IsNull*>(&expr)) {
        auto out = std::make_unique<IsNull>();
        out->expr = bind(*isn->expr, scope);
        out->negated = isn->negated;
        return out;
    }
    if (auto* f = dynamic_cast<const FuncCall*>(&expr)) {
        auto out = std::make_unique<FuncCall>();
        out->name = f->name;
        out->arg = bind(f->arg.get(), scope);
        return out;
    }
    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) {
        auto out = std::make_unique<Coalesce>();
        for (auto& a : co->args) out->args.push_back(bind(*a, scope));
        return out;
    }
    if (auto* cw = dynamic_cast<const CaseWhen*>(&expr)) {
        auto out = std::make_unique<CaseWhen>();
        for (auto& [cond, value] : cw->branches) out->branches.emplace_back(bind(*cond, scope), bind(*value, scope));
        out->elseExpr = bind(cw->elseExpr.get(), scope);
        return out;
    }
    if (dynamic_cast<const Subquery*>(&expr)) {
        // The subquery's OWN columns are bound separately, against its OWN
        // scope, when the engine plans it -- not here.
        return cloneExpr(expr);
    }
    if (auto* in = dynamic_cast<const InSubquery*>(&expr)) {
        auto out = std::make_unique<InSubquery>();
        out->left = bind(*in->left, scope);
        if (in->subquery) {
            auto sub = std::make_unique<Subquery>();
            if (in->subquery->statement) sub->statement = cloneSelect(*in->subquery->statement);
            out->subquery = std::move(sub);
        }
        out->negated = in->negated;
        return out;
    }
    throw ExecutionError("Unknown expression");
}

std::unique_ptr<Expr> naturalJoinCondition(const Scope& scope, size_t rightIndex) {
    const auto& sources = scope.sources();
    const auto& [rightAlias, rightSchema] = sources.at(rightIndex);
    // column name -> "alias.column" of the FIRST earlier source that has it
    std::unordered_map<std::string, std::string> firstMatch;
    for (size_t j = 0; j < rightIndex; ++j) {
        const auto& [alias, schema] = sources[j];
        for (const auto& c : schema.columns) firstMatch.emplace(c.name, alias + "." + c.name);
    }

    std::unique_ptr<Expr> result;
    for (const auto& c : rightSchema.columns) {
        auto it = firstMatch.find(c.name);
        if (it == firstMatch.end()) continue;
        auto cond = std::make_unique<BinaryOp>("=", boundRef(it->second), boundRef(rightAlias + "." + c.name));
        result = result ? std::make_unique<BinaryOp>("AUR", std::move(result), std::move(cond)) : std::move(cond);
    }
    if (!result) {
        std::string other = rightIndex > 0 ? sources[0].first : "";
        throw ExecutionError("'" + rightAlias + "' ka SAMAAN MILAO fail hua -- '" + other +
                             "' se koi column naam match nahi karta");
    }
    return result;
}

std::vector<const Expr*> conjuncts(const Expr* expr) {
    if (expr == nullptr) return {};
    return conjuncts(*expr);
}

std::vector<const Expr*> conjuncts(const Expr& expr) {
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr); b && b->op == "AUR") {
        auto left = conjuncts(*b->left);
        auto right = conjuncts(*b->right);
        left.insert(left.end(), right.begin(), right.end());
        return left;
    }
    return {&expr};
}

// ============================================================================
// 2. Access path: index lookup or full scan?
// ============================================================================

std::string IndexLookup::describe(const Table& table) const {
    const auto& col = table.schema().columns.at(column);
    std::string kind = col.primaryKey ? "MUKHYA KUNJI" : "ANOKHA";
    return "INDEX LOOKUP " + table.schema().name + " PAR " + columnName + " = " + exprLabel(Literal(value)) +
           "  [hash index, " + kind + "]";
}

std::optional<IndexLookup> chooseAccess(const Table& table, const Scope& scope, const Expr* where) {
    if (table.isView()) return std::nullopt;  // a VIEW has no real index -- always a full scan
    const auto& [alias, schema] = scope.sources().at(0);
    for (const Expr* cond : conjuncts(where)) {
        auto* b = dynamic_cast<const BinaryOp*>(cond);
        if (!b || b->op != "=") continue;
        const std::pair<const Expr*, const Expr*> sides[] = {{b->left.get(), b->right.get()},
                                                             {b->right.get(), b->left.get()}};
        for (const auto& [colSide, valSide] : sides) {
            auto* ref = dynamic_cast<const ColumnRef*>(colSide);
            auto* lit = dynamic_cast<const Literal*>(valSide);
            if (!ref || !lit) continue;
            // only a BOUND reference to the first source qualifies
            if (!ref->table || *ref->table != alias || lit->value.isNull()) continue;
            size_t position = schema.indexOf(ref->name);
            const Column& column = schema.columns[position];
            if (!column.isUnique()) continue;
            Value value;
            try {
                value = coerce(lit->value, column.typeName, ref->name);
            } catch (const ExecutionError&) {
                continue;  // type mismatch: let the normal scan report the error
            }
            return IndexLookup{position, ref->name, value};
        }
    }
    return std::nullopt;
}

// ============================================================================
// 3. Join strategy: hash join or nested loop?
// ============================================================================

void checkJoinCondition(const Expr* on, const Scope& scope, size_t rightIndex) {
    for (const auto& key : columnRefKeys(on))
        if (scope.sourceIndex(key) > rightIndex)
            throw ExecutionError("PAR mein '" + key + "' abhi use nahi ho sakta -- wo table baad mein MILAO hoti hai");
}

std::optional<std::pair<std::string, std::string>> chooseJoin(const Expr* on, const Scope& scope, size_t rightIndex) {
    for (const Expr* cond : conjuncts(on)) {
        auto* b = dynamic_cast<const BinaryOp*>(cond);
        if (!b || b->op != "=") continue;
        auto* l = dynamic_cast<const ColumnRef*>(b->left.get());
        auto* r = dynamic_cast<const ColumnRef*>(b->right.get());
        if (!l || !r) continue;
        std::string lKey = refKey(*l), rKey = refKey(*r);
        size_t a = scope.sourceIndex(lKey), bi = scope.sourceIndex(rKey);
        if (a < rightIndex && bi == rightIndex) return std::make_pair(lKey, rKey);
        if (bi < rightIndex && a == rightIndex) return std::make_pair(rKey, lKey);
    }
    return std::nullopt;
}

}  // namespace meradb
