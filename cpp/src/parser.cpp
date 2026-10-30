// cpp/src/parser.cpp
#include "meradb/parser.h"
#include "meradb/aggregates.h"
#include "meradb/ast_util.h"
#include "meradb/datatypes.h"
#include "meradb/errors.h"
#include "meradb/pytext.h"
#include "meradb/scalar_funcs.h"
#include "meradb/stack_guard.h"
#include <algorithm>
#include <cctype>

namespace meradb {

using namespace ast;

Parser::Parser(std::vector<Token> tokens, std::string sourceText)
    : tokens_(std::move(tokens)), sourceText_(std::move(sourceText)) {}

const Token& Parser::peek(int offset) const {
    size_t i = pos_ + static_cast<size_t>(offset);
    return i < tokens_.size() ? tokens_[i] : tokens_.back();  // back() is Eof
}
const Token& Parser::advance() {
    const Token& tok = tokens_[pos_];
    if (tok.type != TokenType::Eof) ++pos_;
    return tok;
}

bool Parser::checkKeyword(const std::string& kw) const {
    return peek().type == TokenType::Keyword && peek().textValue == kw;
}
bool Parser::matchKeyword(const std::string& kw) {
    if (checkKeyword(kw)) { advance(); return true; }
    return false;
}
void Parser::expectKeyword(const std::string& kw) {
    if (!matchKeyword(kw)) error("'" + kw + "' expected tha");
}
bool Parser::checkSymbol(const std::string& sym) const {
    return peek().type == TokenType::Symbol && peek().textValue == sym;
}
bool Parser::matchSymbol(const std::string& sym) {
    if (checkSymbol(sym)) { advance(); return true; }
    return false;
}
void Parser::expectSymbol(const std::string& sym) {
    if (!matchSymbol(sym)) error("'" + sym + "' expected tha");
}
std::string Parser::expectIdent(const std::string& what) {
    if (peek().type != TokenType::Ident) error(what + " expected tha");
    return advance().textValue;
}
void Parser::spend() {
    if (++nestingUsed_ > kMaxNesting)
        error("Query bahut gehri (nested) hai (limit " + std::to_string(kMaxNesting) + ")");
}

void Parser::guardStack() {
    if (stackExhausted()) error(stackLimitMessage());
}

[[noreturn]] void Parser::error(const std::string& msg) const {
    // Python: f"{msg}, par {found} mila (line {line}, col {col})", where found
    // is "end of query" or repr(token value).
    const Token& tok = peek();
    std::string found;
    switch (tok.type) {
        case TokenType::Eof: found = "end of query"; break;
        case TokenType::Number: found = tok.isFloat ? pyReprFloat(tok.doubleValue) : std::to_string(tok.intValue); break;
        default: found = pyRepr(tok.textValue); break;
    }
    throw ParseError(msg + ", par " + found + " mila (line " + std::to_string(tok.line) + ", col " +
                     std::to_string(tok.col) + ")");
}

// ---------------------------------------------------------------------
// Expressions (precedence, lowest to highest): parseOr -> parseAnd ->
// parseNot -> parseComparison -> parseAdditive -> parseTerm -> parseUnary
// -> parsePrimary. Mirrors meradb/parser.py's _parse_or.._parse_primary
// exactly.
// ---------------------------------------------------------------------

std::unique_ptr<Expr> Parser::parseOr() {
    auto left = parseAnd();
    while (matchKeyword("YA")) {
        spend();
        left = std::make_unique<BinaryOp>("YA", std::move(left), parseAnd());
    }
    return left;
}
std::unique_ptr<Expr> Parser::parseAnd() {
    auto left = parseNot();
    while (matchKeyword("AUR")) {
        spend();
        left = std::make_unique<BinaryOp>("AUR", std::move(left), parseNot());
    }
    return left;
}
std::unique_ptr<Expr> Parser::parseNot() {
    guardStack();
    if (matchKeyword("NAHI")) {
        spend();
        auto u = std::make_unique<UnaryOp>();
        u->op = "NAHI";
        u->operand = parseNot();
        return u;
    }
    // English NOT EXISTS (...): NOT is not a keyword, so only this exact pair is taken
    if (peek().type == TokenType::Ident && peek().textValue == "not" && peek(1).type == TokenType::Ident &&
        peek(1).textValue == "exists") {
        advance();
        spend();
        auto u = std::make_unique<UnaryOp>();
        u->op = "NAHI";
        u->operand = parseNot();
        return u;
    }
    return parseComparison();
}

std::unique_ptr<Expr> Parser::parseComparison() {
    auto left = parseAdditive();

    // x HAI KHALI  /  x HAI NAHI KHALI
    if (matchKeyword("HAI")) {
        bool negated = matchKeyword("NAHI");
        expectKeyword("KHALI");
        auto isnull = std::make_unique<IsNull>();
        isnull->expr = std::move(left);
        isnull->negated = negated;
        return isnull;
    }

    // x [NAHI] JAISA / BEECH / MEIN ... -- this NAHI is *infix*, distinct
    // from the prefix NAHI handled in parseNot().
    bool negated = false;
    const Token& nxt = peek(1);
    if (checkKeyword("NAHI") && nxt.type == TokenType::Keyword &&
        (nxt.textValue == "JAISA" || nxt.textValue == "BEECH" || nxt.textValue == "MEIN")) {
        advance();
        negated = true;
    }

    auto node = parsePatternRangeOrList(left);
    if (node) {
        if (negated) {
            auto u = std::make_unique<UnaryOp>();
            u->op = "NAHI";
            u->operand = std::move(node);
            return u;
        }
        return node;
    }

    for (const char* op : {"=", "!=", "<=", ">=", "<", ">"}) {
        if (matchSymbol(op)) {
            // x op ANY|SOME|ALL (DIKHAO ...)
            const Token& q = peek();
            if (q.type == TokenType::Ident && (q.textValue == "any" || q.textValue == "some" || q.textValue == "all") &&
                peek(1).type == TokenType::Symbol && peek(1).textValue == "(" &&
                peek(2).type == TokenType::Keyword && peek(2).textValue == "DIKHAO") {
                std::string kind = q.textValue == "all" ? "ALL" : "ANY";
                advance();
                advance();
                advance();
                spend();
                auto sub = std::make_unique<Subquery>();
                sub->statement = parseSelectBody();
                expectSymbol(")");
                auto quantified = std::make_unique<InSubquery>();
                quantified->left = std::move(left);
                quantified->subquery = std::move(sub);
                quantified->kind = kind;
                quantified->op = op;
                return quantified;
            }
            return std::make_unique<BinaryOp>(op, std::move(left), parseAdditive());
        }
    }
    return left;
}

std::unique_ptr<Expr> Parser::parsePatternRangeOrList(std::unique_ptr<Expr>& left) {
    if (matchKeyword("JAISA")) {
        return std::make_unique<BinaryOp>("JAISA", std::move(left), parseAdditive());
    }
    if (matchKeyword("BEECH")) {
        auto lo = parseAdditive();  // additive stops before AUR, so this is safe
        expectKeyword("AUR");
        auto hi = parseAdditive();
        auto leftCopy = cloneExpr(*left);
        auto ge = std::make_unique<BinaryOp>(">=", std::move(left), std::move(lo));
        auto le = std::make_unique<BinaryOp>("<=", std::move(leftCopy), std::move(hi));
        return std::make_unique<BinaryOp>("AUR", std::move(ge), std::move(le));
    }
    if (matchKeyword("MEIN")) {
        expectSymbol("(");
        spend();
        if (checkKeyword("DIKHAO")) {
            auto sub = std::make_unique<Subquery>();
            expectKeyword("DIKHAO");
            sub->statement = parseSelectBody();
            expectSymbol(")");
            auto inSub = std::make_unique<InSubquery>();
            inSub->left = std::move(left);
            inSub->subquery = std::move(sub);
            inSub->negated = false;
            return inSub;
        }
        // literal list -> OR chain of equality comparisons. Each item is a
        // full expression (parseOr), matching Python's _parse_expr() here.
        auto leftCopy = cloneExpr(*left);
        std::unique_ptr<Expr> node = std::make_unique<BinaryOp>("=", std::move(leftCopy), parseOr());
        while (matchSymbol(",")) {
            spend();  // the list becomes an OR chain: one level per item
            auto leftCopy2 = cloneExpr(*left);
            auto eq = std::make_unique<BinaryOp>("=", std::move(leftCopy2), parseOr());
            node = std::make_unique<BinaryOp>("YA", std::move(node), std::move(eq));
        }
        expectSymbol(")");
        return node;
    }
    // No pattern/range/list keyword matched: `left` is untouched, caller
    // falls through to plain comparison operators.
    return nullptr;
}

std::unique_ptr<Expr> Parser::parseAdditive() {
    auto left = parseTerm();
    while (checkSymbol("+") || checkSymbol("-") || checkSymbol("||")) {
        std::string op = advance().textValue;
        spend();
        left = std::make_unique<BinaryOp>(op, std::move(left), parseTerm());
    }
    return left;
}
std::unique_ptr<Expr> Parser::parseTerm() {
    auto left = parseUnary();
    // DIV (integer division) is a soft word, so a column called `div` keeps working elsewhere
    while (checkSymbol("*") || checkSymbol("/") || checkSymbol("%") ||
           (peek().type == TokenType::Ident && peek().textValue == "div")) {
        std::string op = advance().textValue;
        if (op == "div") op = "DIV";
        spend();
        left = std::make_unique<BinaryOp>(op, std::move(left), parseUnary());
    }
    return left;
}
std::unique_ptr<Expr> Parser::parseUnary() {
    guardStack();
    if (checkSymbol("-")) {
        advance();
        spend();
        auto operand = parseUnary();
        // Fold unary minus directly on a numeric literal at parse time
        // (matches Python's parser-time fold in _parse_unary).
        if (auto* lit = dynamic_cast<Literal*>(operand.get())) {
            if (std::holds_alternative<int64_t>(lit->value.data))
                return std::make_unique<Literal>(Value(-std::get<int64_t>(lit->value.data)));
            if (std::holds_alternative<double>(lit->value.data))
                return std::make_unique<Literal>(Value(-std::get<double>(lit->value.data)));
        }
        auto u = std::make_unique<UnaryOp>();
        u->op = "-";
        u->operand = std::move(operand);
        return u;
    }
    return parsePrimary();
}

std::unique_ptr<Expr> Parser::parsePrimary() {
    guardStack();
    const Token& t = peek();
    if (t.type == TokenType::Number) {
        advance();
        return std::make_unique<Literal>(t.isFloat ? Value(t.doubleValue) : Value(t.intValue));
    }
    if (t.type == TokenType::String) {
        advance();
        return std::make_unique<Literal>(Value(t.textValue));
    }
    if (matchKeyword("SACH")) return std::make_unique<Literal>(Value(true));
    if (matchKeyword("JHOOTH")) return std::make_unique<Literal>(Value(false));
    if (matchKeyword("KHALI")) return std::make_unique<Literal>(Value());
    if (matchKeyword("AGAR")) {
        spend();
        return parseCase();
    }
    if (t.type == TokenType::Ident) {
        std::string name = t.textValue;
        advance();
        if (name == "current_date" && !checkSymbol("(") && !checkSymbol(".")) {
            auto today = std::make_unique<ScalarFunc>();  // CURRENT_DATE without brackets
            today->name = "CURDATE";
            return today;
        }
        if (matchSymbol("(")) {
            spend();
            // function call: aggregate (GINO/KUL/AUSAT/NYUNTAM/ADHIKTAM/...)
            // or PEHLA/COALESCE.
            std::string upper = name;
            for (auto& c : upper) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
            if (upper == "PEHLA" || upper == "COALESCE") {
                auto co = std::make_unique<Coalesce>();
                co->args.push_back(parseOr());
                while (matchSymbol(",")) co->args.push_back(parseOr());
                expectSymbol(")");
                return co;
            }
            if (upper == "IFNULL") {
                auto co = std::make_unique<Coalesce>();
                co->args.push_back(parseOr());
                expectSymbol(",");
                co->args.push_back(parseOr());
                expectSymbol(")");
                return co;
            }
            if (upper == "EXISTS" && checkKeyword("DIKHAO")) {
                advance();
                auto sub = std::make_unique<Subquery>();
                sub->statement = parseSelectBody();
                expectSymbol(")");
                auto exists = std::make_unique<InSubquery>();
                exists->subquery = std::move(sub);
                exists->kind = "EXISTS";
                return exists;
            }
            if (upper == "CAST") {
                // CAST(expr AS type) -- AS is written KAHO or AS
                auto cast = std::make_unique<ScalarFunc>();
                cast->name = "CAST";
                cast->args.push_back(parseOr());
                if (!matchKeyword("KAHO")) {
                    if (peek().type == TokenType::Ident && peek().textValue == "as") advance();
                    else error("CAST mein expression ke baad KAHO (ya AS) expected tha");
                }
                std::optional<std::string> typeName;
                if (peek().type == TokenType::Ident) typeName = normalizeType(peek().textValue);
                if (!typeName) error("CAST ke liye type expected tha (INT/ANK, FLOAT/DASHAMLAV, TEXT/SHABD, DATE/TAREEKH, BOOL)");
                advance();
                cast->castType = *typeName;
                expectSymbol(")");
                return cast;
            }
            if (const ScalarSpec* spec = findScalarFunc(upper)) {
                auto sf = std::make_unique<ScalarFunc>();
                sf->name = upper;
                if (!checkSymbol(")")) {
                    sf->args.push_back(parseOr());
                    while (matchSymbol(",")) sf->args.push_back(parseOr());
                }
                int count = static_cast<int>(sf->args.size());
                if (count < spec->minArgs || (spec->maxArgs >= 0 && count > spec->maxArgs)) {
                    std::string want = spec->maxArgs < 0 ? "kam se kam " + std::to_string(spec->minArgs)
                                       : spec->minArgs == spec->maxArgs ? std::to_string(spec->minArgs)
                                       : std::to_string(spec->minArgs) + " ya " + std::to_string(spec->maxArgs);
                    error(upper + " ko " + want + " argument chahiye (" + std::to_string(count) + " mile)");
                }
                expectSymbol(")");
                return sf;
            }
            if (!isAggregateName(upper) && !checkSymbol("*")) {
                // not a built-in: a call of a stored function (BANAO FUNCTION). The engine knows whether it exists.
                auto uf = std::make_unique<ScalarFunc>();
                uf->user = true;
                uf->name = name;
                if (!checkSymbol(")")) {
                    uf->args.push_back(parseOr());
                    while (matchSymbol(",")) uf->args.push_back(parseOr());
                }
                expectSymbol(")");
                return uf;
            }
            auto fc = std::make_unique<FuncCall>();
            fc->name = upper;
            if (checkSymbol("*")) { advance(); fc->arg = std::make_unique<Star>(); }
            else fc->arg = parseOr();
            expectSymbol(")");
            return fc;
        }
        if (matchSymbol(".")) {
            if (matchSymbol("*")) {
                auto s = std::make_unique<Star>();
                s->table = name;
                return s;
            }
            std::string col = expectIdent("column ka naam");
            return std::make_unique<ColumnRef>(col, name);
        }
        return std::make_unique<ColumnRef>(name);
    }
    if (matchSymbol("(")) {
        spend();
        if (checkKeyword("DIKHAO")) {
            auto sub = std::make_unique<Subquery>();
            expectKeyword("DIKHAO");
            sub->statement = parseSelectBody();
            expectSymbol(")");
            return sub;
        }
        auto inner = parseOr();
        expectSymbol(")");
        return inner;
    }
    error("Value ya column ka naam expected tha");
}

std::unique_ptr<CaseWhen> Parser::parseCase() {
    // Assumes AGAR was already consumed.
    // case := "AGAR" expr "TAB" expr { "AGAR" expr "TAB" expr } ["WARNA" expr] "KHATAM"
    auto cw = std::make_unique<CaseWhen>();
    while (true) {
        auto cond = parseOr();
        expectKeyword("TAB");
        auto value = parseOr();
        cw->branches.emplace_back(std::move(cond), std::move(value));
        if (!matchKeyword("AGAR")) break;
    }
    if (matchKeyword("WARNA")) cw->elseExpr = parseOr();
    expectKeyword("KHATAM");
    return cw;
}

std::unique_ptr<Expr> Parser::parseExpressionEntry() {
    StackBase base;
    return parseOr();
}

// ---------------------------------------------------------------------
// Statement-level parsing (Task 7: DDL/database statements; Task 8: DML,
// SELECT, set ops, transactions). Mirrors meradb/parser.py's
// _parse_statement and its per-keyword handlers.
// ---------------------------------------------------------------------
std::vector<std::unique_ptr<Statement>> Parser::parseScript() {
    StackBase base;
    std::vector<std::unique_ptr<Statement>> result;
    while (peek().type != TokenType::Eof) {
        if (matchSymbol(";")) continue;  // allow empty statements like ';;'
        result.push_back(parseStatement());
        if (peek().type != TokenType::Eof) expectSymbol(";");
    }
    return result;
}

// Every (possibly nested) statement gets its own operator budget, and statements
// nested inside statements (SAMJHAO ..., trigger/procedure bodies) are capped.
std::unique_ptr<Statement> Parser::parseStatement() {
    struct Scope {
        Parser& parser;
        int savedBudget;
        explicit Scope(Parser& p) : parser(p), savedBudget(p.nestingUsed_) {
            if (p.statementDepth_ >= kMaxStatementNesting)
                p.error("Statements bahut gehre nested hain (limit " + std::to_string(kMaxStatementNesting) + ")");
            ++p.statementDepth_;
            p.nestingUsed_ = 0;
        }
        ~Scope() {
            --parser.statementDepth_;
            parser.nestingUsed_ = savedBudget;
        }
    } scope(*this);
    guardStack();
    return parseStatementBody();
}

std::unique_ptr<Statement> Parser::parseStatementBody() {
    const Token& tok = peek();
    // REPLACE INTO t ... / BADAL DAALO MEIN t ...: DAALO MEIN, but a row colliding on MUKHYA KUNJI / ANOKHA is replaced
    if (tok.type == TokenType::Ident && tok.textValue == "replace" && peek(1).type == TokenType::Ident &&
        peek(1).textValue == "into") {
        advance();
        advance();
        return parseInsert(true, true);
    }
    if (tok.type == TokenType::Ident && tok.textValue == "badal" && peek(1).type == TokenType::Keyword &&
        peek(1).textValue == "DAALO") {
        advance();
        advance();
        return parseInsert(true, false);
    }
    // SHOW ... and SET ROLE ... are statement words only here, at the start of a statement (a column may still be called show)
    if (tok.type == TokenType::Ident && tok.textValue == "show") {
        advance();
        return parseShow();
    }
    if (tok.type == TokenType::Ident && tok.textValue == "set" && isSoftWord(1, "role")) {
        advance();
        advance();
        return parseSetRole();
    }
    // SET autocommit = 0 | SET @x = expr; SAVEPOINT / ROLLBACK TO / RELEASE SAVEPOINT; PREPARE / EXECUTE / DEALLOCATE PREPARE:
    // statement words only here, at the start of a statement
    if (tok.type == TokenType::Ident) {
        const std::string word = tok.textValue;
        if (word == "set") {
            advance();
            return parseSet();
        }
        if (word == "savepoint") {
            advance();
            auto s = std::make_unique<Savepoint>();
            s->name = expectIdent("savepoint ka naam");
            return s;
        }
        if (word == "release") {
            advance();
            if (!isSoftWord(0, "savepoint")) error("RELEASE ke baad SAVEPOINT naam expected tha");
            advance();
            auto s = std::make_unique<ReleaseSavepoint>();
            s->name = expectIdent("savepoint ka naam");
            return s;
        }
        if (word == "rollback" && isSoftWord(1, "to")) {
            advance();
            advance();
            if (isSoftWord(0, "savepoint")) advance();
            auto s = std::make_unique<RollbackToSavepoint>();
            s->name = expectIdent("savepoint ka naam");
            return s;
        }
        if (word == "prepare") {
            advance();
            return parsePrepare();
        }
        if (word == "execute") {
            advance();
            return parseExecute();
        }
        if (word == "deallocate") {
            advance();
            if (!isSoftWord(0, "prepare")) error("DEALLOCATE ke baad PREPARE naam expected tha");
            advance();
            auto d = std::make_unique<Deallocate>();
            d->name = expectIdent("prepared statement ka naam");
            return d;
        }
    }
    if (tok.type != TokenType::Keyword) {
        error("Query kisi command se shuru honi chahiye (jaise DIKHAO, DAALO, BANAO)");
    }
    std::string kw = tok.textValue;
    if (kw == "BANAO") { advance(); return parseBanao(); }
    if (kw == "HATAO") { advance(); return parseHatao(); }
    if (kw == "SUDHARO") { advance(); return parseSudharo(); }
    if (kw == "SAAF") { advance(); return parseSaaf(); }
    if (kw == "SIKODO") { advance(); return parseSikodo(); }
    if (kw == "ISTEMAL") { advance(); return parseIstemal(); }
    if (kw == "BATAO") { advance(); return parseBatao(); }
    if (kw == "DAALO") { advance(); return parseInsert(false, false); }
    if (kw == "DIKHAO") { advance(); return parseDikhaoStmt(); }
    if (kw == "BADLO") { advance(); return parseUpdate(); }
    if (kw == "MITAO") { advance(); return parseDelete(); }
    if (kw == "SHURU") { advance(); return std::make_unique<Begin>(); }
    if (kw == "PAKKA") { advance(); return std::make_unique<Commit>(); }
    if (kw == "WAPAS") {
        advance();
        if (isSoftWord(0, "tak") || isSoftWord(0, "to")) {  // WAPAS TAK name = ROLLBACK TO SAVEPOINT name
            advance();
            if (isSoftWord(0, "savepoint")) advance();
            auto s = std::make_unique<RollbackToSavepoint>();
            s->name = expectIdent("savepoint ka naam");
            return s;
        }
        return std::make_unique<Rollback>();
    }
    if (kw == "SAMJHAO") {
        advance();
        auto ex = std::make_unique<Explain>();
        ex->statement = parseStatement();
        return ex;
    }
    if (kw == "ADHIKAR") { advance(); return parseAdhikar(); }
    if (kw == "CHALAO") { advance(); return parseChalao(); }
    error("Ye command nahi pata");
}

// ---- DDL ----

bool Parser::matchIfExists() {
    if (!(isSoftWord(0, "if") && isSoftWord(1, "exists"))) return false;
    advance();
    advance();
    return true;
}

bool Parser::matchIfNotExists() {
    if (!(isSoftWord(0, "if") && isSoftWord(1, "not") && isSoftWord(2, "exists"))) return false;
    advance();
    advance();
    advance();
    return true;
}

std::unique_ptr<Statement> Parser::parseBanao() {
    // BANAO [ANOKHA] INDEX [IF NOT EXISTS] naam PAR table (a, b)
    if (isSoftWord(0, "index") || (checkKeyword("ANOKHA") && isSoftWord(1, "index"))) {
        auto ci = std::make_unique<CreateIndex>();
        ci->unique = matchKeyword("ANOKHA");
        advance();  // INDEX
        ci->ifNotExists = matchIfNotExists();
        ci->name = expectIdent("index ka naam");
        expectKeyword("PAR");
        ci->table = expectIdent("table ka naam");
        ci->columns = parseCompositeColumns(1);
        return ci;
    }
    // BANAO DATABASE name
    if (matchKeyword("DATABASE")) {
        auto d = std::make_unique<CreateDatabase>();
        d->ifNotExists = matchIfNotExists();
        d->name = expectIdent("database ka naam");
        return d;
    }
    // BANAO VIEW naam KAHO DIKHAO ...   (also BANAO OR REPLACE VIEW / BANAO BADAL VIEW: replace an existing view)
    if (matchKeyword("VIEW")) return parseCreateView();
    if ((isSoftWord(0, "or") && isSoftWord(1, "replace") && checkKeywordAt(2, "VIEW")) ||
        (isSoftWord(0, "badal") && checkKeywordAt(1, "VIEW"))) {
        advance();
        if (isSoftWord(0, "replace")) advance();
        advance();  // VIEW
        auto cv = parseCreateView();
        cv->orReplace = true;
        return cv;
    }
    // BANAO ROLE naam
    if (isSoftWord(0, "role") && peek(1).type == TokenType::Ident) {
        advance();
        auto r = std::make_unique<CreateRole>();
        r->ifNotExists = matchIfNotExists();
        r->name = expectIdent("role ka naam");
        return r;
    }

    // BANAO USER naam GUPT 'password'
    if (matchKeyword("USER")) {
        auto u = std::make_unique<CreateUser>();
        u->ifNotExists = matchIfNotExists();
        u->name = expectIdent("user ka naam");
        expectKeyword("GUPT");
        u->password = expectString("password");
        return u;
    }
    // BANAO TRIGGER naam PEHLE|BAAD DAALO|BADLO|MITAO PAR table SHURU ... KHATAM
    if (matchKeyword("TRIGGER")) return parseCreateTrigger();
    // BANAO PROCEDURE naam (p1 TYPE, ...) SHURU ... KHATAM
    if (matchKeyword("PROCEDURE")) return parseCreateProcedure();
    // BANAO FUNCTION naam (p1 TYPE, ...) RETURNS TYPE SHURU ... RETURN expr; KHATAM
    if (isSoftWord(0, "function") && peek(1).type == TokenType::Ident) {
        advance();
        return parseCreateFunction();
    }

    // BANAO TABLE name ( coldef | table-constraint, ... )
    expectKeyword("TABLE");
    auto stmt = std::make_unique<CreateTable>();
    stmt->ifNotExists = matchIfNotExists();
    stmt->name = expectIdent("table ka naam");
    // BANAO TABLE t KAHO DIKHAO ...  (also AS)  -- the table takes the query's columns and rows
    if (checkKeyword("KAHO") || (peek().type == TokenType::Ident && peek().textValue == "as")) {
        advance();
        expectKeyword("DIKHAO");
        stmt->asQuery = parseDikhaoStmt();
        return stmt;
    }
    // BANAO TABLE t LIKE other  -- same columns and constraints, no rows
    if (peek().type == TokenType::Ident && peek().textValue == "like") {
        advance();
        stmt->likeTable = expectIdent("table ka naam");
        return stmt;
    }
    expectSymbol("(");
    parseTableItem(*stmt);
    while (matchSymbol(",")) parseTableItem(*stmt);
    expectSymbol(")");
    return stmt;
}

void Parser::parseTableItem(CreateTable& stmt) {
    // A bare ANOKHA/MUKHYA keyword here (not following an IDENT column
    // name) can only be the table-level composite-constraint form, since a
    // column def always starts with an IDENT (the column's own name).
    if (atForeignKeyItem(0)) {
        stmt.foreignKeys.push_back(parseForeignKeyDef());
        return;
    }
    if (checkKeyword("ANOKHA")) {
        advance();
        stmt.compositeUnique.push_back(parseCompositeColumns());
        return;
    }
    if (checkKeyword("MUKHYA") && peek(1).type == TokenType::Keyword && peek(1).textValue == "KUNJI") {
        advance();
        advance();
        if (stmt.compositePk.has_value()) {
            error("Ek table mein sirf ek MUKHYA KUNJI ho sakti hai (composite bhi sirf ek)");
        }
        stmt.compositePk = parseCompositeColumns();
        return;
    }
    stmt.columns.push_back(parseColumnDef());
}

std::vector<std::string> Parser::parseCompositeColumns(size_t minColumns) {
    expectSymbol("(");
    std::vector<std::string> cols;
    cols.push_back(expectIdent("column ka naam"));
    while (matchSymbol(",")) cols.push_back(expectIdent("column ka naam"));
    expectSymbol(")");
    if (cols.size() < minColumns) {
        error("Composite constraint mein kam se kam 2 columns chahiye (1 column ke liye normal ANOKHA/MUKHYA KUNJI use karo)");
    }
    return cols;
}

bool Parser::isSoftWord(int offset, const char* word) const {
    return peek(offset).type == TokenType::Ident && peek(offset).textValue == word;
}

// ON DELETE|UPDATE CASCADE | SET NULL | RESTRICT | NO ACTION, in any order, each at most once. English words (ON MITAO /
// ON BADLO are accepted for DELETE / UPDATE).
void Parser::parseFkActions(std::string& onDelete, std::string& onUpdate) {
    while (isSoftWord(0, "on") && (isSoftWord(1, "delete") || isSoftWord(1, "update") || checkKeywordAt(1, "MITAO") ||
                                   checkKeywordAt(1, "BADLO"))) {
        advance();
        bool isDelete = isSoftWord(0, "delete") || checkKeyword("MITAO");
        advance();
        std::string action;
        if (isSoftWord(0, "cascade")) { advance(); action = "CASCADE"; }
        else if (isSoftWord(0, "restrict")) { advance(); action = "RESTRICT"; }
        else if (isSoftWord(0, "no") && isSoftWord(1, "action")) { advance(); advance(); action = "NO ACTION"; }
        else if (isSoftWord(0, "set") && (isSoftWord(1, "null") || checkKeywordAt(1, "KHALI"))) {
            advance();
            advance();
            action = "SET NULL";
        } else {
            error(std::string("ON ") + (isDelete ? "DELETE" : "UPDATE") +
                  " ke baad CASCADE, SET NULL, RESTRICT ya NO ACTION expected tha");
        }
        std::string& slot = isDelete ? onDelete : onUpdate;
        if (!slot.empty()) error(std::string("ON ") + (isDelete ? "DELETE" : "UPDATE") + " do baar diya hai");
        slot = action;
    }
}

bool Parser::checkKeywordAt(int offset, const std::string& kw) const {
    return peek(offset).type == TokenType::Keyword && peek(offset).textValue == kw;
}

// [CONSTRAINT name] FOREIGN KEY ... starting `offset` tokens ahead?
bool Parser::atForeignKeyItem(int offset) const {
    if (isSoftWord(offset, "foreign") && isSoftWord(offset + 1, "key")) return true;
    return isSoftWord(offset, "constraint") && peek(offset + 1).type == TokenType::Ident && isSoftWord(offset + 2, "foreign") &&
           isSoftWord(offset + 3, "key");
}

// [CONSTRAINT name] FOREIGN KEY (col) SANDARBH parent(col) [ON DELETE ..] [ON UPDATE ..]
ForeignKeyDef Parser::parseForeignKeyDef() {
    ForeignKeyDef fk;
    if (isSoftWord(0, "constraint")) {
        advance();
        fk.name = expectIdent("constraint ka naam");
    }
    advance();  // foreign
    advance();  // key
    expectSymbol("(");
    fk.column = expectIdent("column ka naam");
    expectSymbol(")");
    if (!matchKeyword("SANDARBH")) {
        if (isSoftWord(0, "references")) advance();
        else error("FOREIGN KEY (...) ke baad SANDARBH parent(column) expected tha");
    }
    fk.refTable = expectIdent("parent table ka naam");
    expectSymbol("(");
    fk.refColumn = expectIdent("parent column ka naam");
    expectSymbol(")");
    parseFkActions(fk.onDelete, fk.onUpdate);
    return fk;
}

ColumnDef Parser::parseColumnDef() {
    // coldef := name TYPE [ "(" INTEGER [ "," INTEGER ] ")" ]
    //           [MUKHYA KUNJI | ZAROORI | ANOKHA | WARNA literal | SANDARBH ref | SHART "(" expr ")"]*
    ColumnDef col;
    col.name = expectIdent("column ka naam");
    const Token& typeTok = peek();
    if (typeTok.type == TokenType::Ident && typeTok.textValue == "enum") {
        // ENUM('a', 'b', ...): stored as TEXT; the members are the only values allowed
        advance();
        expectSymbol("(");
        col.typeName = "TEXT";
        do {
            if (peek().type != TokenType::String) error("Column '" + col.name + "': ENUM ke andar quotes mein members expected the");
            std::string member = advance().textValue;
            if (std::find(col.enumValues.begin(), col.enumValues.end(), member) != col.enumValues.end())
                error("Column '" + col.name + "': ENUM member " + pyRepr(member) + " do baar diya hai");
            col.enumValues.push_back(std::move(member));
        } while (matchSymbol(","));
        expectSymbol(")");
    } else {
        std::optional<std::string> typeName =
            typeTok.type == TokenType::Ident ? normalizeType(typeTok.textValue) : std::nullopt;
        if (!typeName.has_value()) {
            error("Column '" + col.name + "' ka type expected tha (INT/ANK, FLOAT, TEXT/SHABD, BOOL, DATE/TAREEKH, ...)");  // wording shared with the Python engine
        }
        advance();
        col.typeName = *typeName;
        col.maxLength = parseTypeLength(col.typeName, col.name);
    }

    while (true) {
        if (peek().type == TokenType::Ident && (peek().textValue == "auto_increment" || peek().textValue == "auto_ank")) {
            advance();
            col.autoIncrement = true;
        } else if (matchKeyword("MUKHYA")) {
            expectKeyword("KUNJI");
            col.primaryKey = true;
        } else if (matchKeyword("ZAROORI")) {
            col.notNull = true;
        } else if (matchKeyword("ANOKHA")) {
            col.unique = true;
        } else if (matchKeyword("WARNA")) {
            auto value = parseUnary();  // handles -5 as well as 5
            auto* lit = dynamic_cast<Literal*>(value.get());
            if (!lit) error("WARNA ke baad ek fixed value (jaise 18 ya 'Delhi') expected thi");
            col.defaultValue = lit->value;
        } else if (matchKeyword("SANDARBH")) {
            col.refTable = expectIdent("parent table ka naam");
            expectSymbol("(");
            col.refColumn = expectIdent("parent column ka naam");
            expectSymbol(")");
            parseFkActions(col.onDelete, col.onUpdate);
        } else if (matchKeyword("SHART")) {
            expectSymbol("(");
            size_t start = static_cast<size_t>(peek().start);
            parseOr();  // parse-only, to validate syntax and find the end
            size_t end = static_cast<size_t>(peek().start);  // position of the ")" about to be consumed
            expectSymbol(")");
            std::string raw = sourceText_.substr(start, end - start);
            col.check = pytext::rstrip(raw);  // str.rstrip(): Unicode whitespace, as Python's parser does
        } else {
            break;
        }
    }
    return col;
}

std::optional<int> Parser::parseTypeLength(const std::string& typeName, const std::string& column) {
    if (!matchSymbol("(")) return std::nullopt;
    auto expectLengthNumber = [&]() -> int64_t {
        const Token& t = peek();
        if (t.type != TokenType::Number || t.isFloat) {
            error("Column '" + column + "': type ke baad ek whole number (length) expected tha");
        }
        advance();
        return t.intValue;
    };
    int64_t first = expectLengthNumber();
    if (matchSymbol(",")) expectLengthNumber();
    expectSymbol(")");
    return typeName == "TEXT" ? std::optional<int>(static_cast<int>(first)) : std::nullopt;
}

std::unique_ptr<CreateView> Parser::parseCreateView() {
    auto cv = std::make_unique<CreateView>();
    cv->name = expectIdent("view ka naam");
    expectKeyword("KAHO");
    size_t start = static_cast<size_t>(peek().start);
    if (!checkKeyword("DIKHAO")) {
        error("BANAO VIEW ke baad sirf ek DIKHAO query aa sakti hai");
    }
    advance();  // consume DIKHAO
    auto selectStmt = parseSelectBody();
    if (peek().type == TokenType::Keyword &&
        (peek().textValue == "SANYUKT" || peek().textValue == "SAAJHA" || peek().textValue == "CHHODKAR")) {
        error("BANAO VIEW ke baad sirf ek DIKHAO query aa sakti hai");
    }
    size_t end = static_cast<size_t>(peek(-1).end);
    cv->queryText = sourceText_.substr(start, end - start);
    // ... WITH [LOCAL | CASCADED] CHECK OPTION: rows written through the view must stay visible through it
    if (isSoftWord(0, "with")) {
        advance();
        if (isSoftWord(0, "local") || isSoftWord(0, "cascaded")) advance();
        if (!(isSoftWord(0, "check") && isSoftWord(1, "option"))) error("WITH ke baad CHECK OPTION expected tha");
        advance();
        advance();
        cv->checkOption = true;
    }
    return cv;
}

std::unique_ptr<Statement> Parser::parseHatao() {
    if (matchKeyword("DATABASE")) {
        auto d = std::make_unique<DropDatabase>();
        d->ifExists = matchIfExists();
        d->name = expectIdent("database ka naam");
        return d;
    }
    if (matchKeyword("VIEW")) {
        auto d = std::make_unique<DropView>();
        d->ifExists = matchIfExists();
        d->name = expectIdent("view ka naam");
        return d;
    }
    if (matchKeyword("USER")) {
        auto d = std::make_unique<DropUser>();
        d->ifExists = matchIfExists();
        d->name = expectIdent("user ka naam");
        return d;
    }
    if (isSoftWord(0, "role") && peek(1).type == TokenType::Ident) {
        advance();
        auto d = std::make_unique<DropRole>();
        d->ifExists = matchIfExists();
        d->name = expectIdent("role ka naam");
        return d;
    }
    // HATAO TRIGGER|PROCEDURE|FUNCTION [IF EXISTS] naam
    auto ifExists = [&]() {
        if (isSoftWord(0, "if") && isSoftWord(1, "exists")) {
            advance();
            advance();
            return true;
        }
        return false;
    };
    if (matchKeyword("TRIGGER")) {
        auto d = std::make_unique<DropTrigger>();
        d->ifExists = ifExists();
        d->name = expectIdent("trigger ka naam");
        return d;
    }
    if (matchKeyword("PROCEDURE")) {
        auto d = std::make_unique<DropProcedure>();
        d->ifExists = ifExists();
        d->name = expectIdent("procedure ka naam");
        return d;
    }
    if (isSoftWord(0, "function") && (peek(1).type == TokenType::Ident)) {
        advance();
        auto d = std::make_unique<DropFunction>();
        d->ifExists = ifExists();
        d->name = expectIdent("function ka naam");
        return d;
    }
    if (isSoftWord(0, "prepare") && peek(1).type == TokenType::Ident) {  // HATAO PREPARE name = DEALLOCATE PREPARE name
        advance();
        auto d = std::make_unique<Deallocate>();
        d->name = expectIdent("prepared statement ka naam");
        return d;
    }
    if (isSoftWord(0, "index")) {  // HATAO INDEX [IF EXISTS] naam PAR table
        advance();
        auto d = std::make_unique<DropIndex>();
        d->ifExists = matchIfExists();
        d->name = expectIdent("index ka naam");
        expectKeyword("PAR");
        d->table = expectIdent("table ka naam");
        return d;
    }
    expectKeyword("TABLE");
    auto d = std::make_unique<DropTable>();
    d->ifExists = matchIfExists();
    d->name = expectIdent("table ka naam");
    return d;
}

// SHURU stmt; stmt; ... KHATAM -- captures the RAW SOURCE TEXT between SHURU and
// KHATAM ("store source text, reparse fresh later", like BANAO VIEW), and
// parse-validates every statement NOW. Mirrors Parser._parse_block_body.
std::string Parser::parseBlockBody(const std::string& what) {
    expectKeyword("SHURU");
    size_t start = peek().start < 0 ? 0 : static_cast<size_t>(peek().start);
    // the statements (and DECLARE / IF / loops ...) are only validated here; the text is stored and re-parsed when used
    ProcBlock body = parseProcBlock();
    if (peek().type == TokenType::Eof) error(what + " ka SHURU...KHATAM band nahi hua (KHATAM missing)");
    if (body.empty()) error(what + " ke SHURU...KHATAM ke andar kam se kam ek statement chahiye");
    size_t end = static_cast<size_t>(peek(-1).end);  // end of the last ';'
    std::string bodyText = sourceText_.substr(start, end - start);
    expectKeyword("KHATAM");
    return bodyText;
}

std::unique_ptr<Statement> Parser::parseCreateTrigger() {
    auto t = std::make_unique<CreateTrigger>();
    if (isSoftWord(0, "if") && isSoftWord(1, "not") && isSoftWord(2, "exists")) {  // BANAO TRIGGER IF NOT EXISTS naam ...
        advance();
        advance();
        advance();
        t->ifNotExists = true;
    }
    t->name = expectIdent("trigger ka naam");
    if (matchKeyword("PEHLE")) t->timing = "PEHLE";
    else if (matchKeyword("BAAD")) t->timing = "BAAD";
    else error("BANAO TRIGGER naam ke baad PEHLE ya BAAD expected tha");
    for (const char* kw : {"DAALO", "BADLO", "MITAO"}) {
        if (matchKeyword(kw)) {
            t->event = kw;
            break;
        }
    }
    if (t->event.empty()) error("PEHLE/BAAD ke baad DAALO, BADLO ya MITAO expected tha");
    expectKeyword("PAR");
    t->table = expectIdent("table ka naam");
    t->bodyText = parseBlockBody("TRIGGER");
    return t;
}

// A type name for a parameter / variable / function result; a length in brackets (VARCHAR(20), DECIMAL(10, 2)) is read and ignored.
std::string Parser::parseTypeName(const std::string& what) {
    std::optional<std::string> typeName;
    if (peek().type == TokenType::Ident) typeName = normalizeType(peek().textValue);
    if (!typeName) error(what + " ka type expected tha (INT/ANK, FLOAT, TEXT/SHABD, BOOL, DATE/TAREEKH, ...)");
    advance();
    if (matchSymbol("(")) {
        while (true) {
            if (peek().type != TokenType::Number) error("Type ki length (number) expected thi");
            advance();
            if (!matchSymbol(",")) break;
        }
        expectSymbol(")");
    }
    return *typeName;
}

// [IN | OUT | INOUT] name TYPE. The mode word is only a mode when a name and a type follow it (a parameter may be called `in`).
ProcParam Parser::parseProcParam(bool allowMode) {
    ProcParam param;
    if (allowMode && peek().type == TokenType::Ident && peek(1).type == TokenType::Ident && peek(2).type == TokenType::Ident &&
        (peek().textValue == "in" || peek().textValue == "out" || peek().textValue == "inout")) {
        param.mode = advance().textValue;
        for (auto& c : param.mode) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    }
    param.name = expectIdent("parameter ka naam");
    param.typeName = parseTypeName("Parameter '" + param.name + "'");
    return param;
}

std::unique_ptr<Statement> Parser::parseCreateProcedure() {
    auto p = std::make_unique<CreateProcedure>();
    p->name = expectIdent("procedure ka naam");
    expectSymbol("(");
    if (!checkSymbol(")")) {
        p->params.push_back(parseProcParam(true));
        while (matchSymbol(",")) p->params.push_back(parseProcParam(true));
    }
    expectSymbol(")");
    p->bodyText = parseBlockBody("PROCEDURE");
    return p;
}

// BANAO FUNCTION naam (a INT, b INT) RETURNS INT [[NOT] DETERMINISTIC] SHURU ... RETURN expr; KHATAM   (FUNCTION consumed)
std::unique_ptr<Statement> Parser::parseCreateFunction() {
    auto f = std::make_unique<CreateFunction>();
    f->name = expectIdent("function ka naam");
    expectSymbol("(");
    if (!checkSymbol(")")) {
        f->params.push_back(parseProcParam(false));
        while (matchSymbol(",")) f->params.push_back(parseProcParam(false));
    }
    expectSymbol(")");
    if (!isSoftWord(0, "returns")) error("Function ke parameters ke baad RETURNS TYPE expected tha");
    advance();
    f->returnType = parseTypeName("Function '" + f->name + "' ka RETURNS");
    if (isSoftWord(0, "not") && isSoftWord(1, "deterministic")) advance();
    if (isSoftWord(0, "deterministic")) advance();  // accepted and ignored: every function is simply re-run
    f->bodyText = parseBlockBody("FUNCTION");
    return f;
}

// name = expr [, name = expr ...] for SET / RAKHO. A name is a variable, @user_variable, autocommit or naya.col.
std::vector<std::pair<std::string, std::unique_ptr<Expr>>> Parser::parseVarAssignments() {
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> out;
    do {
        std::string name = expectIdent("variable ka naam");
        if (matchSymbol(".")) name += "." + expectIdent("column ka naam");
        expectSymbol("=");
        std::unique_ptr<Expr> value;
        const Token& t = peek();
        if (name == "autocommit" && t.type == TokenType::Ident &&
            (t.textValue == "on" || t.textValue == "off" || t.textValue == "true" || t.textValue == "false")) {
            bool on = t.textValue == "on" || t.textValue == "true";
            advance();
            value = std::make_unique<Literal>(Value(static_cast<int64_t>(on ? 1 : 0)));
        } else {
            value = parseOr();
        }
        out.emplace_back(std::move(name), std::move(value));
    } while (matchSymbol(","));
    return out;
}

std::unique_ptr<Statement> Parser::parseSet() {
    // SET DEFAULT ROLE ALL | NONE | r [, r] TO user
    if (isSoftWord(0, "default") && isSoftWord(1, "role")) {
        advance();
        advance();
        auto d = std::make_unique<SetDefaultRole>();
        if (matchKeyword("SAB") || (isSoftWord(0, "all") && (advance(), true))) d->all = true;
        else if (isSoftWord(0, "none")) { advance(); d->none = true; }
        else d->roles = parseNameList("role ka naam");
        if (!isSoftWord(0, "to") && !checkKeyword("KO")) error("SET DEFAULT ROLE ... ke baad TO user (ya KO user) expected tha");
        advance();
        d->user = expectIdent("user ka naam");
        return d;
    }
    // SET [SESSION | GLOBAL] TRANSACTION ISOLATION LEVEL <level>
    int skip = (isSoftWord(0, "session") || isSoftWord(0, "global")) ? 1 : 0;
    if (isSoftWord(skip, "transaction") && isSoftWord(skip + 1, "isolation")) {
        for (int k = 0; k < skip + 2; ++k) advance();
        if (!isSoftWord(0, "level")) error("ISOLATION ke baad LEVEL expected tha");
        advance();
        auto iso = std::make_unique<SetIsolation>();
        const std::string first = advance().textValue;
        if (first == "serializable") iso->level = "SERIALIZABLE";
        else if (first == "repeatable" && isSoftWord(0, "read")) { advance(); iso->level = "REPEATABLE READ"; }
        else if (first == "read" && (isSoftWord(0, "committed") || isSoftWord(0, "uncommitted"))) {
            iso->level = isSoftWord(0, "committed") ? "READ COMMITTED" : "READ UNCOMMITTED";
            advance();
        } else error("ISOLATION LEVEL ke baad READ UNCOMMITTED, READ COMMITTED, REPEATABLE READ ya SERIALIZABLE expected tha");
        return iso;
    }
    auto s = std::make_unique<SetVariable>();
    s->assignments = parseVarAssignments();
    return s;
}

std::unique_ptr<Statement> Parser::parsePrepare() {
    auto p = std::make_unique<Prepare>();
    p->name = expectIdent("prepared statement ka naam");
    if (!isSoftWord(0, "from")) error("PREPARE naam ke baad FROM 'sql text' expected tha");
    advance();
    p->source = parseOr();
    return p;
}

std::unique_ptr<Statement> Parser::parseExecute() {
    auto e = std::make_unique<ExecutePrepared>();
    e->name = expectIdent("prepared statement ka naam");
    if (isSoftWord(0, "using")) {
        advance();
        e->args.push_back(parseOr());
        while (matchSymbol(",")) e->args.push_back(parseOr());
    }
    return e;
}

std::string Parser::expectString(const std::string& what) {
    if (peek().type != TokenType::String) error(what + " (quotes ke andar ek string) expected tha");
    return advance().textValue;
}

std::string Parser::expectPrivilege() {
    static const char* const kPrivileges[] = {"DIKHAO", "DAALO", "BADLO", "MITAO"};
    if (peek().type == TokenType::Keyword) {
        for (const char* p : kPrivileges) {
            if (peek().textValue == p) {
                advance();
                return p;
            }
        }
    }
    // the English spellings: SELECT / INSERT / UPDATE / DELETE
    static const std::pair<const char*, const char*> kEnglish[] = {
        {"select", "DIKHAO"}, {"insert", "DAALO"}, {"update", "BADLO"}, {"delete", "MITAO"}};
    if (peek().type == TokenType::Ident) {
        for (const auto& [word, p] : kEnglish) {
            if (peek().textValue == word) {
                advance();
                return p;
            }
        }
    }
    error("Adhikar ka naam expected tha (DIKHAO, DAALO, BADLO, MITAO, ya SAB)");
}

// SAB | ALL [PRIVILEGES] | priv [( col, ... )] { , priv [( col, ... )] } -- priv is DIKHAO / DAALO / BADLO / MITAO or the
// English SELECT / INSERT / UPDATE / DELETE. A column list limits DIKHAO / DAALO / BADLO to those columns.
void Parser::parsePrivilegeList(std::vector<std::string>& privileges, std::map<std::string, std::vector<std::string>>& columns) {
    if (matchKeyword("SAB") || (isSoftWord(0, "all") && (advance(), true))) {
        if (isSoftWord(0, "privileges")) advance();
        privileges = {"DIKHAO", "DAALO", "BADLO", "MITAO"};
        return;
    }
    do {
        std::string priv = expectPrivilege();
        if (std::find(privileges.begin(), privileges.end(), priv) != privileges.end())
            error("Adhikar " + priv + " do baar likha hai");
        privileges.push_back(priv);
        if (checkSymbol("(")) {
            if (priv == "MITAO") error("MITAO (DELETE) ke saath column list nahi lag sakti -- wo poori row hataata hai");
            advance();
            std::vector<std::string> cols;
            cols.push_back(expectIdent("column ka naam"));
            while (matchSymbol(",")) cols.push_back(expectIdent("column ka naam"));
            expectSymbol(")");
            columns[priv] = std::move(cols);
        }
    } while (matchSymbol(","));
}

std::vector<std::string> Parser::parseNameList(const char* what) {
    std::vector<std::string> names;
    names.push_back(expectIdent(what));
    while (matchSymbol(",")) names.push_back(expectIdent(what));
    return names;
}

// [FOR user]
std::optional<std::string> Parser::parseForUser() {
    if (!isSoftWord(0, "for")) return std::nullopt;
    advance();
    return expectIdent("user ka naam");
}

// SHOW TABLES | VIEWS | DATABASES | TRIGGERS | GRANTS [FOR user] | PROCEDURE STATUS | CREATE TABLE t | CREATE VIEW v
//      | COLUMNS FROM t       (SHOW has already been consumed)
std::unique_ptr<Statement> Parser::parseShow() {
    if (matchKeyword("TABLES")) return std::make_unique<ShowTables>();
    if (matchKeyword("VIEWS")) return std::make_unique<ShowViews>();
    if (isSoftWord(0, "index") || isSoftWord(0, "indexes") || isSoftWord(0, "keys")) {  // SHOW INDEX FROM table
        advance();
        if (!matchKeyword("SE") && !(isSoftWord(0, "from") && (advance(), true)) && !(isSoftWord(0, "in") && (advance(), true)))
            error("SHOW INDEX ke baad FROM expected tha");
        auto s = std::make_unique<ShowIndex>();
        s->table = expectIdent("table ka naam");
        return s;
    }
    if (isSoftWord(0, "databases")) { advance(); return std::make_unique<ShowDatabases>(); }
    if (isSoftWord(0, "triggers")) { advance(); return std::make_unique<ShowTriggers>(); }
    if (isSoftWord(0, "procedures")) { advance(); return std::make_unique<ShowProcedures>(); }
    if (matchKeyword("PROCEDURE")) {  // PROCEDURE is a reserved word
        if (!isSoftWord(0, "status")) error("SHOW PROCEDURE ke baad STATUS expected tha");
        advance();
        return std::make_unique<ShowProcedures>();
    }
    if (isSoftWord(0, "variables")) {
        advance();
        auto v = std::make_unique<ShowVariables>();
        if (isSoftWord(0, "like")) {
            advance();
            v->pattern = expectString("LIKE pattern");
            v->hasPattern = true;
        }
        return v;
    }
    if (isSoftWord(0, "function")) {  // SHOW FUNCTION STATUS
        advance();
        if (!isSoftWord(0, "status")) error("SHOW FUNCTION ke baad STATUS expected tha");
        advance();
        return std::make_unique<ShowFunctions>();
    }
    if (isSoftWord(0, "grants")) {
        advance();
        auto g = std::make_unique<ShowGrants>();
        g->user = parseForUser();
        return g;
    }
    if (isSoftWord(0, "create")) {
        advance();
        if (matchKeyword("TABLE")) {
            auto s = std::make_unique<ShowCreateTable>();
            s->name = expectIdent("table ka naam");
            return s;
        }
        if (matchKeyword("VIEW")) {
            auto s = std::make_unique<ShowCreateView>();
            s->name = expectIdent("view ka naam");
            return s;
        }
        error("SHOW CREATE ke baad TABLE ya VIEW expected tha");
    }
    if (isSoftWord(0, "columns") || isSoftWord(0, "fields")) {
        advance();
        if (!matchKeyword("SE") && !(isSoftWord(0, "from") && (advance(), true)) && !(isSoftWord(0, "in") && (advance(), true)))
            error("SHOW COLUMNS ke baad FROM expected tha");
        matchKeyword("TABLE");
        auto d = std::make_unique<Describe>();
        d->table = expectIdent("table ka naam");
        return d;
    }
    error("SHOW ke baad TABLES, VIEWS, DATABASES, TRIGGERS, GRANTS, PROCEDURE STATUS, FUNCTION STATUS, VARIABLES, CREATE TABLE/VIEW ya COLUMNS FROM expected tha");
}

// SET ROLE r [, r ...] | SAB | ALL | NONE   (SET ROLE has already been consumed)
std::unique_ptr<Statement> Parser::parseSetRole() {
    auto s = std::make_unique<SetRole>();
    if (matchKeyword("SAB") || (isSoftWord(0, "all") && (advance(), true))) s->all = true;
    else if (isSoftWord(0, "none")) { advance(); s->none = true; }
    else s->roles = parseNameList("role ka naam");
    return s;
}

std::unique_ptr<Statement> Parser::parseAdhikar() {
    // ADHIKAR DO priv, priv PAR table KO user      -- GRANT
    // ADHIKAR WAPAS priv, priv PAR table SE user    -- REVOKE
    // ADHIKAR DO ROLE r [, r] KO user            -- give roles to a user (the word ROLE is required)
    // ADHIKAR DO DIKHAO (a, b) PAR t KO user|role [WITH GRANT OPTION]
    if (matchKeyword("DO")) {
        if (isSoftWord(0, "role") && peek(1).type == TokenType::Ident) {
            advance();
            auto g = std::make_unique<GrantRole>();
            g->roles = parseNameList("role ka naam");
            expectKeyword("KO");
            g->user = expectIdent("user ka naam");
            return g;
        }
        auto g = std::make_unique<Grant>();
        parsePrivilegeList(g->privileges, g->columns);
        expectKeyword("PAR");
        g->table = expectIdent("table/view ka naam");
        expectKeyword("KO");
        g->user = expectIdent("user ya role ka naam");
        if (isSoftWord(0, "with") && isSoftWord(1, "grant") && isSoftWord(2, "option")) {
            advance();
            advance();
            advance();
            g->withGrantOption = true;
        }
        return g;
    }
    if (matchKeyword("WAPAS")) {
        if (isSoftWord(0, "role") && peek(1).type == TokenType::Ident) {
            advance();
            auto r = std::make_unique<RevokeRole>();
            r->roles = parseNameList("role ka naam");
            expectKeyword("SE");
            r->user = expectIdent("user ka naam");
            return r;
        }
        auto r = std::make_unique<Revoke>();
        parsePrivilegeList(r->privileges, r->columns);
        expectKeyword("PAR");
        r->table = expectIdent("table/view ka naam");
        expectKeyword("SE");
        r->user = expectIdent("user ya role ka naam");
        return r;
    }
    error("ADHIKAR ke baad DO (grant) ya WAPAS (revoke) expected tha");
}

std::unique_ptr<Statement> Parser::parseChalao() {
    // CHALAO naam(expr, expr, ...)
    auto call = std::make_unique<CallProcedure>();
    call->name = expectIdent("procedure ka naam");
    expectSymbol("(");
    if (!checkSymbol(")")) {
        call->args.push_back(parseExpressionEntry());
        while (matchSymbol(",")) call->args.push_back(parseExpressionEntry());
    }
    expectSymbol(")");
    return call;
}

std::unique_ptr<Statement> Parser::parseSudharo() {
    // SUDHARO TABLE name JODO [COLUMN] coldef
    // SUDHARO TABLE name HATAO [COLUMN] colname
    // SUDHARO TABLE name NAYA_NAAM new_name
    // SUDHARO TABLE name COLUMN old_name NAYA_NAAM new_name
    // SUDHARO VIEW name KAHO DIKHAO ...      (ALTER VIEW: the view must exist)
    if (matchKeyword("VIEW")) {
        auto cv = parseCreateView();
        cv->alter = true;
        return cv;
    }
    expectKeyword("TABLE");
    std::string table = expectIdent("table ka naam");
    if (matchKeyword("JODO")) {
        // JODO [CONSTRAINT name] FOREIGN KEY (col) SANDARBH parent(col) [ON DELETE ..] [ON UPDATE ..]
        if (atForeignKeyItem(0)) {
            auto a = std::make_unique<AlterAddForeignKey>();
            a->table = table;
            a->foreignKey = parseForeignKeyDef();
            return a;
        }
        if (matchKeyword("ANOKHA")) {
            auto a = std::make_unique<AlterAddComposite>();
            a->table = table;
            a->kind = "ANOKHA";
            a->columns = parseCompositeColumns();
            return a;
        }
        if (checkKeyword("MUKHYA") && peek(1).type == TokenType::Keyword && peek(1).textValue == "KUNJI") {
            advance();
            advance();
            auto a = std::make_unique<AlterAddComposite>();
            a->table = table;
            a->kind = "MUKHYA";
            a->columns = parseCompositeColumns();
            return a;
        }
        matchKeyword("COLUMN");
        auto a = std::make_unique<AlterAddColumn>();
        a->table = table;
        a->column = parseColumnDef();
        // JODO col type ... FIRST  |  AFTER other_col
        if (isSoftWord(0, "first")) {
            advance();
            a->position = "FIRST";
        } else if (isSoftWord(0, "after")) {
            advance();
            a->position = "AFTER";
            a->afterColumn = expectIdent("column ka naam");
        }
        return a;
    }
    // CHANGE old coldef (rename + retype)  |  MODIFY coldef (retype)
    if (isSoftWord(0, "change") || isSoftWord(0, "modify")) {
        auto m = std::make_unique<AlterModifyColumn>();
        m->table = table;
        m->rename = advance().textValue == "change";
        matchKeyword("COLUMN");
        if (m->rename) m->oldName = expectIdent("purane column ka naam");
        m->column = parseColumnDef();
        if (!m->rename) m->oldName = m->column.name;
        return m;
    }
    if (matchKeyword("HATAO")) {
        // HATAO FOREIGN KEY name  |  HATAO CONSTRAINT name
        if (isSoftWord(0, "foreign") && isSoftWord(1, "key")) {
            advance();
            advance();
            auto d = std::make_unique<AlterDropForeignKey>();
            d->table = table;
            d->name = expectIdent("foreign key ka naam");
            return d;
        }
        if (isSoftWord(0, "constraint") && peek(1).type == TokenType::Ident) {
            advance();
            auto d = std::make_unique<AlterDropForeignKey>();
            d->table = table;
            d->name = expectIdent("constraint ka naam");
            return d;
        }
        matchKeyword("COLUMN");
        auto a = std::make_unique<AlterDropColumn>();
        a->table = table;
        a->column = expectIdent("column ka naam");
        return a;
    }
    if (matchKeyword("NAYA_NAAM")) {
        auto r = std::make_unique<RenameTable>();
        r->table = table;
        r->newName = expectIdent("naya table naam");
        return r;
    }
    if (matchKeyword("COLUMN")) {
        auto r = std::make_unique<RenameColumn>();
        r->table = table;
        r->column = expectIdent("column ka naam");
        expectKeyword("NAYA_NAAM");
        r->newName = expectIdent("naya column naam");
        return r;
    }
    error("SUDHARO TABLE ke baad JODO, HATAO, NAYA_NAAM ya COLUMN expected tha");  // wording shared with the Python engine
}

std::unique_ptr<Statement> Parser::parseSaaf() {
    expectKeyword("TABLE");
    auto t = std::make_unique<TruncateTable>();
    t->name = expectIdent("table ka naam");
    return t;
}

std::unique_ptr<Statement> Parser::parseSikodo() {
    expectKeyword("TABLE");
    auto t = std::make_unique<CompactTable>();
    t->name = expectIdent("table ka naam");
    return t;
}

std::unique_ptr<Statement> Parser::parseIstemal() {
    matchKeyword("DATABASE");  // optional: ISTEMAL DATABASE college
    auto u = std::make_unique<UseDatabase>();
    u->name = expectIdent("database ka naam");
    return u;
}

std::unique_ptr<Statement> Parser::parseBatao() {
    matchKeyword("TABLE");  // optional: BATAO TABLE students
    auto d = std::make_unique<Describe>();
    d->table = expectIdent("table ka naam");
    return d;
}

// ---- DML ----

std::unique_ptr<Statement> Parser::parseInsert(bool replace, bool intoAlreadyRead) {
    // DAALO MEIN table [(col, col)] MAAN (expr, expr), (expr, expr) ...
    //                               | DIKHAO ...                     -- INSERT ... SELECT
    //      [TAKRAAV PAR BADLO col = expr, ...]                       -- simplified upsert
    // REPLACE INTO table ... / BADAL DAALO MEIN table ...            -- same, but replace = true
    if (!intoAlreadyRead) expectKeyword("MEIN");
    auto ins = std::make_unique<Insert>();
    ins->replace = replace;
    ins->table = expectIdent("table ka naam");

    if (matchSymbol("(")) {
        std::vector<std::string> cols;
        cols.push_back(expectIdent("column ka naam"));
        while (matchSymbol(",")) cols.push_back(expectIdent("column ka naam"));
        expectSymbol(")");
        ins->columns = std::move(cols);
    }

    if (matchKeyword("MAAN")) {
        ins->rows.push_back(parseValueTuple());
        while (matchSymbol(",")) ins->rows.push_back(parseValueTuple());
    } else if (matchKeyword("DIKHAO")) {
        ins->select = parseSelectBody();
    } else {
        error("DAALO ke baad MAAN ya DIKHAO expected tha");
    }

    if (replace && checkKeyword("TAKRAAV")) error("REPLACE ke saath TAKRAAV PAR BADLO nahi chalta");
    if (matchKeyword("TAKRAAV")) {
        expectKeyword("PAR");
        expectKeyword("BADLO");
        ins->onConflictUpdate = parseAssignmentList();
    }
    return ins;
}

std::vector<std::unique_ptr<Expr>> Parser::parseValueTuple() {
    expectSymbol("(");
    std::vector<std::unique_ptr<Expr>> values;
    values.push_back(parseOr());
    while (matchSymbol(",")) values.push_back(parseOr());
    expectSymbol(")");
    return values;
}

static bool isSetOpKeyword(const Token& t) {
    return t.type == TokenType::Keyword &&
           (t.textValue == "SANYUKT" || t.textValue == "SAAJHA" || t.textValue == "CHHODKAR");
}

std::unique_ptr<Statement> Parser::parseDikhaoStmt() {
    // DIKHAO already consumed. DIKHAO TABLES / VIEWS, or a full select
    // optionally chained left-associatively with SANYUKT/SAAJHA/CHHODKAR:
    // `a SANYUKT b SANYUKT c` -> SetOp(SetOp(a, b), c).
    if (matchKeyword("TABLES")) return std::make_unique<ShowTables>();
    if (matchKeyword("VIEWS")) return std::make_unique<ShowViews>();
    // DIKHAO GRANTS [FOR user] / TRIGGERS / PROCEDURES / DATABASES: soft words, taken only when nothing but the end of the
    // statement follows (so `DIKHAO grants SE t` still selects a column called grants)
    {
        auto atEndOfStatement = [&](int offset) {
            return peek(offset).type == TokenType::Eof || (peek(offset).type == TokenType::Symbol && peek(offset).textValue == ";");
        };
        if (isSoftWord(0, "grants") && (atEndOfStatement(1) || isSoftWord(1, "for"))) {
            advance();
            auto g = std::make_unique<ShowGrants>();
            g->user = parseForUser();
            return g;
        }
        if (isSoftWord(0, "triggers") && atEndOfStatement(1)) { advance(); return std::make_unique<ShowTriggers>(); }
        if (isSoftWord(0, "procedures") && atEndOfStatement(1)) { advance(); return std::make_unique<ShowProcedures>(); }
        if (isSoftWord(0, "databases") && atEndOfStatement(1)) { advance(); return std::make_unique<ShowDatabases>(); }
    }

    std::unique_ptr<Statement> result = parseSelectBody();
    while (isSetOpKeyword(peek())) {
        auto op = std::make_unique<SetOp>();
        op->op = advance().textValue;
        spend();
        expectKeyword("DIKHAO");
        op->left = std::move(result);
        op->right = parseSelectBody();
        result = std::move(op);
    }
    return result;
}

std::unique_ptr<Select> Parser::parseSelectBody() {
    // select body := [ALAG] cols SE table [alias] { join } [JAHAN expr]
    //                [SAMOOH expr, ...] [JINKA expr] [KRAM expr [ULTA|SEEDHA], ...] [SIRF n]
    // (assumes the leading DIKHAO keyword was already consumed by the caller)
    auto sel = std::make_unique<Select>();
    sel->distinct = matchKeyword("ALAG");
    do {
        auto item = parseSelectItem();
        sel->columns.push_back(std::move(item.first));
        sel->aliases.push_back(std::move(item.second));
    } while (matchSymbol(","));

    // DIKHAO NOW(), 1 + 1;  -- no SE: one row computed from the expressions alone
    bool noFrom = peek().type == TokenType::Eof || checkSymbol(";") || checkSymbol(")") ||
                  (peek().type == TokenType::Keyword &&
                   (isSetOpKeyword(peek()) || peek().textValue == "JAHAN" || peek().textValue == "SAMOOH" ||
                    peek().textValue == "JINKA" || peek().textValue == "KRAM" || peek().textValue == "SIRF")) ||
                  (isSoftWord(0, "limit") && peek(1).type == TokenType::Number);
    if (!noFrom) expectKeyword("SE");
    // a source is a table/view name, or a derived table: (DIKHAO ...) alias
    auto parseSource = [&](std::string& table, std::optional<std::string>& alias, std::unique_ptr<Select>& derived) {
        if (matchSymbol("(")) {
            guardStack();
            spend();
            expectKeyword("DIKHAO");
            derived = parseSelectBody();
            expectSymbol(")");
            if (!matchKeyword("KAHO") && peek().type == TokenType::Ident && peek().textValue == "as") advance();
            alias = parseAliasOpt();
            if (!alias) error("Subquery (derived table) ko alias chahiye, jaise (DIKHAO ...) t");
        } else {
            table = expectIdent("table ka naam");
            alias = parseAliasOpt();
        }
    };
    if (!noFrom) parseSource(sel->table, sel->alias, sel->derived);

    while (!noFrom) {
        // exactly one of BAAYAN/DAHINA/DONO/SAMAAN, or a plain MILAO (INNER)
        std::string kind;
        if (matchKeyword("BAAYAN")) { expectKeyword("MILAO"); kind = "LEFT"; }
        else if (matchKeyword("DAHINA")) { expectKeyword("MILAO"); kind = "RIGHT"; }
        else if (matchKeyword("DONO")) { expectKeyword("MILAO"); kind = "FULL"; }
        else if (matchKeyword("SAMAAN")) { expectKeyword("MILAO"); kind = "NATURAL"; }
        else if (matchKeyword("MILAO")) { kind = "INNER"; }
        else break;
        Join j;
        j.kind = kind;
        std::optional<std::string> alias;
        parseSource(j.table, alias, j.derived);
        j.alias = alias ? *alias : j.table;
        if (kind != "NATURAL") {
            // NATURAL has no PAR at all -- the planner synthesizes the ON
            // condition from the shared column names. USING (a, b) is the
            // same idea for the listed columns.
            if (peek().type == TokenType::Ident && peek().textValue == "using") {
                advance();
                expectSymbol("(");
                j.usingColumns.push_back(expectIdent("column ka naam"));
                while (matchSymbol(",")) j.usingColumns.push_back(expectIdent("column ka naam"));
                expectSymbol(")");
            } else {
                expectKeyword("PAR");
                j.on = parseOr();
            }
        }
        sel->joins.push_back(std::move(j));
    }

    if (matchKeyword("JAHAN")) sel->where = parseOr();
    if (matchKeyword("SAMOOH")) {
        sel->groupBy.push_back(parseOr());
        while (matchSymbol(",")) sel->groupBy.push_back(parseOr());
    }
    if (matchKeyword("JINKA")) sel->having = parseOr();
    if (matchKeyword("KRAM")) {
        sel->orderBy.push_back(parseOrderItem());
        while (matchSymbol(",")) sel->orderBy.push_back(parseOrderItem());
    }
    // LIMIT is the English spelling of SIRF; a soft word, taken only when a number follows
    bool limitWord = isSoftWord(0, "limit") && peek(1).type == TokenType::Number;
    if (limitWord) advance();
    if (limitWord || matchKeyword("SIRF")) {
        // SIRF n  |  SIRF n OFFSET m  |  SIRF m, n  (the MySQL form: skip m, keep n)
        auto wholeNumber = [&](const char* what) {
            const Token& t = peek();
            if (t.type != TokenType::Number || t.isFloat) error(std::string(what) + " ke baad ek whole number expected tha");
            advance();
            return static_cast<int>(t.intValue);
        };
        int first = wholeNumber("SIRF");
        if (matchSymbol(",")) {
            sel->offset = first;
            sel->limit = wholeNumber(",");
        } else {
            sel->limit = first;
            if (peek().type == TokenType::Ident && peek().textValue == "offset") {
                advance();
                sel->offset = wholeNumber("OFFSET");
            }
        }
    }
    // FOR UPDATE [NOWAIT | SKIP LOCKED] | FOR SHARE | LOCK IN SHARE MODE
    if (isSoftWord(0, "for") && (isSoftWord(1, "update") || isSoftWord(1, "share"))) {
        sel->lockMode = peek(1).textValue == "update" ? "UPDATE" : "SHARE";
        advance();
        advance();
        if (isSoftWord(0, "nowait")) advance();
        else if (isSoftWord(0, "skip") && isSoftWord(1, "locked")) {
            advance();
            advance();
        }
    } else if (isSoftWord(0, "lock") && isSoftWord(1, "in") && isSoftWord(2, "share") && isSoftWord(3, "mode")) {
        sel->lockMode = "SHARE";
        for (int k = 0; k < 4; ++k) advance();
    }
    return sel;
}

std::pair<std::unique_ptr<Expr>, std::optional<std::string>> Parser::parseSelectItem() {
    // item = "*" | IDENT "." "*" | expr [ "KAHO" IDENT ]
    // `*` can't be parsed as an expression (it would look like multiplication)
    std::unique_ptr<Expr> expr;
    if (matchSymbol("*")) expr = std::make_unique<Star>();
    else expr = parseOr();
    if (dynamic_cast<Star*>(expr.get())) {  // bare `*` or `alias.*`
        if (checkKeyword("KAHO")) error("'*' ke baad KAHO (alias) nahi laga sakte");
        return {std::move(expr), std::nullopt};
    }
    if (matchKeyword("KAHO")) return {std::move(expr), expectIdent("alias ka naam")};
    return {std::move(expr), std::nullopt};
}

std::optional<std::string> Parser::parseAliasOpt() {
    // `SE students s` -- an IDENT right after the table name is its alias.
    // Keywords (JAHAN, MILAO, ...) are TokenType::Keyword, never taken here.
    if (peek().type == TokenType::Ident) {
        // MILAO b USING (id): USING is the join clause, not an alias
        if (peek().textValue == "using" && peek(1).type == TokenType::Symbol && peek(1).textValue == "(")
            return std::nullopt;
        if (peek().textValue == "lock" && isSoftWord(1, "in") && isSoftWord(2, "share")) return std::nullopt;  // LOCK IN SHARE MODE
        // SE t LIMIT 5: LIMIT is the row limit, not an alias
        if (peek().textValue == "limit" && peek(1).type == TokenType::Number) return std::nullopt;
        // BANAO VIEW v KAHO DIKHAO * SE t WITH CHECK OPTION: WITH belongs to the view, not to the table
        if (peek().textValue == "with" && (isSoftWord(1, "check") || isSoftWord(1, "local") || isSoftWord(1, "cascaded")))
            return std::nullopt;
        return advance().textValue;
    }
    return std::nullopt;
}

OrderItem Parser::parseOrderItem() {
    OrderItem oi;
    oi.expr = parseOr();
    if (matchKeyword("ULTA")) {
        oi.descending = true;
    } else {
        matchKeyword("SEEDHA");
        oi.descending = false;
    }
    return oi;
}

std::unique_ptr<Statement> Parser::parseUpdate() {
    // BADLO table RAKHO col = expr, col = expr [JAHAN expr]
    auto upd = std::make_unique<Update>();
    upd->table = expectIdent("table ka naam");
    expectKeyword("RAKHO");
    upd->assignments = parseAssignmentList();
    if (matchKeyword("JAHAN")) upd->where = parseOr();
    return upd;
}

std::vector<std::pair<std::string, std::unique_ptr<Expr>>> Parser::parseAssignmentList() {
    // shared by BADLO ... RAKHO and DAALO ... TAKRAAV PAR BADLO
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> out;
    out.push_back(parseAssignment());
    while (matchSymbol(",")) out.push_back(parseAssignment());
    return out;
}

std::pair<std::string, std::unique_ptr<Expr>> Parser::parseAssignment() {
    std::string col = expectIdent("column ka naam");
    expectSymbol("=");
    return {col, parseOr()};
}

std::unique_ptr<Statement> Parser::parseDelete() {
    // MITAO SE table [JAHAN expr]
    expectKeyword("SE");
    auto del = std::make_unique<Delete>();
    del->table = expectIdent("table ka naam");
    if (matchKeyword("JAHAN")) del->where = parseOr();
    return del;
}

std::vector<std::unique_ptr<Statement>> parseScript(const std::string& text) {
    Parser p(tokenize(text), text);
    return p.parseScript();
}

std::unique_ptr<Expr> parseExpression(const std::string& text) {
    Parser p(tokenize(text), text);
    auto e = p.parseExpressionEntry();
    if (!p.atEnd()) throw ParseError("SHART expression adhoori parse hui: '" + text + "'");
    return e;
}

}  // namespace meradb
