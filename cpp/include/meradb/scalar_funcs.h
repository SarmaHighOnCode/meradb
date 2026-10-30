// cpp/include/meradb/scalar_funcs.h
//
// Scalar (row-by-row) functions: UPPER, LOWER, LENGTH, TRIM, SUBSTRING, CONCAT, ROUND, YEAR, DATEDIFF, ... and
// CAST. Unlike the aggregates (GINO, KUL, ...) each call looks at ONE row's values and returns one value.
//
// KHALI in => KHALI out (NULLIF is the one exception). The parser knows the
// names and their argument counts (findScalarFunc); the evaluator evaluates the arguments and calls callScalarFunc.
#pragma once
#include "meradb/datatypes.h"
#include <string>
#include <vector>

namespace meradb {

struct ScalarSpec {
    const char* name;      // the spelling a user types (upper-case)
    const char* canonical; // what it is called inside (CHAR_LENGTH -> LENGTH)
    int minArgs;
    int maxArgs;           // -1 = any number
};

// nullptr if `upperName` is not a scalar function.
const ScalarSpec* findScalarFunc(const std::string& upperName);
// "UPPER, LOWER, ..." -- every name findScalarFunc knows, for error messages.
std::string scalarFunctionList();

// Calls the function on already-evaluated arguments. The argument count was checked by the parser.
Value callScalarFunc(const std::string& upperName, const std::vector<Value>& args);

// CAST(value AS typeName): typeName is a normalized type (INT, FLOAT, TEXT, DATE, BOOL).
Value castValue(const Value& value, const std::string& typeName);

// The text form every string function sees for a non-text value (numbers, dates, SACH/JHOOTH).
std::string valueAsText(const Value& value);

}  // namespace meradb
