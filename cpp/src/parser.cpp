// cpp/src/parser.cpp
#include "meradb/parser.h"
#include "meradb/ast_util.h"
#include "meradb/datatypes.h"
#include "meradb/errors.h"
#include "meradb/pytext.h"
#include "meradb/stack_guard.h"
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
        if (matchSymbol(op)) return std::make_unique<BinaryOp>(op, std::move(left), parseAdditive());
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
    while (checkSymbol("+") || checkSymbol("-")) {
        std::string op = advance().textValue;
        spend();
        left = std::make_unique<BinaryOp>(op, std::move(left), parseTerm());
    }
    return left;
}
std::unique_ptr<Expr> Parser::parseTerm() {
    auto left = parseUnary();
    while (checkSymbol("*") || checkSymbol("/") || checkSymbol("%")) {
        std::string op = advance().textValue;
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
    if (kw == "DAALO") { advance(); return parseInsert(); }
    if (kw == "DIKHAO") { advance(); return parseDikhaoStmt(); }
    if (kw == "BADLO") { advance(); return parseUpdate(); }
    if (kw == "MITAO") { advance(); return parseDelete(); }
    if (kw == "SHURU") { advance(); return std::make_unique<Begin>(); }
    if (kw == "PAKKA") { advance(); return std::make_unique<Commit>(); }
    if (kw == "WAPAS") { advance(); return std::make_unique<Rollback>(); }
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

std::unique_ptr<Statement> Parser::parseBanao() {
    // BANAO DATABASE name
    if (matchKeyword("DATABASE")) {
        auto d = std::make_unique<CreateDatabase>();
        d->name = expectIdent("database ka naam");
        return d;
    }
    // BANAO VIEW naam KAHO DIKHAO ...
    if (matchKeyword("VIEW")) return parseCreateView();

    // BANAO USER naam GUPT 'password'
    if (matchKeyword("USER")) {
        auto u = std::make_unique<CreateUser>();
        u->name = expectIdent("user ka naam");
        expectKeyword("GUPT");
        u->password = expectString("password");
        return u;
    }
    // BANAO TRIGGER naam PEHLE|BAAD DAALO|BADLO|MITAO PAR table SHURU ... KHATAM
    if (matchKeyword("TRIGGER")) return parseCreateTrigger();
    // BANAO PROCEDURE naam (p1 TYPE, ...) SHURU ... KHATAM
    if (matchKeyword("PROCEDURE")) return parseCreateProcedure();

    // BANAO TABLE name ( coldef | table-constraint, ... )
    expectKeyword("TABLE");
    auto stmt = std::make_unique<CreateTable>();
    stmt->name = expectIdent("table ka naam");
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

std::vector<std::string> Parser::parseCompositeColumns() {
    expectSymbol("(");
    std::vector<std::string> cols;
    cols.push_back(expectIdent("column ka naam"));
    while (matchSymbol(",")) cols.push_back(expectIdent("column ka naam"));
    expectSymbol(")");
    if (cols.size() < 2) {
        error("Composite constraint mein kam se kam 2 columns chahiye (1 column ke liye normal ANOKHA/MUKHYA KUNJI use karo)");
    }
    return cols;
}

ColumnDef Parser::parseColumnDef() {
    // coldef := name TYPE [ "(" INTEGER [ "," INTEGER ] ")" ]
    //           [MUKHYA KUNJI | ZAROORI | ANOKHA | WARNA literal | SANDARBH ref | SHART "(" expr ")"]*
    ColumnDef col;
    col.name = expectIdent("column ka naam");
    const Token& typeTok = peek();
    std::optional<std::string> typeName =
        typeTok.type == TokenType::Ident ? normalizeType(typeTok.textValue) : std::nullopt;
    if (!typeName.has_value()) {
        error("Column '" + col.name + "' ka type expected tha (INT/ANK, FLOAT, TEXT/SHABD, BOOL, DATE/TAREEKH, ...)");
    }
    advance();
    col.typeName = *typeName;
    col.maxLength = parseTypeLength(col.typeName, col.name);

    while (true) {
        if (matchKeyword("MUKHYA")) {
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
    return cv;
}

std::unique_ptr<Statement> Parser::parseHatao() {
    if (matchKeyword("DATABASE")) {
        auto d = std::make_unique<DropDatabase>();
        d->name = expectIdent("database ka naam");
        return d;
    }
    if (matchKeyword("VIEW")) {
        auto d = std::make_unique<DropView>();
        d->name = expectIdent("view ka naam");
        return d;
    }
    if (matchKeyword("USER")) {
        auto d = std::make_unique<DropUser>();
        d->name = expectIdent("user ka naam");
        return d;
    }
    if (matchKeyword("TRIGGER")) {
        auto d = std::make_unique<DropTrigger>();
        d->name = expectIdent("trigger ka naam");
        return d;
    }
    if (matchKeyword("PROCEDURE")) {
        auto d = std::make_unique<DropProcedure>();
        d->name = expectIdent("procedure ka naam");
        return d;
    }
    expectKeyword("TABLE");
    auto d = std::make_unique<DropTable>();
    d->name = expectIdent("table ka naam");
    return d;
}

// SHURU stmt; stmt; ... KHATAM -- captures the RAW SOURCE TEXT between SHURU and
// KHATAM ("store source text, reparse fresh later", like BANAO VIEW), and
// parse-validates every statement NOW. Mirrors Parser._parse_block_body.
std::string Parser::parseBlockBody(const std::string& what) {
    expectKeyword("SHURU");
    size_t start = peek().start < 0 ? 0 : static_cast<size_t>(peek().start);
    size_t statementCount = 0;
    while (!checkKeyword("KHATAM")) {
        if (peek().type == TokenType::Eof) error(what + " ka SHURU...KHATAM band nahi hua (KHATAM missing)");
        auto body = parseStatement();
        (void)body;  // only validated here; the text is stored and re-parsed when used
        ++statementCount;
        expectSymbol(";");
    }
    if (statementCount == 0) error(what + " ke SHURU...KHATAM ke andar kam se kam ek statement chahiye");
    size_t end = static_cast<size_t>(peek(-1).end);  // end of the last ';'
    std::string bodyText = sourceText_.substr(start, end - start);
    expectKeyword("KHATAM");
    return bodyText;
}

std::unique_ptr<Statement> Parser::parseCreateTrigger() {
    auto t = std::make_unique<CreateTrigger>();
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

ProcParam Parser::parseProcParam() {
    ProcParam param;
    param.name = expectIdent("parameter ka naam");
    std::optional<std::string> typeName;
    if (peek().type == TokenType::Ident) typeName = normalizeType(peek().textValue);
    if (!typeName) {
        error("Parameter '" + param.name + "' ka type expected tha (INT/ANK, FLOAT, TEXT/SHABD, BOOL, DATE/TAREEKH, ...)");
    }
    advance();
    param.typeName = *typeName;
    return param;
}

std::unique_ptr<Statement> Parser::parseCreateProcedure() {
    auto p = std::make_unique<CreateProcedure>();
    p->name = expectIdent("procedure ka naam");
    expectSymbol("(");
    if (!checkSymbol(")")) {
        p->params.push_back(parseProcParam());
        while (matchSymbol(",")) p->params.push_back(parseProcParam());
    }
    expectSymbol(")");
    p->bodyText = parseBlockBody("PROCEDURE");
    return p;
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
    error("Adhikar ka naam expected tha (DIKHAO, DAALO, BADLO, MITAO, ya SAB)");
}

std::vector<std::string> Parser::parsePrivilegeList() {
    if (matchKeyword("SAB")) return {"DIKHAO", "DAALO", "BADLO", "MITAO"};
    std::vector<std::string> privileges;
    privileges.push_back(expectPrivilege());
    while (matchSymbol(",")) privileges.push_back(expectPrivilege());
    return privileges;
}

std::unique_ptr<Statement> Parser::parseAdhikar() {
    // ADHIKAR DO priv, priv PAR table KO user      -- GRANT
    // ADHIKAR WAPAS priv, priv PAR table SE user    -- REVOKE
    if (matchKeyword("DO")) {
        auto g = std::make_unique<Grant>();
        g->privileges = parsePrivilegeList();
        expectKeyword("PAR");
        g->table = expectIdent("table/view ka naam");
        expectKeyword("KO");
        g->user = expectIdent("user ka naam");
        return g;
    }
    if (matchKeyword("WAPAS")) {
        auto r = std::make_unique<Revoke>();
        r->privileges = parsePrivilegeList();
        expectKeyword("PAR");
        r->table = expectIdent("table/view ka naam");
        expectKeyword("SE");
        r->user = expectIdent("user ka naam");
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
    expectKeyword("TABLE");
    std::string table = expectIdent("table ka naam");
    if (matchKeyword("JODO")) {
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
        return a;
    }
    if (matchKeyword("HATAO")) {
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
    error("SUDHARO TABLE ke baad JODO, HATAO, NAYA_NAAM ya COLUMN expected tha");
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

std::unique_ptr<Statement> Parser::parseInsert() {
    // DAALO MEIN table [(col, col)] MAAN (expr, expr), (expr, expr) ...
    //                               | DIKHAO ...                     -- INSERT ... SELECT
    //      [TAKRAAV PAR BADLO col = expr, ...]                       -- simplified upsert
    expectKeyword("MEIN");
    auto ins = std::make_unique<Insert>();
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

    expectKeyword("SE");
    sel->table = expectIdent("table ka naam");
    sel->alias = parseAliasOpt();

    while (true) {
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
        j.table = expectIdent("table ka naam");
        auto alias = parseAliasOpt();
        j.alias = alias ? *alias : j.table;
        if (kind != "NATURAL") {
            // NATURAL has no PAR at all -- the planner synthesizes the ON
            // condition from the shared column names.
            expectKeyword("PAR");
            j.on = parseOr();
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
    if (matchKeyword("SIRF")) {
        const Token& t = peek();
        if (t.type != TokenType::Number || t.isFloat) error("SIRF ke baad ek whole number expected tha");
        advance();
        sel->limit = static_cast<int>(t.intValue);
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
    if (peek().type == TokenType::Ident) return advance().textValue;
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
