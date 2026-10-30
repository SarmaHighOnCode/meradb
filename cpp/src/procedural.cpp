// cpp/src/procedural.cpp -- the PROCEDURAL side of the Engine:
//   * running trigger / procedure / function bodies (DECLARE, SET, IF, loops, cursors, SELECT .. INTO, SIGNAL, RETURN)
//   * stored functions, procedure parameter modes (IN / OUT / INOUT)
//   * session variables (@x, autocommit), prepared statements (PREPARE / EXECUTE / DEALLOCATE)
// A body is parsed fresh on every run (parser_proc.cpp); ordinary statements inside it keep going through
// executeStatement, so privileges, FK rules and triggers apply exactly as for a statement typed by hand.
#include "meradb/aggregates.h"
#include "meradb/ast_util.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/scalar_funcs.h"
#include "meradb/stack_guard.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace meradb {

namespace {

Value textValue(std::string s) { return Value(std::move(s)); }

// A function's arguments and result are converted the way MySQL does: '1000' becomes 1000 for an INT, 5 becomes '5'
// for a TEXT. Anything that does not convert falls through to the strict column rules (and their error).
Value looseCoerce(const Value& value, const std::string& typeName, const std::string& name) {
    if (auto* text = std::get_if<std::string>(&value.data)) {
        if (typeName == "INT" || typeName == "FLOAT") {
            const char* begin = text->c_str();
            char* end = nullptr;
            if (typeName == "INT") {
                long long n = std::strtoll(begin, &end, 10);
                if (end != begin && *end == '\0') return Value(static_cast<int64_t>(n));
            } else {
                double d = std::strtod(begin, &end);
                if (end != begin && *end == '\0') return Value(d);
            }
        }
    } else if (typeName == "TEXT" && !value.isNull() && !std::holds_alternative<std::string>(value.data)) {
        return Value(formatValue(value));
    }
    return coerce(value, typeName, name);
}

Result messageResult(std::string message) {
    Result r;
    r.message = std::move(message);
    return r;
}

std::string upperOf(std::string s) {
    for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A value as a SQL literal MeraDB reads back as the same value (for the ? marks of a prepared statement).
std::string sqlLiteral(const Value& v) {
    if (v.isNull()) return "KHALI";
    if (auto* b = std::get_if<bool>(&v.data)) return *b ? "SACH" : "JHOOTH";
    if (auto* i = std::get_if<int64_t>(&v.data)) return *i < 0 ? "(" + std::to_string(*i) + ")" : std::to_string(*i);
    if (auto* d = std::get_if<double>(&v.data)) {
        if (!std::isfinite(*d)) throw ExecutionError("Infinity / NaN ko ? ki jagah nahi bheja ja sakta");
        std::string text;
        for (int digits = 1; digits <= 340; ++digits) {  // the shortest fixed notation that reads back exactly
            char buffer[512];
            std::snprintf(buffer, sizeof buffer, "%.*f", digits, *d);
            if (std::strtod(buffer, nullptr) == *d) {
                text = buffer;
                break;
            }
        }
        return *d < 0 ? "(" + text + ")" : text;
    }
    std::string text = std::holds_alternative<Date>(v.data) ? std::get<Date>(v.data).isoFormat() : std::get<std::string>(v.data);
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out + "'";
}

// the byte offsets of every ? that is not inside a 'string' or a -- comment
std::vector<size_t> placeholderPositions(const std::string& text) {
    std::vector<size_t> positions;
    bool inString = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            if (c == '\'') {
                if (i + 1 < text.size() && text[i + 1] == '\'') ++i;
                else inString = false;
            }
            continue;
        }
        if (c == '\'') inString = true;
        else if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (c == '?') positions.push_back(i);
    }
    return positions;
}

std::string fillPlaceholders(const std::string& text, const std::vector<size_t>& positions, const std::vector<std::string>& literals) {
    std::string out;
    size_t from = 0;
    for (size_t i = 0; i < positions.size(); ++i) {
        out.append(text, from, positions[i] - from);
        out += literals[i];
        from = positions[i] + 1;
    }
    out.append(text, from, std::string::npos);
    return out;
}

// SQL LIKE for SHOW VARIABLES LIKE: % any run, _ one character, case-insensitive
bool likeText(const std::string& text, const std::string& pattern, size_t t = 0, size_t p = 0) {
    for (; p < pattern.size(); ++p) {
        if (pattern[p] == '%') {
            for (size_t k = t; k <= text.size(); ++k)
                if (likeText(text, pattern, k, p + 1)) return true;
            return false;
        }
        if (t >= text.size()) return false;
        if (pattern[p] != '_' && ::tolower(static_cast<unsigned char>(pattern[p])) != ::tolower(static_cast<unsigned char>(text[t])))
            return false;
        ++t;
    }
    return t == text.size();
}

bool truthy(const Value& v, bool& ok) {
    ok = true;
    if (auto* b = std::get_if<bool>(&v.data)) return *b;
    if (auto* i = std::get_if<int64_t>(&v.data)) return *i != 0;
    if (auto* d = std::get_if<double>(&v.data)) return *d != 0.0;
    if (auto* s = std::get_if<std::string>(&v.data)) {
        std::string u = upperOf(*s);
        if (u == "ON" || u == "TRUE" || u == "1") return true;
        if (u == "OFF" || u == "FALSE" || u == "0") return false;
    }
    ok = false;
    return false;
}

void walkBody(const ast::ProcBlock& block, const std::function<void(const ast::ProcNode&)>& visit) {
    for (const auto& node : block) {
        visit(*node);
        if (auto* branch = dynamic_cast<const ast::PIf*>(node.get())) {
            for (const auto& b : branch->branches) walkBody(b.second, visit);
            walkBody(branch->elseBlock, visit);
        } else if (auto* loop = dynamic_cast<const ast::PLoop*>(node.get())) {
            walkBody(loop->body, visit);
        }
    }
}

}  // namespace

// ============================================================================
// SessionHost: what the evaluator asks the engine for
// ============================================================================

Value Engine::userVariable(const std::string& name) {
    auto it = userVars_.find(name);
    return it == userVars_.end() ? Value() : it->second;
}

bool Engine::hasFunction(const std::string& name) { return catalog().hasFunction(name); }

Value Engine::callFunction(const std::string& name, const std::vector<Value>& args) {
    auto fn = catalog().findFunction(name);
    if (!fn) throw unknownFunctionError(name);
    if (args.size() != fn->params.size())
        throw ExecutionError("Function '" + name + "' ko " + std::to_string(fn->params.size()) + " argument(s) chahiye, " +
                             std::to_string(args.size()) + " mile");
    if (functionDepth_ >= kMaxFunctionDepth)
        throw ExecutionError("Function '" + name + "' bahut gehra chal raha hai (limit " + std::to_string(kMaxFunctionDepth) +
                             ") -- shayad recursion kabhi rukti nahi");
    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    } guard(functionDepth_);

    ProcFrame frame;
    frame.isFunction = true;
    for (size_t i = 0; i < args.size(); ++i)
        frame.vars[fn->params[i].first] = {fn->params[i].second, looseCoerce(args[i], fn->params[i].second, fn->params[i].first)};
    runBodyTree(fn->bodyText, frame);
    if (!frame.returned) throw ExecutionError("Function '" + name + "' ne RETURN nahi kiya");
    return looseCoerce(frame.returnValue, fn->returnType, name);
}

// ============================================================================
// the interpreter
// ============================================================================

RefReplacer Engine::frameReplacer(ProcFrame& f) {
    return [&f](const ast::ColumnRef& ref) -> std::optional<Value> {
        if (ref.table) {
            if (*ref.table == "naya") {
                const Row* row = f.newRow ? f.newRow : f.newView;
                if (row) {
                    auto it = row->find(ref.name);
                    if (it != row->end()) return it->second;
                }
            }
            if (*ref.table == "purana" && f.oldRow) {
                auto it = f.oldRow->find(ref.name);
                if (it != f.oldRow->end()) return it->second;
            }
            return std::nullopt;
        }
        auto it = f.vars.find(ref.name);  // a variable beats a column of the same name, as in MySQL
        if (it != f.vars.end()) return it->second.value;
        return std::nullopt;
    };
}

// The value of an expression: variables (when `frame` is given) are substituted, a plain expression is evaluated
// directly, one with a sub-select goes through a one-column DIKHAO so the planner can handle it.
Value Engine::evalExprValue(const ast::Expr& expr, ProcFrame* frame) {
    requireStack();
    auto copy = cloneExpr(expr);
    if (frame) substituteInPlace(copy, frameReplacer(*frame), true);
    std::vector<const ast::Expr*> subqueries;
    findSubqueries(*copy, subqueries);
    if (subqueries.empty()) return evaluate(*copy, Row{});
    ast::Select select;
    select.columns.push_back(std::move(copy));
    select.aliases.push_back(std::nullopt);
    Result r = executeStatement(select);
    return r.rows.empty() || r.rows[0].empty() ? Value() : r.rows[0][0];
}

void Engine::assignVariable(ProcFrame* frame, const std::string& name, Value value) {
    if (!name.empty() && name[0] == '@') {
        if (functionDepth_ > 0) throw ExecutionError("Function ke andar @variable nahi badla ja sakta");
        userVars_[name] = std::move(value);
        return;
    }
    if (size_t dot = name.find('.'); dot != std::string::npos) {
        std::string owner = name.substr(0, dot), column = name.substr(dot + 1);
        if (owner == "purana") throw ExecutionError("PURANA ki value badli nahi ja sakti");
        if (owner != "naya") throw ExecutionError("'" + name + "' ko set nahi kiya ja sakta (sirf NAYA.column)");
        if (!frame || !frame->newRow)
            throw ExecutionError(frame && !frame->newError.empty() ? frame->newError
                                                                    : "NAYA sirf PEHLE DAALO / PEHLE BADLO trigger mein badla ja sakta hai");
        auto it = frame->newRow->find(column);
        if (it == frame->newRow->end()) throw ExecutionError("NAYA mein column '" + column + "' nahi hai");
        it->second = std::move(value);
        frame->newChanged = true;
        return;
    }
    if (frame) {
        auto it = frame->vars.find(name);
        if (it != frame->vars.end()) {
            it->second.value = coerce(value, it->second.type, name);
            return;
        }
    }
    if (name == "autocommit") {
        if (functionDepth_ > 0) throw ExecutionError("Function ke andar autocommit nahi badla ja sakta");
        bool ok = false;
        bool on = truthy(value, ok);
        if (!ok) throw ExecutionError("autocommit ki value 0, 1, ON ya OFF honi chahiye");
        setAutocommit(on);
        return;
    }
    throw ExecutionError("Variable '" + name + "' declare nahi hui (DECLARE " + name + " TYPE) -- ya '@" + name +
                         "' likho session variable ke liye");
}

void Engine::runBodyTree(const std::string& bodyText, ProcFrame& frame) {
    struct DepthGuard {  // local class: same access rights as the enclosing member function
        Engine& engine;
        explicit DepthGuard(Engine& e) : engine(e) {
            if (engine.bodyDepth_ >= kMaxBodyDepth)
                throw ExecutionError(
                    "Trigger/procedure bahut gehra chal raha hai (limit " + std::to_string(kMaxBodyDepth) +
                    ") -- shayad koi trigger khud ko baar-baar chala raha hai");
            ++engine.bodyDepth_;
        }
        ~DepthGuard() { --engine.bodyDepth_; }
    } guard(*this);
    requireStack();

    ast::ProcBlock block = parseProcBody(bodyText);  // fresh parse every time, like Python
    ProcFlow flow = runProcBlock(block, frame);
    if (flow == ProcFlow::Leave || flow == ProcFlow::Iterate)
        throw ExecutionError("LEAVE/ITERATE ke liye label '" + frame.flowLabel + "' wala koi loop nahi mila");
}

Engine::ProcFlow Engine::runProcBlock(const ast::ProcBlock& block, ProcFrame& frame) {
    for (const auto& node : block) {
        ProcFlow flow = runProcNode(*node, frame);
        if (flow != ProcFlow::Next) return flow;
    }
    return ProcFlow::Next;
}

// Parses one stored statement, puts the variables in and runs it through executeStatement.
Result Engine::runProcQuery(const std::string& text, ProcFrame& frame) {
    auto statements = parseScript(text);
    if (statements.size() != 1) throw ExecutionError("Body ka ek statement samajh nahi aaya: " + text);
    substituteStatementInPlace(*statements[0], frameReplacer(frame), true);
    return executeStatement(*statements[0]);
}

void Engine::runProcSql(const std::string& text, ProcFrame& frame) {
    auto statements = parseScript(text);
    for (auto& stmt : statements) {
        if (auto* call = dynamic_cast<ast::CallProcedure*>(stmt.get())) {  // OUT / INOUT parameters need the caller's variables
            std::vector<const ast::Expr*> args;
            for (const auto& a : call->args) args.push_back(a.get());
            Result r = callProcedure(call->name, args, &frame);
            if (frame.results) frame.results->push_back(std::move(r));
            continue;
        }
        substituteStatementInPlace(*stmt, frameReplacer(frame), true);
        Result r = executeStatement(*stmt);
        if (frame.results) frame.results->push_back(std::move(r));
    }
}

void Engine::runProcSelectInto(const ast::PSelectInto& node, ProcFrame& frame) {
    Result r = runProcQuery(node.selectText, frame);
    if (r.columns.size() != node.vars.size())
        throw ExecutionError("SELECT ... INTO mein " + std::to_string(node.vars.size()) + " variable hain par query " +
                             std::to_string(r.columns.size()) + " column deti hai");
    if (r.rows.empty()) {
        if (frame.notFound) {  // the NOT FOUND handler takes over
            assignVariable(&frame, frame.notFound->var, evalExprValue(*frame.notFound->value, &frame));
            return;
        }
        throw ExecutionError("SELECT ... INTO: koi row nahi mili (zero rows) -- INTO ko ek row chahiye");
    }
    if (r.rows.size() > 1)
        throw ExecutionError("SELECT ... INTO: ek se zyada rows mili (" + std::to_string(r.rows.size()) +
                             ") -- INTO ko sirf ek row chahiye");
    for (size_t i = 0; i < node.vars.size(); ++i) assignVariable(&frame, node.vars[i], r.rows[0][i]);
    if (frame.results) frame.results->push_back(Result{});  // counts as a statement, says nothing in the summary
}

void Engine::runProcCursorOp(const ast::PCursorOp& node, ProcFrame& frame) {
    auto it = frame.cursors.find(node.name);
    if (it == frame.cursors.end()) throw ExecutionError("Cursor '" + node.name + "' declare nahi hua (DECLARE " + node.name + " CURSOR FOR ...)");
    ProcFrame::Cursor& cursor = it->second;
    if (node.op == "open") {
        if (cursor.open) throw ExecutionError("Cursor '" + node.name + "' pehle se khula hai");
        Result r = runProcQuery(cursor.selectText, frame);  // the result is taken once, here
        cursor.rows = std::move(r.rows);
        cursor.position = 0;
        cursor.open = true;
    } else if (node.op == "close") {
        if (!cursor.open) throw ExecutionError("Cursor '" + node.name + "' khula hi nahi hai");
        cursor.open = false;
        cursor.rows.clear();
    } else {  // fetch
        if (!cursor.open) throw ExecutionError("Cursor '" + node.name + "' khula nahi hai (pehle OPEN karo)");
        if (cursor.position >= cursor.rows.size()) {
            if (frame.notFound) {
                assignVariable(&frame, frame.notFound->var, evalExprValue(*frame.notFound->value, &frame));
                return;
            }
            throw ExecutionError("FETCH: cursor '" + node.name + "' mein aur rows nahi hain (NOT FOUND) -- DECLARE CONTINUE HANDLER FOR NOT FOUND lagao");
        }
        const auto& row = cursor.rows[cursor.position++];
        if (row.size() != node.vars.size())
            throw ExecutionError("FETCH mein " + std::to_string(node.vars.size()) + " variable hain par cursor " +
                                 std::to_string(row.size()) + " column deta hai");
        for (size_t i = 0; i < node.vars.size(); ++i) assignVariable(&frame, node.vars[i], row[i]);
    }
}

Engine::ProcFlow Engine::runProcLoop(const ast::PLoop& loop, ProcFrame& frame) {
    int64_t iterations = 0;
    // what a LEAVE / ITERATE that came out of the body means for THIS loop
    enum class Next { Again, Stop, Pass };
    auto settle = [&](ProcFlow flow, ProcFlow& out) {
        if (flow == ProcFlow::Next) return Next::Again;
        if (flow == ProcFlow::Return) {
            out = ProcFlow::Return;
            return Next::Pass;
        }
        bool mine = frame.flowLabel.empty() || frame.flowLabel == loop.label;
        if (!mine) {
            out = flow;
            return Next::Pass;
        }
        frame.flowLabel.clear();
        return flow == ProcFlow::Leave ? Next::Stop : Next::Again;
    };
    auto tick = [&]() {
        if (++iterations > kMaxLoopIterations)
            throw ExecutionError("Loop " + std::to_string(kMaxLoopIterations) +
                                 " baar se zyada chal gaya (safety limit) -- shayad ye kabhi nahi rukega");
    };
    ProcFlow out = ProcFlow::Next;
    if (loop.kind == "REPEAT") {
        do {
            tick();
            Next n = settle(runProcBlock(loop.body, frame), out);
            if (n == Next::Pass) return out;
            if (n == Next::Stop) return ProcFlow::Next;
        } while (!isTrue(evalExprValue(*loop.cond, &frame)));
        return ProcFlow::Next;
    }
    while (loop.kind == "LOOP" || isTrue(evalExprValue(*loop.cond, &frame))) {
        tick();
        Next n = settle(runProcBlock(loop.body, frame), out);
        if (n == Next::Pass) return out;
        if (n == Next::Stop) break;
    }
    return ProcFlow::Next;
}

Engine::ProcFlow Engine::runProcNode(const ast::ProcNode& node, ProcFrame& frame) {
    requireStack();
    using namespace ast;
    if (auto* n = dynamic_cast<const PSql*>(&node)) {
        runProcSql(n->text, frame);
    } else if (auto* n = dynamic_cast<const PSet*>(&node)) {
        for (const auto& [name, expr] : n->assignments) assignVariable(&frame, name, evalExprValue(*expr, &frame));
    } else if (auto* n = dynamic_cast<const PDeclare*>(&node)) {
        for (const auto& name : n->names) {
            Value initial = n->defaultValue ? coerce(evalExprValue(*n->defaultValue, &frame), n->typeName, name) : Value();
            frame.vars[name] = {n->typeName, initial};
        }
    } else if (auto* n = dynamic_cast<const PDeclareCursor*>(&node)) {
        frame.cursors[n->name] = ProcFrame::Cursor{n->selectText, false, {}, 0};
    } else if (auto* n = dynamic_cast<const PDeclareHandler*>(&node)) {
        frame.notFound = n;
    } else if (auto* n = dynamic_cast<const PIf*>(&node)) {
        for (const auto& [cond, block] : n->branches)
            if (isTrue(evalExprValue(*cond, &frame))) return runProcBlock(block, frame);
        return runProcBlock(n->elseBlock, frame);
    } else if (auto* n = dynamic_cast<const PLoop*>(&node)) {
        return runProcLoop(*n, frame);
    } else if (auto* n = dynamic_cast<const PLeave*>(&node)) {
        frame.flowLabel = n->label;
        return n->iterate ? ProcFlow::Iterate : ProcFlow::Leave;
    } else if (auto* n = dynamic_cast<const PReturn*>(&node)) {
        if (!frame.isFunction) throw ExecutionError("RETURN sirf function ke andar chalta hai");
        frame.returnValue = evalExprValue(*n->value, &frame);
        frame.returned = true;
        return ProcFlow::Return;
    } else if (auto* n = dynamic_cast<const PSignal*>(&node)) {
        Value message = evalExprValue(*n->message, &frame);
        throw SignalError(message.isNull() ? "(KHALI)" : formatValue(message));
    } else if (auto* n = dynamic_cast<const PSelectInto*>(&node)) {
        runProcSelectInto(*n, frame);
    } else if (auto* n = dynamic_cast<const PCursorOp*>(&node)) {
        runProcCursorOp(*n, frame);
    } else {
        throw ExecutionError("Body ka ye hissa samajh nahi aaya");
    }
    return ProcFlow::Next;
}

// What may appear in which kind of body -- checked when the trigger / procedure / function is created.
void Engine::checkBodyAllowed(const ast::ProcBlock& block, const std::string& kind, const std::string& trigTiming,
                              const std::string& trigEvent) {
    walkBody(block, [&](const ast::ProcNode& node) {
        if (auto* set = dynamic_cast<const ast::PSet*>(&node)) {
            for (const auto& a : set->assignments) {
                const std::string& target = a.first;
                if (target.rfind("naya.", 0) == 0) {
                    if (kind != "TRIGGER" || trigTiming != "PEHLE" || trigEvent == "MITAO")
                        throw ExecutionError("NAYA sirf PEHLE DAALO / PEHLE BADLO trigger mein badla ja sakta hai");
                } else if (target.rfind("purana.", 0) == 0) {
                    throw ExecutionError("PURANA ki value badli nahi ja sakti");
                } else if (kind == "FUNCTION" && (target[0] == '@' || target == "autocommit")) {
                    throw ExecutionError("Function ke andar @variable ya session variable nahi badla ja sakta");
                }
            }
        } else if (dynamic_cast<const ast::PReturn*>(&node)) {
            if (kind != "FUNCTION") throw ExecutionError("RETURN sirf function ke andar chalta hai");
        } else if (auto* sql = dynamic_cast<const ast::PSql*>(&node)) {
            if (kind == "FUNCTION") {
                auto statements = parseScript(sql->text);
                for (const auto& stmt : statements)
                    if (!isReadOnlyStatement(*stmt))
                        throw ExecutionError("Function ke andar sirf padhne wali statements (DIKHAO) chal sakti hain -- " +
                                             astClassName(*stmt) + " data ya schema badalti hai");
            }
        }
    });
}

// ============================================================================
// procedures: parameters with modes
// ============================================================================

Result Engine::callProcedure(const std::string& name, const std::vector<const ast::Expr*>& args, ProcFrame* caller) {
    auto proc = catalog().findProcedure(name);
    if (!proc) throw ExecutionError("Procedure '" + name + "' exist nahi karta");
    if (args.size() != proc->params.size())
        throw ExecutionError("Procedure '" + name + "' ko " + std::to_string(proc->params.size()) +
                             " argument(s) chahiye, " + std::to_string(args.size()) + " mile");

    // Arguments are evaluated left to right (constant expressions at a bare CHALAO, the caller's variables inside a
    // body), each coerced to its parameter's type. An OUT / INOUT argument names a variable that gets the result back.
    ProcFrame frame;
    std::vector<std::string> targets(args.size());
    bool anyOut = false;
    for (size_t i = 0; i < proc->params.size(); ++i) {
        const auto& [pname, ptype] = proc->params[i];
        const std::string mode = i < proc->modes.size() ? proc->modes[i] : "IN";
        if (mode != "IN") {
            anyOut = true;
            auto* ref = dynamic_cast<const ast::ColumnRef*>(args[i]);
            bool local = ref && !ref->table && caller && caller->vars.count(ref->name);
            if (!ref || ref->table || !(ref->name[0] == '@' || local))
                throw ExecutionError("Procedure '" + name + "': " + mode + " parameter '" + pname +
                                     "' ke liye @variable (ya declare kiya hua variable) dena zaroori hai");
            targets[i] = ref->name;
        }
        Value raw;
        if (mode != "OUT") raw = caller ? evalExprValue(*args[i], caller) : evalExprValue(*args[i], nullptr);
        frame.vars[pname] = {ptype, coerce(raw, ptype, pname)};
    }

    std::vector<Result> results;
    frame.results = &results;
    runBodyTree(proc->bodyText, frame);

    Result out;
    std::vector<Value> outValues;
    for (size_t i = 0; i < proc->params.size(); ++i) {
        if (targets[i].empty()) continue;
        Value value = frame.vars[proc->params[i].first].value;
        assignVariable(caller, targets[i], value);
        out.columns.push_back(proc->params[i].first);
        outValues.push_back(value);
    }
    if (anyOut) out.rows.push_back(std::move(outValues));

    std::string summary;
    for (const auto& r : results) {
        if (r.message.empty()) continue;
        if (!summary.empty()) summary += "; ";
        summary += r.message;
    }
    out.message = "Procedure '" + name + "' chal gaya (" + std::to_string(results.size()) + " statement(s)): " + summary;
    return out;
}

// ============================================================================
// stored functions
// ============================================================================

Result Engine::execCreateFunction(const ast::CreateFunction& stmt) {
    Catalog& cat = catalog();
    if (cat.hasFunction(stmt.name)) throw ExecutionError("Function '" + stmt.name + "' pehle se hai");
    const std::string upper = upperOf(stmt.name);
    if (findScalarFunc(upper) || isAggregateName(upper) || upper == "PEHLA" || upper == "COALESCE" || upper == "IFNULL" ||
        upper == "CAST" || upper == "EXISTS" || upper == "ANY" || upper == "SOME" || upper == "ALL")
        throw ExecutionError("'" + stmt.name + "' ek built-in function ka naam hai -- koi aur naam chuno");
    std::set<std::string> names;
    for (const auto& p : stmt.params) names.insert(p.name);
    if (names.size() != stmt.params.size())
        throw ExecutionError("Function '" + stmt.name + "': ek parameter naam do baar diya hai");
    checkBodyAllowed(parseProcBody(stmt.bodyText), "FUNCTION", "", "");
    std::vector<std::pair<std::string, std::string>> params;
    for (const auto& p : stmt.params) params.emplace_back(p.name, p.typeName);
    cat.addFunction(stmt.name, params, stmt.returnType, stmt.bodyText, nowText());
    return messageResult("Function '" + stmt.name + "' ban gaya (" + std::to_string(stmt.params.size()) + " parameter(s), RETURNS " +
                         stmt.returnType + ")");
}

Result Engine::execDropFunction(const ast::DropFunction& stmt) {
    Catalog& cat = catalog();
    if (!cat.hasFunction(stmt.name)) {
        if (stmt.ifExists) return messageResult("Function '" + stmt.name + "' exist nahi karta -- kuch nahi hatana (IF EXISTS)");
        throw ExecutionError("Function '" + stmt.name + "' exist nahi karta");
    }
    cat.removeFunction(stmt.name);
    return messageResult("Function '" + stmt.name + "' hata diya");
}

// every database's functions: name, database, returns, created
Result Engine::execShowFunctions(const ast::ShowFunctions&) {
    Result r;
    r.columns = {"name", "database", "returns", "created"};
    for (const auto& db : instance_->databases()) {
        Catalog& cat = instance_->catalog(db);
        for (auto it = cat.functions.begin(); it != cat.functions.end(); ++it) {
            auto info = cat.findFunction(it.key());
            if (!info) continue;
            r.rows.push_back({textValue(it.key()), textValue(db), textValue(info->returnType),
                              info->created.empty() ? Value() : textValue(info->created)});
        }
    }
    r.message = std::to_string(r.rows.size()) + " function(s)";
    return r;
}

// ============================================================================
// SET, SHOW VARIABLES
// ============================================================================

Result Engine::execSetVariable(const ast::SetVariable& stmt) {
    std::string said;
    for (const auto& [name, expr] : stmt.assignments) {
        if (name.empty() || (name[0] != '@' && name != "autocommit"))
            throw ExecutionError("SET ke liye '" + name + "' koi variable nahi hai -- session variable '@" + name +
                                 "' likho, ya autocommit");
        Value value = evalExprValue(*expr, nullptr);
        assignVariable(nullptr, name, value);
        if (!said.empty()) said += "; ";
        said += name + " = " + (value.isNull() ? "KHALI" : formatValue(value));
    }
    if (stmt.assignments.size() == 1 && stmt.assignments[0].first == "autocommit")
        return messageResult(autocommit_ ? "autocommit = 1 (har statement turant save; khula transaction PAKKA ho gaya)"
                                         : "autocommit = 0 (ab har badlav transaction mein; PAKKA se save, WAPAS se undo)");
    return messageResult(said);
}

Result Engine::execShowVariables(const ast::ShowVariables& stmt) {
    Result r;
    r.columns = {"variable_name", "value"};
    if (!stmt.hasPattern || likeText("autocommit", stmt.pattern)) r.rows.push_back({textValue("autocommit"), textValue(autocommit_ ? "ON" : "OFF")});
    if (!stmt.hasPattern || likeText("transaction_isolation", stmt.pattern))
        r.rows.push_back({textValue("transaction_isolation"), textValue(isolation_)});
    r.message = std::to_string(r.rows.size()) + " variable(s)";
    return r;
}

// ============================================================================
// PREPARE / EXECUTE / DEALLOCATE PREPARE
// ============================================================================

Result Engine::execPrepare(const ast::Prepare& stmt) {
    Value source = evalExprValue(*stmt.source, nullptr);
    if (!std::holds_alternative<std::string>(source.data))
        throw ExecutionError("PREPARE ko SQL text (string, ya string wala @variable) chahiye");
    const std::string text = std::get<std::string>(source.data);
    // parse it once now with a harmless number in every ? slot, so a typo shows up at PREPARE time
    auto positions = placeholderPositions(text);
    auto statements = parseScript(fillPlaceholders(text, positions, std::vector<std::string>(positions.size(), "0")));
    if (statements.size() != 1) throw ExecutionError("PREPARE mein bilkul ek statement hona chahiye (" + std::to_string(statements.size()) + " mile)");
    prepared_[stmt.name] = text;
    return messageResult("Prepared statement '" + stmt.name + "' tayyar (" + std::to_string(positions.size()) + " ? placeholder)");
}

Result Engine::execExecutePrepared(const ast::ExecutePrepared& stmt) {
    auto it = prepared_.find(stmt.name);
    if (it == prepared_.end()) throw ExecutionError("Prepared statement '" + stmt.name + "' exist nahi karta (pehle PREPARE karo)");
    const std::string text = it->second;
    auto positions = placeholderPositions(text);
    if (stmt.args.size() != positions.size())
        throw ExecutionError("Prepared statement '" + stmt.name + "' mein " + std::to_string(positions.size()) +
                             " ? hain par USING mein " + std::to_string(stmt.args.size()) + " value(s) mili");
    std::vector<std::string> literals;
    for (const auto& arg : stmt.args) literals.push_back(sqlLiteral(evalExprValue(*arg, nullptr)));
    auto statements = parseScript(fillPlaceholders(text, positions, literals));
    if (statements.size() != 1) throw ExecutionError("Prepared statement mein bilkul ek statement hona chahiye");
    return executeStatement(*statements[0]);
}

Result Engine::execDeallocate(const ast::Deallocate& stmt) {
    if (prepared_.erase(stmt.name) == 0)
        throw ExecutionError("Prepared statement '" + stmt.name + "' exist nahi karta");
    return messageResult("Prepared statement '" + stmt.name + "' hata diya");
}

}  // namespace meradb
