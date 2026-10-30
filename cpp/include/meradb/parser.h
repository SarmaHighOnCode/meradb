// cpp/include/meradb/parser.h
#pragma once
#include "meradb/ast.h"
#include "meradb/proc_ast.h"
#include "meradb/tokenizer.h"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace meradb {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens, std::string sourceText = "");
    std::vector<std::unique_ptr<ast::Statement>> parseScript();

    // Statement-level entry points added by Task 7/8; expression parsing
    // (Task 6) is usable standalone via parseExpressionEntry(), and
    // internally by every statement parser.
    std::unique_ptr<ast::Expr> parseExpressionEntry();
    bool atEnd() const { return peek().type == TokenType::Eof; }
    // A whole trigger / procedure / function body (the text between SHURU and KHATAM): statements, DECLARE, IF, loops ...
    ast::ProcBlock parseProcBody();

private:
    std::vector<Token> tokens_;
    std::string sourceText_;
    size_t pos_ = 0;

    // Hostile-input limits (a deliberate divergence from Python, which has none and
    // dies with RecursionError): every operator and every nesting level of ONE
    // statement spends one unit of a shared budget; a statement inside a
    // TRIGGER/PROCEDURE body gets a fresh budget, and statements nest 32 deep.
    // Independently, guardStack() refuses once the stack bytes used pass the
    // budget of stack_guard.h (the count alone does not bound stack use).
    static constexpr int kMaxNesting = 400;
    static constexpr int kMaxStatementNesting = 32;
    int nestingUsed_ = 0;
    int statementDepth_ = 0;
    void spend();
    void guardStack();  // byte-based bound on recursion depth: see stack_guard.h

    const Token& peek(int offset = 0) const;
    const Token& advance();
    bool checkKeyword(const std::string& kw) const;
    bool matchKeyword(const std::string& kw);
    void expectKeyword(const std::string& kw);
    bool checkSymbol(const std::string& sym) const;
    bool matchSymbol(const std::string& sym);
    void expectSymbol(const std::string& sym);
    std::string expectIdent(const std::string& what = "naam");
    [[noreturn]] void error(const std::string& msg) const;

    // expression grammar, lowest to highest precedence
    std::unique_ptr<ast::Expr> parseOr();
    std::unique_ptr<ast::Expr> parseAnd();
    std::unique_ptr<ast::Expr> parseNot();
    std::unique_ptr<ast::Expr> parseComparison();
    std::unique_ptr<ast::Expr> parseComparisonStep(std::unique_ptr<ast::Expr>& left);
    // Handles JAISA/BEECH/MEIN. Only consumes (moves out of) `left` when one
    // of those keywords actually matches; returns nullptr and leaves `left`
    // untouched otherwise, so the caller can fall through to plain
    // comparison operators (mirrors Python's by-reference `left` param).
    std::unique_ptr<ast::Expr> parsePatternRangeOrList(std::unique_ptr<ast::Expr>& left);
    std::unique_ptr<ast::Expr> parseAdditive();
    std::unique_ptr<ast::Expr> parseTerm();
    std::unique_ptr<ast::Expr> parseUnary();
    std::unique_ptr<ast::Expr> parsePrimary();
    std::unique_ptr<ast::CaseWhen> parseCase();

    // Task 7/8: statement-level parsing
    std::unique_ptr<ast::Statement> parseStatement();      // guarded: see spend() / the Scope in parser.cpp
    std::unique_ptr<ast::Statement> parseStatementBody();  // the real statement dispatcher
    std::unique_ptr<ast::Select> parseSelectBody();
    std::unique_ptr<ast::Statement> parseBanao();
    std::unique_ptr<ast::Statement> parseHatao();
    // Phase 2: users / privileges / procedure calls
    std::unique_ptr<ast::Statement> parseAdhikar();
    std::unique_ptr<ast::Statement> parseChalao();
    void parsePrivilegeList(std::vector<std::string>& privileges, std::map<std::string, std::vector<std::string>>& columns);
    std::string expectPrivilege();
    std::vector<std::string> parseNameList(const char* what);
    std::unique_ptr<ast::Statement> parseShow();
    std::unique_ptr<ast::Statement> parseSetRole();
    std::optional<std::string> parseForUser();
    std::string expectString(const std::string& what);
    std::string parseBlockBody(const std::string& what);
    std::unique_ptr<ast::Statement> parseCreateTrigger();
    std::unique_ptr<ast::Statement> parseCreateProcedure();
    ast::ProcParam parseProcParam(bool allowMode);
    std::string parseTypeName(const std::string& what);
    std::unique_ptr<ast::Statement> parseCreateFunction();
    std::unique_ptr<ast::Statement> parseSet();
    std::unique_ptr<ast::Statement> parsePrepare();
    std::unique_ptr<ast::Statement> parseExecute();
    std::vector<std::pair<std::string, std::unique_ptr<ast::Expr>>> parseVarAssignments();
    // ---- procedural bodies (parser_proc.cpp)
    bool atBlockEnd() const;
    bool matchWord(const char* word);  // a soft word (IDENT) or the keyword with this spelling
    bool isWord(int offset, const char* word) const;
    ast::ProcBlock parseProcBlock();
    std::unique_ptr<ast::ProcNode> parseProcStatement();
    std::unique_ptr<ast::ProcNode> parseProcSql();
    std::unique_ptr<ast::ProcNode> parseProcDeclare();
    std::unique_ptr<ast::ProcNode> parseProcSet();
    std::unique_ptr<ast::ProcNode> parseProcIf();
    std::unique_ptr<ast::ProcNode> parseProcLoop(const std::string& label);
    std::unique_ptr<ast::ProcNode> parseProcSelect();
    std::unique_ptr<ast::ProcNode> parseProcCursorOp(const std::string& op);
    void expectBlockEnd(const std::string& kind);  // END IF | KHATAM AGAR | END WHILE | KHATAM JAB TAK | END LOOP | END REPEAT
    std::unique_ptr<ast::Statement> parseSudharo();
    std::unique_ptr<ast::Statement> parseSaaf();
    std::unique_ptr<ast::Statement> parseSikodo();
    std::unique_ptr<ast::Statement> parseIstemal();
    std::unique_ptr<ast::Statement> parseBatao();
    std::unique_ptr<ast::Statement> parseInsert(bool replace, bool intoAlreadyRead);
    std::unique_ptr<ast::Statement> parseDikhaoStmt();
    std::unique_ptr<ast::Statement> parseUpdate();
    std::unique_ptr<ast::Statement> parseDelete();
    void parseTableItem(ast::CreateTable& stmt);
    std::vector<std::string> parseCompositeColumns(size_t minColumns = 2);
    ast::ColumnDef parseColumnDef();
    bool matchIfExists();     // IF EXISTS
    bool matchIfNotExists();  // IF NOT EXISTS
    bool isSoftWord(int offset, const char* word) const;  // an IDENT (lower-cased) with this exact spelling
    bool checkKeywordAt(int offset, const std::string& kw) const;
    bool atForeignKeyItem(int offset) const;
    ast::ForeignKeyDef parseForeignKeyDef();
    void parseFkActions(std::string& onDelete, std::string& onUpdate);
    std::optional<int> parseTypeLength(const std::string& typeName, const std::string& column);
    std::unique_ptr<ast::CreateView> parseCreateView();
    std::vector<std::unique_ptr<ast::Expr>> parseValueTuple();
    std::vector<std::pair<std::string, std::unique_ptr<ast::Expr>>> parseAssignmentList();
    std::pair<std::string, std::unique_ptr<ast::Expr>> parseAssignment();
    std::pair<std::unique_ptr<ast::Expr>, std::optional<std::string>> parseSelectItem();
    std::optional<std::string> parseAliasOpt();
    ast::OrderItem parseOrderItem();
};

std::vector<std::unique_ptr<ast::Statement>> parseScript(const std::string& text);
std::unique_ptr<ast::Expr> parseExpression(const std::string& text);
// A stored body (trigger / procedure / function) parsed into its statement tree; the text is what BANAO ... SHURU ... KHATAM kept.
ast::ProcBlock parseProcBody(const std::string& bodyText);

}  // namespace meradb
