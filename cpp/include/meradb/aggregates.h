// cpp/include/meradb/aggregates.h
//
// Aggregate functions: they turn MANY rows into ONE value. Mirrors
// meradb/aggregates.py.
//
//     GINO(*)        COUNT(*)   number of rows
//     GINO(x)        COUNT(x)   number of rows where x is not KHALI
//     KUL(x)         SUM(x)
//     AUSAT(x)       AVG(x)
//     NYUNTAM(x)     MIN(x)
//     ADHIKTAM(x)    MAX(x)
//
// Every aggregate except GINO(*) IGNORES KHALI values; with zero non-KHALI
// values KUL/AUSAT/NYUNTAM/ADHIKTAM return KHALI (GINO returns 0).
//
// Grouping itself (SAMOOH/JINKA partitioning) lives in the engine; this
// module only does the per-group math.
#pragma once
#include "meradb/ast.h"
#include "meradb/evaluator.h"
#include <string>
#include <vector>

namespace meradb {

// "COUNT" -> "GINO", "SUM" -> "KUL", ... Throws ExecutionError for an
// unknown function or `X(*)` with anything but GINO/COUNT.
std::string canonicalName(const ast::FuncCall& func);

// Compute one aggregate over the rows of one group.
Value computeAggregate(const ast::FuncCall& func, const std::vector<Row>& groupRows);

}  // namespace meradb
