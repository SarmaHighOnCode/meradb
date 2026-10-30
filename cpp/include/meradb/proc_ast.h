// cpp/include/meradb/proc_ast.h
//
// The PROCEDURAL part of MeraDB: what a trigger / procedure / function body is made of once it is parsed. A body is
// a list of nodes. Ordinary SQL statements stay as their SOURCE TEXT (PSql) -- each run re-parses them fresh and
// substitutes the variables, exactly as triggers always did; only the control flow (IF, WHILE, LOOP, REPEAT, DECLARE,
// cursors ...) is a real tree. Built by Parser::parseProcBlock, run by the Engine (procedural.cpp).
#pragma once
#include "meradb/ast.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace meradb::ast {

struct ProcNode { virtual ~ProcNode() = default; };
using ProcBlock = std::vector<std::unique_ptr<ProcNode>>;

// one ordinary statement (DAALO, BADLO, DIKHAO, CHALAO, PREPARE, ...), as its source text
struct PSql : ProcNode { std::string text; };
// SET a = expr [, b = expr]  /  RAKHO a = expr. A target is a local variable, `@user_variable`, `autocommit`, or
// `naya.col` (a BEFORE trigger changing the row about to be written).
struct PSet : ProcNode { std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments; };
// DECLARE a, b type [DEFAULT expr]
struct PDeclare : ProcNode {
    std::vector<std::string> names;
    std::string typeName;
    std::unique_ptr<Expr> defaultValue;
};
// DECLARE c CURSOR FOR DIKHAO ...  (the query is kept as text and run at OPEN)
struct PDeclareCursor : ProcNode { std::string name; std::string selectText; };
// DECLARE CONTINUE HANDLER FOR NOT FOUND SET v = expr
struct PDeclareHandler : ProcNode { std::string var; std::unique_ptr<Expr> value; };
// IF / AGAR: one branch per IF / ELSEIF, then the optional ELSE
struct PIf : ProcNode {
    std::vector<std::pair<std::unique_ptr<Expr>, ProcBlock>> branches;
    ProcBlock elseBlock;
};
// WHILE cond DO .. END WHILE | LOOP .. END LOOP | REPEAT .. UNTIL cond END REPEAT
struct PLoop : ProcNode {
    std::string kind;   // WHILE | LOOP | REPEAT
    std::string label;  // "" = none
    std::unique_ptr<Expr> cond;
    ProcBlock body;
};
// LEAVE label / ITERATE label
struct PLeave : ProcNode { std::string label; bool iterate = false; };
struct PReturn : ProcNode { std::unique_ptr<Expr> value; };
struct PSignal : ProcNode { std::unique_ptr<Expr> message; };
// DIKHAO a, b INTO x, y SE ... (the INTO clause is cut out of selectText)
struct PSelectInto : ProcNode { std::string selectText; std::vector<std::string> vars; };
// OPEN c / FETCH c INTO a, b / CLOSE c
struct PCursorOp : ProcNode { std::string op; std::string name; std::vector<std::string> vars; };

}  // namespace meradb::ast
