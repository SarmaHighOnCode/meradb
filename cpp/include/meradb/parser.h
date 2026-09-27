// cpp/include/meradb/parser.h
#pragma once
#include "meradb/ast.h"
#include "meradb/tokenizer.h"
#include <memory>
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

private:
    std::vector<Token> tokens_;
    std::string sourceText_;
    size_t pos_ = 0;

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
    std::unique_ptr<ast::Statement> parseStatement();
    std::unique_ptr<ast::Select> parseSelectBody();
    std::unique_ptr<ast::Statement> parseBanao();
    std::unique_ptr<ast::Statement> parseHatao();
    std::unique_ptr<ast::Statement> parseSudharo();
    std::unique_ptr<ast::Statement> parseSaaf();
    std::unique_ptr<ast::Statement> parseSikodo();
    std::unique_ptr<ast::Statement> parseIstemal();
    std::unique_ptr<ast::Statement> parseBatao();
    std::unique_ptr<ast::Statement> parseInsert();
    std::unique_ptr<ast::Statement> parseDikhaoStmt();
    std::unique_ptr<ast::Statement> parseUpdate();
    std::unique_ptr<ast::Statement> parseDelete();
    void parseTableItem(ast::CreateTable& stmt);
    std::vector<std::string> parseCompositeColumns();
    ast::ColumnDef parseColumnDef();
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

}  // namespace meradb
