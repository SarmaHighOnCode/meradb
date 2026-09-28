// cpp/src/evaluator.cpp -- mirrors meradb/evaluator.py
#include "meradb/evaluator.h"
#include "meradb/errors.h"
#include "meradb/pyvalue.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>

namespace meradb {
using namespace ast;

namespace {

constexpr int64_t I64_MAX = std::numeric_limits<int64_t>::max();
constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();

// ---------------------------------------------------------------- kinds ----

enum class Kind { Bool, Number, Date, Text };

// Python's _kind(): only called on non-null values.
Kind kindOf(const Value& v) {
    if (std::holds_alternative<bool>(v.data)) return Kind::Bool;
    if (std::holds_alternative<int64_t>(v.data) || std::holds_alternative<double>(v.data)) return Kind::Number;
    if (std::holds_alternative<Date>(v.data)) return Kind::Date;
    return Kind::Text;
}

bool isInt(const Value& v) { return isIntValue(v); }
bool isDouble(const Value& v) { return isDoubleValue(v); }
bool isText(const Value& v) { return isTextValue(v); }

double toDouble(const Value& v) {
    return isInt(v) ? static_cast<double>(std::get<int64_t>(v.data)) : std::get<double>(v.data);
}

// `{format_value(v)!r}` in Python error messages
std::string shown(const Value& v) { return pyRepr(formatValue(v)); }

[[noreturn]] void intOverflow(const std::string& op) {
    // Python ints are unbounded; ours are 8 bytes (the on-disk INT size).
    throw ExecutionError("'" + op + "' ka result INT ke liye bahut bada hai (8-byte limit)");
}

// ----------------------------------------------------------- comparison ----

bool applyOrdering(const std::string& op, int c) {
    if (op == "=") return c == 0;
    if (op == "!=") return c != 0;
    if (op == "<") return c < 0;
    if (op == "<=") return c <= 0;
    if (op == ">") return c > 0;
    return c >= 0;
}

bool applyDouble(const std::string& op, double x, double y) {
    // direct IEEE operators so NaN behaves like Python (only != is true)
    if (op == "=") return x == y;
    if (op == "!=") return x != y;
    if (op == "<") return x < y;
    if (op == "<=") return x <= y;
    if (op == ">") return x > y;
    return x >= y;
}

bool compareNumbers(const std::string& op, const Value& a, const Value& b) {
    if (isInt(a) && isInt(b)) {
        int64_t x = std::get<int64_t>(a.data), y = std::get<int64_t>(b.data);
        return applyOrdering(op, x < y ? -1 : (x > y ? 1 : 0));
    }
    if (isDouble(a) && isDouble(b)) return applyDouble(op, std::get<double>(a.data), std::get<double>(b.data));
    // mixed int / float
    double d = isDouble(a) ? std::get<double>(a.data) : std::get<double>(b.data);
    if (std::isnan(d)) return op == "!=";
    int c = isInt(a) ? cmpIntDouble(std::get<int64_t>(a.data), d) : -cmpIntDouble(std::get<int64_t>(b.data), d);
    return applyOrdering(op, c);
}

Value compare(const std::string& op, Value left, Value right) {
    // `JAHAN dob > '2005-01-01'` -- a string compared to a DATE is parsed first.
    if (std::holds_alternative<Date>(left.data) && isText(right))
        right = Value(parseDate(std::get<std::string>(right.data)));
    else if (std::holds_alternative<Date>(right.data) && isText(left))
        left = Value(parseDate(std::get<std::string>(left.data)));

    Kind kind = kindOf(left);
    if (kind != kindOf(right))
        throw ExecutionError(shown(left) + " aur " + shown(right) + " ko compare nahi kar sakte (alag types)");
    switch (kind) {
        case Kind::Number:
            return Value(compareNumbers(op, left, right));
        case Kind::Bool: {
            int x = std::get<bool>(left.data), y = std::get<bool>(right.data);
            return Value(applyOrdering(op, x - y));
        }
        case Kind::Date: {
            int32_t x = std::get<Date>(left.data).ordinal, y = std::get<Date>(right.data).ordinal;
            return Value(applyOrdering(op, x < y ? -1 : (x > y ? 1 : 0)));
        }
        case Kind::Text: {
            // byte order of UTF-8 == code point order, like Python str
            int c = std::get<std::string>(left.data).compare(std::get<std::string>(right.data));
            return Value(applyOrdering(op, c));
        }
    }
    return Value();
}

// ---------------------------------------------------------------- JAISA ----

// Decode UTF-8 into code points so `_` matches exactly one CHARACTER, like
// Python's str. A malformed byte becomes its own (never-matching-a-letter)
// code point instead of throwing.
std::u32string decodeUtf8(const std::string& s) {
    std::u32string out;
    size_t i = 0;
    while (i < s.size()) {
        auto b = static_cast<unsigned char>(s[i]);
        int extra = b < 0x80 ? 0 : (b >> 5) == 0x6 ? 1 : (b >> 4) == 0xE ? 2 : (b >> 3) == 0x1E ? 3 : -1;
        char32_t cp = extra == 0 ? b : extra == 1 ? (b & 0x1F) : extra == 2 ? (b & 0x0F) : (b & 0x07);
        bool ok = extra >= 0 && i + static_cast<size_t>(extra) < s.size();
        if (ok && extra > 0) {
            for (int k = 1; ok && k <= extra; ++k) {
                auto c = static_cast<unsigned char>(s[i + k]);
                if ((c >> 6) != 0x2) ok = false;
                else cp = (cp << 6) | (c & 0x3F);
            }
        }
        if (!ok) {
            out.push_back(0xDC00 + b);  // lone surrogate range: never a real character
            ++i;
            continue;
        }
        out.push_back(cp);
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

// re.IGNORECASE equivalence: every code point that Python's `re` matches
// case-insensitively against another maps to the same representative (the
// smallest code point of its class). The table is generated from the
// reference Python 3.12 `re` module (all of Unicode, pairwise-verified).
struct FoldEntry {
    char32_t from, to;
};
const FoldEntry FOLD_TABLE[] = {
#include "casefold_table.inc"
};

char32_t foldCase(char32_t c) {
    if (c < 0x41) return c;  // fast path: nothing below 'A' folds
    auto it = std::lower_bound(std::begin(FOLD_TABLE), std::end(FOLD_TABLE), c,
                               [](const FoldEntry& e, char32_t x) { return e.from < x; });
    return (it != std::end(FOLD_TABLE) && it->from == c) ? it->to : c;
}

// naam JAISA 'R%' -- % = any characters (even none), _ = exactly one
// character; case-insensitive, whole-string match.
bool likeMatch(const std::string& textUtf8, const std::string& patternUtf8) {
    std::u32string text = decodeUtf8(textUtf8), pattern = decodeUtf8(patternUtf8);
    for (auto& c : text) c = foldCase(c);
    for (auto& c : pattern) c = foldCase(c);
    size_t t = 0, p = 0, starP = std::u32string::npos, starT = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == U'%') {
            starP = p++;
            starT = t;
        } else if (p < pattern.size() && (pattern[p] == U'_' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (starP != std::u32string::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == U'%') ++p;
    return p == pattern.size();
}

Value like(const Value& text, const Value& pattern) {
    if (!isText(text) || !isText(pattern)) throw ExecutionError("JAISA sirf TEXT ke saath chalta hai");
    return Value(likeMatch(std::get<std::string>(text.data), std::get<std::string>(pattern.data)));
}

// ----------------------------------------------------------- arithmetic ----

void requireNumber(const Value& v, const std::string& op) {
    if (kindOf(v) != Kind::Number)
        throw ExecutionError("'" + op + "' sirf numbers ke saath chalta hai, " + shown(v) + " mila");
}

void requireBool(const Value& v, const std::string& op) {
    if (!std::holds_alternative<bool>(v.data))
        throw ExecutionError("'" + op + "' ko SACH/JHOOTH condition chahiye, " + shown(v) + " mila");
}

void requireBoolOrNull(const Value& v, const std::string& op) {
    if (!v.isNull()) requireBool(v, op);
}

int64_t checkedAdd(int64_t x, int64_t y, const std::string& op) {
    if ((y > 0 && x > I64_MAX - y) || (y < 0 && x < I64_MIN - y)) intOverflow(op);
    return x + y;
}

int64_t checkedSub(int64_t x, int64_t y, const std::string& op) {
    if ((y < 0 && x > I64_MAX + y) || (y > 0 && x < I64_MIN + y)) intOverflow(op);
    return x - y;
}

int64_t checkedMul(int64_t x, int64_t y, const std::string& op) {
    if (x > 0) {
        if (y > 0 ? x > I64_MAX / y : y < I64_MIN / x) intOverflow(op);
    } else if (x < 0) {
        if (y > 0 ? x < I64_MIN / y : (y != 0 && x < I64_MAX / y)) intOverflow(op);
    }
    return x * y;
}

// Python float %: the result takes the sign of the divisor.
double pyFloatMod(double x, double y) {
    double mod = std::fmod(x, y);
    if (mod != 0) {
        if ((y < 0) != (mod < 0)) mod += y;
    } else {
        mod = std::copysign(0.0, y);
    }
    return mod;
}

Value arithmetic(const std::string& op, const Value& left, const Value& right) {
    if (op == "+" && isText(left) && isText(right))
        return Value(std::get<std::string>(left.data) + std::get<std::string>(right.data));  // 'Ra' + 'vi'
    requireNumber(left, op);
    requireNumber(right, op);
    bool bothInt = isInt(left) && isInt(right);
    if (op == "+" || op == "-" || op == "*") {
        if (bothInt) {
            int64_t x = std::get<int64_t>(left.data), y = std::get<int64_t>(right.data);
            if (op == "+") return Value(checkedAdd(x, y, op));
            if (op == "-") return Value(checkedSub(x, y, op));
            return Value(checkedMul(x, y, op));
        }
        double x = toDouble(left), y = toDouble(right);
        if (op == "+") return Value(x + y);
        if (op == "-") return Value(x - y);
        return Value(x * y);
    }
    if ((op == "/" || op == "%") && toDouble(right) == 0) throw ExecutionError("Zero se divide nahi kar sakte");
    if (op == "/") {
        if (bothInt) {
            // Python: int(left / right) -- true division, then truncate, like SQL
            double q = std::trunc(pyTrueDivide(std::get<int64_t>(left.data), std::get<int64_t>(right.data)));
            if (q >= TWO_POW_63 || q < -TWO_POW_63) intOverflow(op);
            return Value(static_cast<int64_t>(q));
        }
        return Value(toDouble(left) / toDouble(right));
    }
    if (op == "%") {
        if (bothInt) {
            int64_t x = std::get<int64_t>(left.data), y = std::get<int64_t>(right.data);
            if (y == -1) return Value(int64_t{0});  // avoids INT64_MIN % -1 overflow
            int64_t r = x % y;
            if (r != 0 && ((r < 0) != (y < 0))) r += y;  // floor modulo, like Python
            return Value(r);
        }
        return Value(pyFloatMod(toDouble(left), toDouble(right)));
    }
    throw ExecutionError("Unknown operator " + op);
}

// ------------------------------------------------------------ AUR / YA ----

bool isBool(const Value& v, bool which) { return std::holds_alternative<bool>(v.data) && std::get<bool>(v.data) == which; }

Value evalAnd(const BinaryOp& expr, const Row& row, const SubqueryResults* subqueries) {
    Value left = evaluate(*expr.left, row, subqueries);
    requireBoolOrNull(left, "AUR");
    if (isBool(left, false)) return Value(false);  // short-circuit
    Value right = evaluate(*expr.right, row, subqueries);
    requireBoolOrNull(right, "AUR");
    if (isBool(right, false)) return Value(false);
    if (left.isNull() || right.isNull()) return Value();
    return Value(true);
}

Value evalOr(const BinaryOp& expr, const Row& row, const SubqueryResults* subqueries) {
    Value left = evaluate(*expr.left, row, subqueries);
    requireBoolOrNull(left, "YA");
    if (isBool(left, true)) return Value(true);
    Value right = evaluate(*expr.right, row, subqueries);
    requireBoolOrNull(right, "YA");
    if (isBool(right, true)) return Value(true);
    if (left.isNull() || right.isNull()) return Value();
    return Value(false);
}

// ---------------------------------------------------------------- walks ----

void columnRefsImpl(const Expr* expr, std::vector<const ColumnRef*>& out, bool skipAggregates) {
    if (expr == nullptr) return;
    if (auto* ref = dynamic_cast<const ColumnRef*>(expr)) {
        out.push_back(ref);
    } else if (auto* b = dynamic_cast<const BinaryOp*>(expr)) {
        columnRefsImpl(b->left.get(), out, skipAggregates);
        columnRefsImpl(b->right.get(), out, skipAggregates);
    } else if (auto* u = dynamic_cast<const UnaryOp*>(expr)) {
        columnRefsImpl(u->operand.get(), out, skipAggregates);
    } else if (auto* isn = dynamic_cast<const IsNull*>(expr)) {
        columnRefsImpl(isn->expr.get(), out, skipAggregates);
    } else if (auto* f = dynamic_cast<const FuncCall*>(expr)) {
        if (!skipAggregates) columnRefsImpl(f->arg.get(), out, skipAggregates);
    } else if (auto* co = dynamic_cast<const Coalesce*>(expr)) {
        for (auto& a : co->args) columnRefsImpl(a.get(), out, skipAggregates);
    } else if (auto* cw = dynamic_cast<const CaseWhen*>(expr)) {
        for (auto& [cond, value] : cw->branches) {
            columnRefsImpl(cond.get(), out, skipAggregates);
            columnRefsImpl(value.get(), out, skipAggregates);
        }
        columnRefsImpl(cw->elseExpr.get(), out, skipAggregates);
    }
    // no case for Subquery/InSubquery on purpose: a subquery's own columns
    // are bound and validated separately when it is planned.
}

void columnRefNodesImpl(const Expr* expr, std::vector<const ColumnRef*>& out) {
    if (expr == nullptr) return;
    if (auto* ref = dynamic_cast<const ColumnRef*>(expr)) {
        out.push_back(ref);
    } else if (auto* b = dynamic_cast<const BinaryOp*>(expr)) {
        columnRefNodesImpl(b->left.get(), out);
        columnRefNodesImpl(b->right.get(), out);
    } else if (auto* u = dynamic_cast<const UnaryOp*>(expr)) {
        columnRefNodesImpl(u->operand.get(), out);
    } else if (auto* isn = dynamic_cast<const IsNull*>(expr)) {
        columnRefNodesImpl(isn->expr.get(), out);
    } else if (auto* f = dynamic_cast<const FuncCall*>(expr)) {
        columnRefNodesImpl(f->arg.get(), out);
    }
}

void findAggregatesImpl(const Expr* expr, std::vector<const FuncCall*>& out) {
    if (expr == nullptr) return;
    if (auto* f = dynamic_cast<const FuncCall*>(expr)) {
        out.push_back(f);  // don't look inside: aggregates can't be nested
    } else if (auto* b = dynamic_cast<const BinaryOp*>(expr)) {
        findAggregatesImpl(b->left.get(), out);
        findAggregatesImpl(b->right.get(), out);
    } else if (auto* u = dynamic_cast<const UnaryOp*>(expr)) {
        findAggregatesImpl(u->operand.get(), out);
    } else if (auto* isn = dynamic_cast<const IsNull*>(expr)) {
        findAggregatesImpl(isn->expr.get(), out);
    } else if (auto* co = dynamic_cast<const Coalesce*>(expr)) {
        for (auto& a : co->args) findAggregatesImpl(a.get(), out);
    } else if (auto* cw = dynamic_cast<const CaseWhen*>(expr)) {
        for (auto& [cond, value] : cw->branches) {
            findAggregatesImpl(cond.get(), out);
            findAggregatesImpl(value.get(), out);
        }
        findAggregatesImpl(cw->elseExpr.get(), out);
    }
}

void findSubqueriesImpl(const Expr* expr, std::vector<const Expr*>& out) {
    if (expr == nullptr) return;
    if (dynamic_cast<const Subquery*>(expr)) {
        out.push_back(expr);
    } else if (auto* in = dynamic_cast<const InSubquery*>(expr)) {
        out.push_back(expr);
        findSubqueriesImpl(in->left.get(), out);
    } else if (auto* b = dynamic_cast<const BinaryOp*>(expr)) {
        findSubqueriesImpl(b->left.get(), out);
        findSubqueriesImpl(b->right.get(), out);
    } else if (auto* u = dynamic_cast<const UnaryOp*>(expr)) {
        findSubqueriesImpl(u->operand.get(), out);
    } else if (auto* isn = dynamic_cast<const IsNull*>(expr)) {
        findSubqueriesImpl(isn->expr.get(), out);
    } else if (auto* f = dynamic_cast<const FuncCall*>(expr)) {
        findSubqueriesImpl(f->arg.get(), out);
    } else if (auto* co = dynamic_cast<const Coalesce*>(expr)) {
        for (auto& a : co->args) findSubqueriesImpl(a.get(), out);
    } else if (auto* cw = dynamic_cast<const CaseWhen*>(expr)) {
        for (auto& [cond, value] : cw->branches) {
            findSubqueriesImpl(cond.get(), out);
            findSubqueriesImpl(value.get(), out);
        }
        findSubqueriesImpl(cw->elseExpr.get(), out);
    }
}

}  // namespace

// ============================================================================
// evaluate
// ============================================================================

bool isTrue(const Value& v) { return isBool(v, true); }

Value evaluate(const Expr& expr, const Row& row, const SubqueryResults* subqueries) {
    if (auto* lit = dynamic_cast<const Literal*>(&expr)) return lit->value;

    if (dynamic_cast<const Subquery*>(&expr) || dynamic_cast<const InSubquery*>(&expr)) {
        // The engine pre-computes every subquery's result for THIS outer row;
        // evaluate() only looks the answer up by node identity.
        const SubqueryResult* result = nullptr;
        if (subqueries) {
            auto it = subqueries->find(&expr);
            if (it != subqueries->end()) result = &it->second;
        }
        if (result == nullptr) throw ExecutionError("Subquery ka result pehle se ready nahi tha (internal error)");
        auto* in = dynamic_cast<const InSubquery*>(&expr);
        if (in == nullptr) return result->scalar;  // already reduced to a scalar by the engine
        Value left = evaluate(*in->left, row, subqueries);
        if (left.isNull()) return Value();
        bool isMember = false;
        for (const auto& v : result->values)
            if (!v.isNull() && pyEquals(left, v)) { isMember = true; break; }
        return Value(in->negated ? !isMember : isMember);
    }

    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) {
        for (auto& arg : co->args) {
            Value v = evaluate(*arg, row, subqueries);
            if (!v.isNull()) return v;
        }
        return Value();
    }

    if (auto* cw = dynamic_cast<const CaseWhen*>(&expr)) {
        for (auto& [cond, value] : cw->branches)
            if (isTrue(evaluate(*cond, row, subqueries))) return evaluate(*value, row, subqueries);
        return cw->elseExpr ? evaluate(*cw->elseExpr, row, subqueries) : Value();
    }

    if (auto* ref = dynamic_cast<const ColumnRef*>(&expr)) {
        std::string key = refKey(*ref);  // bound: "alias.col"; unbound (even `t.col`): "col"
        auto it = row.find(key);
        if (it == row.end()) throw ExecutionError("Column '" + key + "' nahi mila");
        return it->second;
    }

    if (auto* isn = dynamic_cast<const IsNull*>(&expr)) {
        bool isNull = evaluate(*isn->expr, row, subqueries).isNull();
        return Value(isn->negated ? !isNull : isNull);
    }

    if (auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
        Value v = evaluate(*u->operand, row, subqueries);
        if (v.isNull()) return Value();
        if (u->op == "-") {
            requireNumber(v, "-");
            if (isInt(v)) {
                int64_t x = std::get<int64_t>(v.data);
                if (x == I64_MIN) intOverflow("-");
                return Value(-x);
            }
            return Value(-std::get<double>(v.data));
        }
        if (u->op == "NAHI") {
            requireBool(v, "NAHI");
            return Value(!std::get<bool>(v.data));
        }
        throw ExecutionError("Unknown expression");
    }

    if (auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
        if (b->op == "AUR") return evalAnd(*b, row, subqueries);
        if (b->op == "YA") return evalOr(*b, row, subqueries);

        Value left = evaluate(*b->left, row, subqueries);
        Value right = evaluate(*b->right, row, subqueries);
        if (left.isNull() || right.isNull()) return Value();  // anything combined with KHALI is KHALI
        const std::string& op = b->op;
        if (op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
            return compare(op, std::move(left), std::move(right));
        if (op == "JAISA") return like(left, right);
        return arithmetic(op, left, right);
    }

    if (auto* f = dynamic_cast<const FuncCall*>(&expr)) {
        // Aggregates are NOT computed here: the engine computes them once per
        // group and stores the answer in the row under aggKey().
        auto it = row.find(aggKey(*f));
        if (it != row.end()) return it->second;
        throw ExecutionError(exprLabel(*f) +
                             " yahan nahi chal sakta -- aggregates sirf DIKHAO list, JINKA aur KRAM mein chalte hain");
    }

    if (dynamic_cast<const Star*>(&expr)) throw ExecutionError("'*' yahan use nahi ho sakta");

    throw ExecutionError("Unknown expression");
}

// ============================================================================
// utilities used by the engine
// ============================================================================

std::string refKey(const ColumnRef& ref) { return ref.bound && ref.table ? *ref.table + "." + ref.name : ref.name; }

void columnRefs(const Expr& expr, std::vector<const ColumnRef*>& out, bool skipAggregates) {
    columnRefsImpl(&expr, out, skipAggregates);
}

std::vector<std::string> columnRefKeys(const Expr* expr, bool skipAggregates) {
    std::vector<const ColumnRef*> refs;
    columnRefsImpl(expr, refs, skipAggregates);
    std::vector<std::string> keys;
    keys.reserve(refs.size());
    for (auto* r : refs) keys.push_back(refKey(*r));
    return keys;
}

void columnRefNodes(const Expr& expr, std::vector<const ColumnRef*>& out) { columnRefNodesImpl(&expr, out); }

void findAggregates(const Expr& expr, std::vector<const FuncCall*>& out) { findAggregatesImpl(&expr, out); }

void findSubqueries(const Expr& expr, std::vector<const Expr*>& out) { findSubqueriesImpl(&expr, out); }

std::string aggKey(const FuncCall& func) {
    // Python uses the tuple ("agg", label), which can never equal a column
    // name; the ':' plays that role here (no identifier contains one).
    return "agg:" + exprLabel(func);
}

std::string exprLabel(const Expr& expr) {
    if (auto* ref = dynamic_cast<const ColumnRef*>(&expr)) return ref->table ? *ref->table + "." + ref->name : ref->name;
    if (auto* lit = dynamic_cast<const Literal*>(&expr))
        return isText(lit->value) ? pyRepr(std::get<std::string>(lit->value.data)) : formatValue(lit->value);
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr))
        return exprLabel(*b->left) + " " + b->op + " " + exprLabel(*b->right);
    if (auto* u = dynamic_cast<const UnaryOp*>(&expr))
        return u->op == "NAHI" ? u->op + " " + exprLabel(*u->operand) : "-" + exprLabel(*u->operand);
    if (auto* isn = dynamic_cast<const IsNull*>(&expr))
        return exprLabel(*isn->expr) + " HAI " + (isn->negated ? "NAHI " : "") + "KHALI";
    if (auto* f = dynamic_cast<const FuncCall*>(&expr))
        return f->name + "(" + (f->arg ? exprLabel(*f->arg) : std::string()) + ")";
    if (auto* s = dynamic_cast<const Star*>(&expr)) return s->table ? *s->table + ".*" : "*";
    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) {
        std::string out = "PEHLA(";
        for (size_t i = 0; i < co->args.size(); ++i) out += (i ? ", " : "") + exprLabel(*co->args[i]);
        return out + ")";
    }
    if (dynamic_cast<const CaseWhen*>(&expr)) return "AGAR ... KHATAM";
    if (dynamic_cast<const Subquery*>(&expr)) return "(DIKHAO ...)";
    if (auto* in = dynamic_cast<const InSubquery*>(&expr))
        return exprLabel(*in->left) + " " + (in->negated ? "NAHI " : "") + "MEIN (DIKHAO ...)";
    return "?";
}

}  // namespace meradb
