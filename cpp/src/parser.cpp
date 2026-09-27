// cpp/src/parser.cpp
#include "meradb/parser.h"
#include "meradb/ast_util.h"
#include "meradb/datatypes.h"
#include "meradb/errors.h"
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
[[noreturn]] void Parser::error(const std::string& msg) const {
    throw ParseError(msg + " (line " + std::to_string(peek().line) + ")");
}

// ---------------------------------------------------------------------
// Expressions (precedence, lowest to highest): parseOr -> parseAnd ->
// parseNot -> parseComparison -> parseAdditive -> parseTerm -> parseUnary
// -> parsePrimary. Mirrors meradb/parser.py's _parse_or.._parse_primary
// exactly.
// ---------------------------------------------------------------------

std::unique_ptr<Expr> Parser::parseOr() {
    auto left = parseAnd();
    while (matchKeyword("YA")) left = std::make_unique<BinaryOp>("YA", std::move(left), parseAnd());
    return left;
}
std::unique_ptr<Expr> Parser::parseAnd() {
    auto left = parseNot();
    while (matchKeyword("AUR")) left = std::make_unique<BinaryOp>("AUR", std::move(left), parseNot());
    return left;
}
std::unique_ptr<Expr> Parser::parseNot() {
    if (matchKeyword("NAHI")) {
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
        if (checkKeyword("DIKHAO")) {
            auto sub = std::make_unique<Subquery>();
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
        left = std::make_unique<BinaryOp>(op, std::move(left), parseTerm());
    }
    return left;
}
std::unique_ptr<Expr> Parser::parseTerm() {
    auto left = parseUnary();
    while (checkSymbol("*") || checkSymbol("/") || checkSymbol("%")) {
        std::string op = advance().textValue;
        left = std::make_unique<BinaryOp>(op, std::move(left), parseUnary());
    }
    return left;
}
std::unique_ptr<Expr> Parser::parseUnary() {
    if (checkSymbol("-")) {
        advance();
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
    if (matchKeyword("AGAR")) return parseCase();
    if (t.type == TokenType::Ident) {
        std::string name = t.textValue;
        advance();
        if (matchSymbol("(")) {
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
        if (checkKeyword("DIKHAO")) {
            auto sub = std::make_unique<Subquery>();
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

std::unique_ptr<Expr> Parser::parseExpressionEntry() { return parseOr(); }

// ---------------------------------------------------------------------
// Statement-level parsing (Task 7/8). Stubbed here so Task 6 links; Task
// 7/8 replace these bodies.
// ---------------------------------------------------------------------
std::unique_ptr<Select> Parser::parseSelectBody() {
    throw ParseError("SELECT parsing not implemented yet");
}
std::unique_ptr<Statement> Parser::parseStatement() {
    throw ParseError("Statement parsing not implemented yet");
}
std::vector<std::unique_ptr<Statement>> Parser::parseScript() {
    throw ParseError("Script parsing not implemented yet");
}

std::vector<std::unique_ptr<Statement>> parseScript(const std::string& text) {
    Parser p(tokenize(text), text);
    return p.parseScript();
}

std::unique_ptr<Expr> parseExpression(const std::string& text) {
    Parser p(tokenize(text), text);
    auto e = p.parseExpressionEntry();
    return e;
}

}  // namespace meradb
