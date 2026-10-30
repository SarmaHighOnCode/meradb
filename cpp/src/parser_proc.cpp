// cpp/src/parser_proc.cpp -- the BODY language of triggers, procedures and functions (see proc_ast.h):
//   DECLARE, SET / RAKHO, IF / AGAR, WHILE / JAB TAK, LOOP, REPEAT, LEAVE, ITERATE, RETURN, SIGNAL, SELECT .. INTO,
//   cursors (OPEN / FETCH / CLOSE) and every ordinary statement (kept as its source text).
// All of it lives in Parser (parser.h); this file only keeps parser.cpp readable.
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/stack_guard.h"
#include <algorithm>
#include <cctype>

namespace meradb {

using namespace ast;

namespace {
std::string upperOf(std::string s) {
    for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

// A word that is either a soft word (an IDENT, lower-case) or the reserved keyword spelled the same way (AGAR, DO, WARNA ...).
bool Parser::isWord(int offset, const char* word) const {
    const Token& t = peek(offset);
    if (t.type == TokenType::Ident) return t.textValue == word;
    if (t.type == TokenType::Keyword) return t.textValue == upperOf(word);
    return false;
}

bool Parser::matchWord(const char* word) {
    if (isWord(0, word)) {
        advance();
        return true;
    }
    return false;
}

// What ends a list of statements: the KHATAM that closes the body or an IF / loop, an IF's WARNA / ELSE / ELSEIF, END, UNTIL.
bool Parser::atBlockEnd() const {
    const Token& t = peek();
    if (t.type == TokenType::Eof) return true;
    if (t.type == TokenType::Keyword) return t.textValue == "KHATAM" || t.textValue == "WARNA";
    if (t.type == TokenType::Ident)
        return t.textValue == "else" || t.textValue == "elseif" || t.textValue == "end" || t.textValue == "until";
    return false;
}

ProcBlock Parser::parseProcBlock() {
    ProcBlock block;
    while (!atBlockEnd()) {
        block.push_back(parseProcStatement());
        expectSymbol(";");
    }
    return block;
}

ProcBlock Parser::parseProcBody() {
    StackBase base;
    ProcBlock block = parseProcBlock();
    if (!atEnd()) error("Body ke andar ye samajh nahi aaya (KHATAM / END / ELSE bina IF ya loop ke?)");
    return block;
}

ProcBlock parseProcBody(const std::string& bodyText) {
    Parser p(tokenize(bodyText), bodyText);
    return p.parseProcBody();
}

void Parser::expectBlockEnd(const std::string& kind) {
    auto fail = [&]() {
        if (kind == "IF") error("IF ka end 'END IF' (ya 'KHATAM AGAR') se hona chahiye");
        if (kind == "WHILE") error("WHILE ka end 'END WHILE' (ya 'KHATAM JAB TAK') se hona chahiye");
        error("'END " + kind + "' expected tha");
    };
    if (!matchKeyword("KHATAM") && !matchWord("end")) fail();
    bool ok = false;
    if (kind == "IF") ok = matchWord("if") || matchWord("agar");
    else if (kind == "WHILE") {
        ok = matchWord("while");
        if (!ok && isWord(0, "jab") && isWord(1, "tak")) {
            advance();
            advance();
            ok = true;
        }
    } else if (kind == "LOOP") ok = matchWord("loop");
    else ok = matchWord("repeat");
    if (!ok) fail();
}

// One ordinary SQL statement, kept as its source text (and parse-validated now).
std::unique_ptr<ProcNode> Parser::parseProcSql() {
    auto node = std::make_unique<PSql>();
    int start = peek().start < 0 ? 0 : peek().start;
    parseStatement();
    int end = peek(-1).end;
    node->text = sourceText_.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
    return node;
}

std::unique_ptr<ProcNode> Parser::parseProcStatement() {
    struct Depth {  // like parseStatement's Scope: nested control flow is capped, every statement gets a fresh operator budget
        Parser& parser;
        int savedBudget;
        explicit Depth(Parser& p) : parser(p), savedBudget(p.nestingUsed_) {
            if (p.statementDepth_ >= kMaxStatementNesting)
                p.error("Statements bahut gehre nested hain (limit " + std::to_string(kMaxStatementNesting) + ")");
            ++p.statementDepth_;
            p.nestingUsed_ = 0;
        }
        ~Depth() {
            --parser.statementDepth_;
            parser.nestingUsed_ = savedBudget;
        }
    } depth(*this);
    guardStack();

    auto atLoop = [&]() {
        return isWord(0, "loop") || isWord(0, "while") || isWord(0, "repeat") || (isWord(0, "jab") && isWord(1, "tak"));
    };
    std::string label;
    if (peek().type == TokenType::Ident && peek(1).type == TokenType::Symbol && peek(1).textValue == ":") {
        label = advance().textValue;
        advance();  // ':'
        if (!atLoop()) error("Label ke baad LOOP, WHILE ya REPEAT chahiye");
    }
    if (atLoop()) return parseProcLoop(label);

    if (peek().type == TokenType::Ident) {
        const std::string word = peek().textValue;
        if (word == "declare") return parseProcDeclare();
        if (word == "set" && !isWord(1, "role")) {
            advance();
            return parseProcSet();
        }
        if (word == "if") {
            advance();
            return parseProcIf();
        }
        if (word == "leave" || word == "iterate") {
            advance();
            auto node = std::make_unique<PLeave>();
            node->iterate = word == "iterate";
            if (peek().type == TokenType::Ident) node->label = advance().textValue;
            return node;
        }
        if (word == "return") {
            advance();
            auto node = std::make_unique<PReturn>();
            node->value = parseOr();
            return node;
        }
        if (word == "signal") {
            advance();
            auto node = std::make_unique<PSignal>();
            node->message = parseOr();
            return node;
        }
        if (word == "open" || word == "close" || word == "fetch") {
            advance();
            return parseProcCursorOp(word);
        }
    }
    if (checkKeyword("RAKHO")) {
        advance();
        return parseProcSet();
    }
    if (checkKeyword("AGAR")) {
        advance();
        return parseProcIf();
    }
    if (checkKeyword("DIKHAO")) return parseProcSelect();
    return parseProcSql();
}

std::unique_ptr<ProcNode> Parser::parseProcDeclare() {
    advance();  // DECLARE
    if (isWord(0, "continue") && isWord(1, "handler")) {
        advance();
        advance();
        if (!matchWord("for")) error("DECLARE CONTINUE HANDLER ke baad FOR NOT FOUND chahiye");
        if (!(isWord(0, "not") && isWord(1, "found"))) error("Abhi sirf 'FOR NOT FOUND' handler chalta hai");
        advance();
        advance();
        if (!matchWord("set")) error("HANDLER ... FOR NOT FOUND ke baad SET variable = expr chahiye");
        auto node = std::make_unique<PDeclareHandler>();
        node->var = expectIdent("variable ka naam");
        expectSymbol("=");
        node->value = parseOr();
        return node;
    }
    std::vector<std::string> names;
    names.push_back(expectIdent("variable ka naam"));
    while (matchSymbol(",")) names.push_back(expectIdent("variable ka naam"));
    if (names.size() == 1 && isWord(0, "cursor")) {
        advance();
        if (!matchWord("for")) error("DECLARE name CURSOR ke baad FOR DIKHAO ... chahiye");
        int start = peek().start < 0 ? 0 : peek().start;
        expectKeyword("DIKHAO");
        parseDikhaoStmt();
        int end = peek(-1).end;
        auto node = std::make_unique<PDeclareCursor>();
        node->name = names[0];
        node->selectText = sourceText_.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
        return node;
    }
    auto node = std::make_unique<PDeclare>();
    node->names = names;
    node->typeName = parseTypeName("Variable '" + names[0] + "'");
    if (matchWord("default") || matchKeyword("WARNA")) node->defaultValue = parseOr();
    return node;
}

std::unique_ptr<ProcNode> Parser::parseProcSet() {
    auto node = std::make_unique<PSet>();
    node->assignments = parseVarAssignments();
    return node;
}

std::unique_ptr<ProcNode> Parser::parseProcIf() {
    auto node = std::make_unique<PIf>();
    while (true) {
        auto cond = parseOr();
        if (!matchWord("then") && !matchKeyword("TAB")) error("IF/AGAR ki shart ke baad THEN (ya TAB) expected tha");
        node->branches.emplace_back(std::move(cond), parseProcBlock());
        if (matchWord("elseif")) continue;
        if (checkKeyword("WARNA") && checkKeywordAt(1, "AGAR")) {  // WARNA AGAR = ELSEIF
            advance();
            advance();
            continue;
        }
        break;
    }
    if (checkKeyword("WARNA") || isWord(0, "else")) {
        advance();
        node->elseBlock = parseProcBlock();
    }
    expectBlockEnd("IF");
    return node;
}

std::unique_ptr<ProcNode> Parser::parseProcLoop(const std::string& label) {
    auto node = std::make_unique<PLoop>();
    node->label = label;
    bool hinglishWhile = isWord(0, "jab") && isWord(1, "tak");
    if (isWord(0, "while") || hinglishWhile) {
        advance();
        if (hinglishWhile) advance();
        node->kind = "WHILE";
        node->cond = parseOr();
        if (!matchKeyword("DO") && !matchWord("karo")) error("WHILE ki shart ke baad DO (ya KARO) expected tha");
        node->body = parseProcBlock();
        expectBlockEnd("WHILE");
    } else if (isWord(0, "loop")) {
        advance();
        node->kind = "LOOP";
        node->body = parseProcBlock();
        expectBlockEnd("LOOP");
    } else {
        advance();  // REPEAT
        node->kind = "REPEAT";
        node->body = parseProcBlock();
        if (!matchWord("until")) error("REPEAT ke baad UNTIL shart chahiye");
        node->cond = parseOr();
        expectBlockEnd("REPEAT");
    }
    if (!label.empty() && peek().type == TokenType::Ident && peek().textValue == label) advance();  // END LOOP label
    return node;
}

// DIKHAO ... INTO a, b ... : the INTO clause (before SE, or at the very end) is cut out of the query text.
std::unique_ptr<ProcNode> Parser::parseProcSelect() {
    size_t into = 0, endIndex = pos_;
    bool found = false;
    int depth = 0;
    for (size_t i = pos_ + 1; i < tokens_.size(); ++i) {
        const Token& t = tokens_[i];
        endIndex = i;
        if (t.type == TokenType::Eof) break;
        if (t.type == TokenType::Symbol) {
            if (t.textValue == "(") ++depth;
            else if (t.textValue == ")") --depth;
            else if (t.textValue == ";" && depth <= 0) break;
        } else if (depth == 0 && !found && t.type == TokenType::Ident && t.textValue == "into") {
            found = true;
            into = i;
        }
    }
    if (!found) return parseProcSql();

    auto node = std::make_unique<PSelectInto>();
    size_t j = into + 1;
    while (true) {
        if (j >= endIndex || tokens_[j].type != TokenType::Ident) {
            pos_ = j < tokens_.size() ? j : tokens_.size() - 1;
            error("INTO ke baad variable ka naam expected tha");
        }
        node->vars.push_back(tokens_[j].textValue);
        ++j;
        if (j < endIndex && tokens_[j].type == TokenType::Symbol && tokens_[j].textValue == ",") {
            ++j;
            continue;
        }
        break;
    }
    const size_t start = static_cast<size_t>(tokens_[pos_].start);
    const size_t intoStart = static_cast<size_t>(tokens_[into].start);
    const size_t intoEnd = static_cast<size_t>(tokens_[j - 1].end);
    const size_t stmtEnd = static_cast<size_t>(tokens_[endIndex - 1].end);
    node->selectText = sourceText_.substr(start, intoStart - start);
    if (intoEnd < stmtEnd) node->selectText += " " + sourceText_.substr(intoEnd, stmtEnd - intoEnd);
    tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(into), tokens_.begin() + static_cast<std::ptrdiff_t>(j));
    parseStatement();  // validates the query without its INTO clause
    return node;
}

std::unique_ptr<ProcNode> Parser::parseProcCursorOp(const std::string& op) {
    auto node = std::make_unique<PCursorOp>();
    node->op = op;
    node->name = expectIdent("cursor ka naam");
    if (op == "fetch") {
        if (!matchWord("into")) error("FETCH cursor ke baad INTO variable(s) chahiye");
        node->vars.push_back(expectIdent("variable ka naam"));
        while (matchSymbol(",")) node->vars.push_back(expectIdent("variable ka naam"));
    }
    return node;
}

}  // namespace meradb
