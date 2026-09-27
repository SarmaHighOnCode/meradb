# MeraDB C++ Port — Phase 1: Core Engine Implementation Plan

**Goal:** Port MeraDB's core query engine (tokenizer → parser → AST → planner →
evaluator → aggregates → datatypes → catalog → table → storage) from Python
to C++, driven by a local-mode CLI runner, with behavior verified against
the existing Python engine on real `.mdb` scripts.

**Architecture:** Direct layer-for-layer mirror of the Python implementation
(same module boundaries, same on-disk formats, same Hinglish grammar). AST
nodes become a `std::unique_ptr`-owned class hierarchy with a visitor
pattern; dynamic values become a `Value` class wrapping `std::variant`.
No networking, no users/privileges/triggers/procedures in this phase —
those are Phase 2.

**Tech Stack:** C++17, CMake + FetchContent, nlohmann/json (catalog
persistence), Catch2 v3 (tests). No other dependencies in this phase.

**Spec:** [docs/cpp-port/specs/2026-09-27-cpp-port-design.md](../specs/2026-09-27-cpp-port-design.md)

## Global Constraints

- C++17 minimum, buildable with MSVC or MinGW-w64/g++ on Windows and with
  g++/clang on Linux/Mac.
- Dependencies: only nlohmann/json and Catch2 in this phase, pulled via
  CMake `FetchContent` — no manual install steps for a fresh clone.
- No raw `new`/`delete` for AST or Value ownership — `std::unique_ptr`
  throughout.
- Existing Python implementation (`meradb/`) is never modified or removed.
  All new code lives under `cpp/`.
- Every layer gets its own header + source file, matching the Python
  module boundaries (`tokenizer`, `ast`, `parser`, `planner`, `evaluator`,
  `aggregates`, `datatypes`, `catalog`, `table`, `storage`, `errors`,
  `engine`) — one clear responsibility per file, same as the Python side.
- On-disk formats must byte-for-byte match the Python version's heap-file
  format (`storage.py`'s `encode_row`/`HeapFile` layout) and catalog JSON
  shape (`catalog.py`), so files written by one implementation are readable
  by the other — this is also how cross-engine diffing verification works.
- Error message text should follow the same `[Stage Galti] <message>`
  convention as `errors.py`, with a `stage()` accessor and a `message()`
  accessor (bare, unprefixed) mirroring the Python fix for the
  double-prefix bug.
- Every Hinglish keyword, operator, and grammar shape must match the
  Python tokenizer/parser exactly — this is a port, not a redesign; when
  in doubt, the Python source (`meradb/tokenizer.py`, `meradb/parser.py`,
  `meradb/ast_nodes.py`) is the ground truth to re-check against.

---

## Reference: Full Keyword List (from `meradb/tokenizer.py`)

Case-insensitive; identifiers are lower-cased, keywords upper-cased in the
token stream.

**DDL/structural:** `BANAO`(CREATE) `HATAO`(DROP) `SUDHARO`(ALTER)
`JODO`(ADD, inside ALTER) `SAAF`(TRUNCATE) `TABLE` `TABLES` `DATABASE`
`COLUMN` `ISTEMAL`(USE) `BATAO`(DESCRIBE) `SIKODO`(VACUUM/compact) `VIEW`
`VIEWS`

**Constraints:** `MUKHYA`(PRIMARY) `KUNJI`(KEY) `ZAROORI`(NOT NULL)
`ANOKHA`(UNIQUE) `WARNA`(DEFAULT, also CASE-ELSE) `SANDARBH`(REFERENCES)
`SHART`(CHECK) `NAYA_NAAM`(RENAME TO)

**DML:** `DAALO`(INSERT) `MEIN`(INTO) `MAAN`(VALUES) `DIKHAO`(SELECT)
`SE`(FROM) `JAHAN`(WHERE) `BADLO`(UPDATE) `RAKHO`(SET) `MITAO`(DELETE)
`KRAM`(ORDER BY) `SEEDHA`(ASC) `ULTA`(DESC) `SIRF`(LIMIT) `ALAG`(DISTINCT)
`SAMOOH`(GROUP BY) `JINKA`(HAVING) `MILAO`(JOIN) `BAAYAN`(LEFT)
`DAHINA`(RIGHT) `DONO`(FULL) `SAMAAN`(NATURAL) `PAR`(ON) `KAHO`(AS)
`SAMJHAO`(EXPLAIN)

**Transactions:** `SHURU`(BEGIN) `PAKKA`(COMMIT) `WAPAS`(ROLLBACK)

**Logic/literals:** `JAISA`(LIKE) `BEECH`(BETWEEN) `AUR`(AND) `YA`(OR)
`NAHI`(NOT) `HAI`(IS) `KHALI`(NULL) `SACH`(TRUE) `JHOOTH`(FALSE)

**Set ops / CASE / upsert:** `SANYUKT`(UNION) `SAAJHA`(INTERSECT)
`CHHODKAR`(EXCEPT) `AGAR`(CASE) `TAB`(THEN) `KHATAM`(END)
`TAKRAAV`(CONFLICT)

**Symbols:** two-char `<= >= != <>` (`<>` normalizes to `!=`); one-char
`( ) , ; * = < > + - / % .`

`PEHLA` (COALESCE) is NOT a reserved keyword — it's an ordinary identifier
special-cased when parsing a function-call-shaped primary expression.

Out of scope for Phase 1 (Phase 2 territory — do not implement yet):
`USER`, `GUPT`, `ADHIKAR`, `DO`, `KO`, `SAB`, `TRIGGER`, `PEHLE`, `BAAD`,
`PROCEDURE`, `CHALAO`.

---

### Task 1: Project scaffolding & build system

**Files:**
- Create: `cpp/CMakeLists.txt`
- Create: `cpp/tests/CMakeLists.txt`
- Create: `cpp/tests/test_sanity.cpp`
- Create: `.gitignore` entries for `cpp/build/`

**Interfaces:**
- Produces: a `meradb_core` static library target (empty for now) and a
  `meradb_tests` executable target that later tasks add sources/tests to.

- [ ] **Step 1: Write the root CMakeLists.txt**

```cmake
# cpp/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(meradb_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

include(FetchContent)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
  catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.4
)
FetchContent_MakeAvailable(catch2)

add_library(meradb_core STATIC
  # source files are appended here by later tasks
)
target_include_directories(meradb_core PUBLIC include)
target_link_libraries(meradb_core PUBLIC nlohmann_json::nlohmann_json)

add_executable(meradb_cli src/main.cpp)
target_link_libraries(meradb_cli PRIVATE meradb_core)

enable_testing()
add_subdirectory(tests)
```

- [ ] **Step 2: Write the tests CMakeLists.txt**

```cmake
# cpp/tests/CMakeLists.txt
add_executable(meradb_tests
  test_sanity.cpp
  # test files are appended here by later tasks
)
target_link_libraries(meradb_tests PRIVATE meradb_core Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(meradb_tests)
```

Note: `catch_discover_tests` needs Catch2's CMake module on the include
path — add `list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)`
and `include(Catch)` right after `FetchContent_MakeAvailable(catch2)` in
the root `CMakeLists.txt` if `include(Catch)` fails to resolve.

- [ ] **Step 3: Write a trivial sanity test**

```cpp
// cpp/tests/test_sanity.cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("build system works", "[sanity]") {
    REQUIRE(1 + 1 == 2);
}
```

- [ ] **Step 4: Write a placeholder main.cpp**

```cpp
// cpp/src/main.cpp
#include <iostream>

int main() {
    std::cout << "MeraDB C++ (scaffolding)\n";
    return 0;
}
```

- [ ] **Step 5: Configure and build**

Run:
```bash
cmake -S cpp -B cpp/build
cmake --build cpp/build
```
Expected: configure succeeds (FetchContent downloads json + Catch2), build
succeeds, producing `meradb_cli` and `meradb_tests` binaries.

- [ ] **Step 6: Run the sanity test**

Run: `ctest --test-dir cpp/build --output-on-failure`
Expected: 1 test passes ("build system works").

- [ ] **Step 7: Add .gitignore entry and commit**

```bash
echo "cpp/build/" >> .gitignore
git add cpp/CMakeLists.txt cpp/tests/CMakeLists.txt cpp/tests/test_sanity.cpp cpp/src/main.cpp .gitignore
git commit -m "Scaffold C++ build system (CMake, nlohmann/json, Catch2)"
```

---

### Task 2: Errors module

**Files:**
- Create: `cpp/include/meradb/errors.h`
- Test: `cpp/tests/test_errors.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/errors.cpp` — actually header-only,
  no .cpp needed; skip)
- Modify: `cpp/tests/CMakeLists.txt` (add `test_errors.cpp` to the
  `meradb_tests` sources list)

**Interfaces:**
- Produces: `meradb::MeraDBError` base class and `TokenizerError`,
  `ParseError`, `ExecutionError`, `StorageError`, `ConnectionFailed`,
  `ServerUnavailable` subclasses, each constructible from a `std::string`,
  with `.stage()` and `.message()` accessors and a `what()` override
  matching Python's `[Stage Galti] <message>` format. Every later task's
  error handling throws one of these.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_errors.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"

using namespace meradb;

TEST_CASE("MeraDBError formats stage and message", "[errors]") {
    ExecutionError e("naam column not found");
    REQUIRE(e.stage() == "Execution");
    REQUIRE(e.message() == "naam column not found");
    REQUIRE(std::string(e.what()) == "[Execution Galti] naam column not found");
}

TEST_CASE("Each error subclass reports its own stage", "[errors]") {
    REQUIRE(TokenizerError("x").stage() == "Tokenizer");
    REQUIRE(ParseError("x").stage() == "Parser");
    REQUIRE(StorageError("x").stage() == "Storage");
    REQUIRE(ConnectionFailed("x").stage() == "Connection");
    REQUIRE(ServerUnavailable("x").stage() == "Connection");
}

TEST_CASE("ServerUnavailable is-a ConnectionFailed is-a MeraDBError", "[errors]") {
    ServerUnavailable e("no server");
    const MeraDBError& base = e;
    REQUIRE(base.stage() == "Connection");
    const ConnectionFailed& mid = e;
    REQUIRE(mid.message() == "no server");
}
```

- [ ] **Step 2: Add the test file to the build and run to verify it fails**

Add `test_errors.cpp` to `cpp/tests/CMakeLists.txt`'s source list. Run:
```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/errors.h` does not exist yet (compile error).

- [ ] **Step 3: Write the implementation**

Note: `stage()` is virtual and each subclass overrides it, but calling a
virtual function from the *base* constructor always resolves to the
base's own override (C++ construction-order rule), not the subclass's.
So the formatted `what()` string can't be built in `MeraDBError`'s
constructor — instead, every subclass constructor calls a protected
`refreshFormatted()` helper from its *own* constructor body, by which
point virtual dispatch correctly resolves to that subclass's `stage()`.

```cpp
// cpp/include/meradb/errors.h
#pragma once
#include <stdexcept>
#include <string>

namespace meradb {

class MeraDBError : public std::exception {
public:
    explicit MeraDBError(std::string message) : message_(std::move(message)) {
        refreshFormatted();
    }
    virtual ~MeraDBError() = default;
    virtual std::string stage() const { return "MeraDB"; }
    const std::string& message() const { return message_; }
    const char* what() const noexcept override { return formatted_.c_str(); }

protected:
    // Subclass constructors call this after their own stage() becomes
    // callable via the vtable (i.e. from the subclass's own constructor
    // body), so `formatted_` reflects the correct stage name.
    void refreshFormatted() { formatted_ = "[" + stage() + " Galti] " + message_; }

private:
    std::string message_;
    std::string formatted_;
};

class TokenizerError : public MeraDBError {
public:
    explicit TokenizerError(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Tokenizer"; }
};

class ParseError : public MeraDBError {
public:
    explicit ParseError(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Parser"; }
};

class ExecutionError : public MeraDBError {
public:
    explicit ExecutionError(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Execution"; }
};

class StorageError : public MeraDBError {
public:
    explicit StorageError(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Storage"; }
};

class ConnectionFailed : public MeraDBError {
public:
    explicit ConnectionFailed(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Connection"; }
};

class ServerUnavailable : public ConnectionFailed {
public:
    explicit ServerUnavailable(std::string message) : ConnectionFailed(std::move(message)) { refreshFormatted(); }
    // stage() inherited from ConnectionFailed — matches Python's
    // ServerUnavailable(ConnectionFailed), which doesn't override stage either.
};

}  // namespace meradb
```

Delete the placeholder macro block above it — only the "final version"
header content should end up in the file.

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R errors`
Expected: all `[errors]` tests PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/include/meradb/errors.h cpp/tests/test_errors.cpp cpp/tests/CMakeLists.txt
git commit -m "Add C++ errors module (MeraDBError hierarchy)"
```

---

### Task 3: Value & datatypes module

**Files:**
- Create: `cpp/include/meradb/datatypes.h`
- Create: `cpp/src/datatypes.cpp`
- Test: `cpp/tests/test_datatypes.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/datatypes.cpp` to `meradb_core`'s
  sources)
- Modify: `cpp/tests/CMakeLists.txt` (add `test_datatypes.cpp`)

**Interfaces:**
- Consumes: `meradb::ExecutionError` from Task 2.
- Produces: `meradb::Value` (a variant wrapper: `std::monostate` for
  KHALI/NULL, `int64_t`, `double`, `std::string`, `bool`, and a `Date`
  struct for `TAREEKH`), `normalizeType(name) -> std::optional<std::string>`,
  `coerce(Value, type_name, column_name) -> Value`, `formatValue(Value) ->
  std::string`, `parseDate(text, column_name = "") -> Date`. Every later
  layer (table, storage, evaluator, engine) uses `Value` as the row-cell
  type.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_datatypes.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/datatypes.h"
#include "meradb/errors.h"

using namespace meradb;

TEST_CASE("normalizeType resolves aliases", "[datatypes]") {
    REQUIRE(normalizeType("ank") == "INT");
    REQUIRE(normalizeType("INTEGER") == "INT");
    REQUIRE(normalizeType("dashamlav") == "FLOAT");
    REQUIRE(normalizeType("varchar2") == "TEXT");
    REQUIRE(normalizeType("haan_na") == "BOOL");
    REQUIRE(normalizeType("tareekh") == "DATE");
    REQUIRE_FALSE(normalizeType("nonsense").has_value());
}

TEST_CASE("coerce validates and widens", "[datatypes]") {
    Value v = coerce(Value(int64_t{5}), "FLOAT", "cgpa");
    REQUIRE(std::get<double>(v.data) == 5.0);

    REQUIRE_THROWS_AS(coerce(Value(std::string("x")), "INT", "id"), ExecutionError);
}

TEST_CASE("coerce rejects bool for INT columns", "[datatypes]") {
    // Python bool is an int subclass, so coerce must check bool BEFORE int.
    REQUIRE_THROWS_AS(coerce(Value(true), "INT", "id"), ExecutionError);
}

TEST_CASE("coerce passes NULL through untouched", "[datatypes]") {
    Value v = coerce(Value(), "INT", "id");
    REQUIRE(v.isNull());
}

TEST_CASE("formatValue renders Hinglish literals", "[datatypes]") {
    REQUIRE(formatValue(Value(true)) == "SACH");
    REQUIRE(formatValue(Value(false)) == "JHOOTH");
    REQUIRE(formatValue(Value()) == "KHALI");
}

TEST_CASE("parseDate parses ISO dates", "[datatypes]") {
    Date d = parseDate("2004-05-12");
    REQUIRE(d.toOrdinal() > 0);
    REQUIRE_THROWS_AS(parseDate("not-a-date"), ExecutionError);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_datatypes.cpp` to `cpp/tests/CMakeLists.txt`. Run:
```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/datatypes.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/datatypes.h
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace meradb {

struct Date {
    int32_t ordinal;  // proleptic Gregorian ordinal, matches Python date.toordinal()
    int32_t toOrdinal() const { return ordinal; }
    std::string isoFormat() const;  // "YYYY-MM-DD"
    static Date fromOrdinal(int32_t ord) { return Date{ord}; }
    bool operator==(const Date& other) const { return ordinal == other.ordinal; }
};

struct Value {
    using Storage = std::variant<std::monostate, int64_t, double, std::string, bool, Date>;
    Storage data;

    Value() : data(std::monostate{}) {}
    explicit Value(int64_t v) : data(v) {}
    explicit Value(double v) : data(v) {}
    explicit Value(std::string v) : data(std::move(v)) {}
    explicit Value(bool v) : data(v) {}
    explicit Value(Date v) : data(v) {}

    bool isNull() const { return std::holds_alternative<std::monostate>(data); }
};

constexpr int64_t INT_MIN_VALUE = INT64_MIN;
constexpr int64_t INT_MAX_VALUE = INT64_MAX;

std::optional<std::string> normalizeType(const std::string& name);
Date parseDate(const std::string& text, const std::string& column = "");
Value coerce(const Value& value, const std::string& typeName, const std::string& column);
std::string formatValue(const Value& value);

}  // namespace meradb
```

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/datatypes.cpp
#include "meradb/datatypes.h"
#include "meradb/errors.h"
#include <cmath>
#include <ctime>
#include <unordered_map>

namespace meradb {

namespace {
const std::unordered_map<std::string, std::string>& aliasTable() {
    static const std::unordered_map<std::string, std::string> table = {
        {"INT", "INT"}, {"INTEGER", "INT"}, {"ANK", "INT"},
        {"FLOAT", "FLOAT"}, {"REAL", "FLOAT"}, {"DASHAMLAV", "FLOAT"},
        {"NUMBER", "FLOAT"}, {"NUMERIC", "FLOAT"}, {"DECIMAL", "FLOAT"},
        {"TEXT", "TEXT"}, {"STRING", "TEXT"}, {"VARCHAR", "TEXT"},
        {"VARCHAR2", "TEXT"}, {"CHAR", "TEXT"}, {"SHABD", "TEXT"},
        {"BOOL", "BOOL"}, {"BOOLEAN", "BOOL"}, {"HAAN_NA", "BOOL"},
        {"DATE", "DATE"}, {"TAREEKH", "DATE"},
    };
    return table;
}

std::string toUpper(std::string s) {
    for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

std::optional<std::string> normalizeType(const std::string& name) {
    // NOTE: real "VARCHAR(20)"-style names arrive here with the length
    // already stripped by the parser (same split Python's parser does
    // before calling datatypes.normalize_type) — this function only
    // resolves the bare type-name alias.
    auto it = aliasTable().find(toUpper(name));
    if (it == aliasTable().end()) return std::nullopt;
    return it->second;
}

Date parseDate(const std::string& text, const std::string& column) {
    std::tm tm{};
    if (sscanf(text.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
        throw ExecutionError("'" + text + "' ek theek DATE nahi hai" +
                              (column.empty() ? "" : " (column: " + column + ")"));
    }
    // Convert Gregorian y-m-d to a proleptic ordinal (days since 0001-01-01,
    // matching Python's date.toordinal()). Days-before-year uses the
    // standard leap-year-aware formula.
    auto isLeap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; };
    static const int cumDays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int y = tm.tm_year, m = tm.tm_mon, d = tm.tm_mday;
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        throw ExecutionError("'" + text + "' ek theek DATE nahi hai");
    }
    int64_t daysBeforeYear = 365LL * (y - 1) + (y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400;
    int64_t dayOfYear = cumDays[m - 1] + d + (m > 2 && isLeap(y) ? 1 : 0);
    return Date{static_cast<int32_t>(daysBeforeYear + dayOfYear)};
}

Value coerce(const Value& value, const std::string& typeName, const std::string& column) {
    if (value.isNull()) return value;

    if (typeName == "INT") {
        // bool checked BEFORE int: Value never stores a bool as an int
        // itself (they're distinct variant alternatives), so this is just
        // an explicit rejection, matching Python's "isinstance(v, bool)"
        // guard against its int-subclass quirk.
        if (std::holds_alternative<bool>(value.data))
            throw ExecutionError("'" + column + "' INT hai, BOOL nahi milna chahiye");
        if (std::holds_alternative<int64_t>(value.data)) return value;
        if (std::holds_alternative<double>(value.data)) {
            double d = std::get<double>(value.data);
            if (d == std::floor(d)) return Value(static_cast<int64_t>(d));
        }
        throw ExecutionError("'" + column + "' ke liye ANK/INT chahiye");
    }
    if (typeName == "FLOAT") {
        if (std::holds_alternative<double>(value.data)) return value;
        if (std::holds_alternative<int64_t>(value.data))
            return Value(static_cast<double>(std::get<int64_t>(value.data)));
        throw ExecutionError("'" + column + "' ke liye DASHAMLAV/FLOAT chahiye");
    }
    if (typeName == "TEXT") {
        if (std::holds_alternative<std::string>(value.data)) return value;
        throw ExecutionError("'" + column + "' ke liye TEXT chahiye");
    }
    if (typeName == "BOOL") {
        if (std::holds_alternative<bool>(value.data)) return value;
        throw ExecutionError("'" + column + "' ke liye BOOL chahiye");
    }
    if (typeName == "DATE") {
        if (std::holds_alternative<Date>(value.data)) return value;
        if (std::holds_alternative<std::string>(value.data))
            return Value(parseDate(std::get<std::string>(value.data), column));
        throw ExecutionError("'" + column + "' ke liye TAREEKH/DATE chahiye");
    }
    throw ExecutionError("Anjaan type: " + typeName);
}

std::string formatValue(const Value& value) {
    if (value.isNull()) return "KHALI";
    if (std::holds_alternative<bool>(value.data)) return std::get<bool>(value.data) ? "SACH" : "JHOOTH";
    if (std::holds_alternative<int64_t>(value.data)) return std::to_string(std::get<int64_t>(value.data));
    if (std::holds_alternative<double>(value.data)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.10g", std::get<double>(value.data));
        return buf;
    }
    if (std::holds_alternative<std::string>(value.data)) return std::get<std::string>(value.data);
    if (std::holds_alternative<Date>(value.data)) return std::get<Date>(value.data).isoFormat();
    return "";
}

std::string Date::isoFormat() const {
    // Inverse of parseDate's ordinal math: walk the ordinal back to y-m-d.
    // Implemented with a simple day-counting loop for clarity, since dates
    // in this project's test data are always in a small, sane range.
    int64_t remaining = ordinal;
    int year = 1;
    auto isLeap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; };
    while (true) {
        int64_t daysInYear = isLeap(year) ? 366 : 365;
        if (remaining <= daysInYear) break;
        remaining -= daysInYear;
        ++year;
    }
    static const int monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int month = 1;
    for (int i = 0; i < 12; ++i) {
        int days = monthDays[i] + (i == 1 && isLeap(year) ? 1 : 0);
        if (remaining <= days) { month = i + 1; break; }
        remaining -= days;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, static_cast<int>(remaining));
    return buf;
}

}  // namespace meradb
```

- [ ] **Step 5: Add `src/datatypes.cpp` to `meradb_core` in `cpp/CMakeLists.txt`**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R datatypes`
Expected: all `[datatypes]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/datatypes.h cpp/src/datatypes.cpp cpp/tests/test_datatypes.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ Value/datatypes module"
```

---

### Task 4: Tokenizer

**Files:**
- Create: `cpp/include/meradb/tokenizer.h`
- Create: `cpp/src/tokenizer.cpp`
- Test: `cpp/tests/test_tokenizer.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::TokenizerError` from Task 2.
- Produces: `meradb::TokenType` enum (`Keyword, Ident, Number, String,
  Symbol, Eof`), `meradb::Token` struct (`type, textValue, intValue,
  doubleValue, isFloat, line, col`), `meradb::Tokenizer` class with
  `tokenize() -> std::vector<Token>`, and a free function
  `meradb::tokenize(const std::string& text) -> std::vector<Token>`.
  Every later parser task consumes `std::vector<Token>`.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_tokenizer.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/tokenizer.h"
#include "meradb/errors.h"

using namespace meradb;

TEST_CASE("tokenize recognizes keywords case-insensitively", "[tokenizer]") {
    auto tokens = tokenize("banao TABLE");
    REQUIRE(tokens[0].type == TokenType::Keyword);
    REQUIRE(tokens[0].textValue == "BANAO");
    REQUIRE(tokens[1].type == TokenType::Keyword);
    REQUIRE(tokens[1].textValue == "TABLE");
}

TEST_CASE("tokenize lower-cases identifiers", "[tokenizer]") {
    auto tokens = tokenize("Students");
    REQUIRE(tokens[0].type == TokenType::Ident);
    REQUIRE(tokens[0].textValue == "students");
}

TEST_CASE("tokenize distinguishes int and float numbers", "[tokenizer]") {
    auto tokens = tokenize("42 3.14");
    REQUIRE(tokens[0].type == TokenType::Number);
    REQUIRE_FALSE(tokens[0].isFloat);
    REQUIRE(tokens[0].intValue == 42);
    REQUIRE(tokens[1].type == TokenType::Number);
    REQUIRE(tokens[1].isFloat);
    REQUIRE(tokens[1].doubleValue == 3.14);
}

TEST_CASE("tokenize reads single-quoted strings with escaped quotes", "[tokenizer]") {
    auto tokens = tokenize("'Ravi''s book'");
    REQUIRE(tokens[0].type == TokenType::String);
    REQUIRE(tokens[0].textValue == "Ravi's book");
}

TEST_CASE("tokenize handles two-char symbols before one-char", "[tokenizer]") {
    auto tokens = tokenize("<= >= != <>");
    REQUIRE(tokens[0].textValue == "<=");
    REQUIRE(tokens[1].textValue == ">=");
    REQUIRE(tokens[2].textValue == "!=");
    REQUIRE(tokens[3].textValue == "!=");  // <> normalizes to !=
}

TEST_CASE("tokenize skips line comments", "[tokenizer]") {
    auto tokens = tokenize("DIKHAO -- this is a comment\n naam");
    REQUIRE(tokens[0].textValue == "DIKHAO");
    REQUIRE(tokens[1].textValue == "naam");
}

TEST_CASE("tokenize always ends with Eof", "[tokenizer]") {
    auto tokens = tokenize("");
    REQUIRE(tokens.back().type == TokenType::Eof);
}

TEST_CASE("tokenize records start/end char offsets for source slicing", "[tokenizer]") {
    // CHECK/VIEW/TRIGGER/PROCEDURE bodies are re-parsed later from raw
    // source text, so every token must know its byte offsets.
    auto tokens = tokenize("umar >= 0");
    REQUIRE(tokens[0].start == 0);
    REQUIRE(tokens[0].end == 4);
}

TEST_CASE("tokenize throws TokenizerError on unrecognized character", "[tokenizer]") {
    REQUIRE_THROWS_AS(tokenize("naam @ 5"), TokenizerError);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_tokenizer.cpp` to `cpp/tests/CMakeLists.txt`. Run:
```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/tokenizer.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/tokenizer.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace meradb {

enum class TokenType { Keyword, Ident, Number, String, Symbol, Eof };

struct Token {
    TokenType type;
    std::string textValue;   // upper-cased for Keyword/Symbol, lower-cased for Ident, raw for String
    int64_t intValue = 0;
    double doubleValue = 0.0;
    bool isFloat = false;
    int line = 1;
    int col = 1;
    int start = -1;  // byte offset into source text
    int end = -1;
};

class Tokenizer {
public:
    explicit Tokenizer(std::string text);
    std::vector<Token> tokenize();

private:
    std::string text_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;

    char peek(int offset = 0) const;
    char advance();
    void skipWhitespaceAndComments();
    Token readWord();
    Token readNumber();
    Token readString();
    [[noreturn]] void error(const std::string& msg) const;
};

std::vector<Token> tokenize(const std::string& text);

}  // namespace meradb
```

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/tokenizer.cpp
#include "meradb/tokenizer.h"
#include "meradb/errors.h"
#include <cctype>
#include <set>
#include <unordered_set>

namespace meradb {

namespace {
const std::unordered_set<std::string>& keywordSet() {
    static const std::unordered_set<std::string> kw = {
        "BANAO", "HATAO", "SUDHARO", "JODO", "SAAF", "SIKODO", "TABLE", "TABLES",
        "DATABASE", "COLUMN", "ISTEMAL", "BATAO", "VIEW", "VIEWS",
        "MUKHYA", "KUNJI", "ZAROORI", "ANOKHA", "WARNA", "SANDARBH", "SHART", "NAYA_NAAM",
        "DAALO", "MEIN", "MAAN", "DIKHAO", "SE", "JAHAN", "BADLO", "RAKHO", "MITAO",
        "KRAM", "SEEDHA", "ULTA", "SIRF", "ALAG", "SAMOOH", "JINKA", "MILAO",
        "BAAYAN", "DAHINA", "DONO", "SAMAAN", "PAR", "KAHO", "SAMJHAO",
        "SHURU", "PAKKA", "WAPAS",
        "JAISA", "BEECH", "AUR", "YA", "NAHI", "HAI", "KHALI", "SACH", "JHOOTH",
        "SANYUKT", "SAAJHA", "CHHODKAR", "AGAR", "TAB", "KHATAM", "TAKRAAV",
    };
    return kw;
}

std::string toUpper(std::string s) {
    for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

Tokenizer::Tokenizer(std::string text) : text_(std::move(text)) {}

char Tokenizer::peek(int offset) const {
    size_t p = pos_ + static_cast<size_t>(offset);
    return p < text_.size() ? text_[p] : '\0';
}

char Tokenizer::advance() {
    char c = text_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
    return c;
}

void Tokenizer::skipWhitespaceAndComments() {
    while (pos_ < text_.size()) {
        char c = peek();
        if (c == '\xEF' && peek(1) == '\xBB' && peek(2) == '\xBF') {
            // UTF-8 BOM, 3 bytes — skip as raw bytes (not through advance's
            // line/col tracking, since it's not a real character).
            pos_ += 3;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) { advance(); continue; }
        if (c == '-' && peek(1) == '-') {
            while (pos_ < text_.size() && peek() != '\n') advance();
            continue;
        }
        break;
    }
}

[[noreturn]] void Tokenizer::error(const std::string& msg) const {
    throw TokenizerError(msg + " (line " + std::to_string(line_) + ")");
}

Token Tokenizer::readWord() {
    int startLine = line_, startCol = col_;
    size_t start = pos_;
    while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) advance();
    std::string word = text_.substr(start, pos_ - start);
    std::string upper = toUpper(word);
    Token t;
    t.line = startLine; t.col = startCol;
    t.start = static_cast<int>(start); t.end = static_cast<int>(pos_);
    if (keywordSet().count(upper)) {
        t.type = TokenType::Keyword;
        t.textValue = upper;
    } else {
        t.type = TokenType::Ident;
        t.textValue = toLower(word);
    }
    return t;
}

Token Tokenizer::readNumber() {
    int startLine = line_, startCol = col_;
    size_t start = pos_;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    bool isFloat = false;
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        isFloat = true;
        advance();  // consume '.'
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    }
    std::string text = text_.substr(start, pos_ - start);
    Token t;
    t.type = TokenType::Number;
    t.textValue = text;
    t.isFloat = isFloat;
    t.line = startLine; t.col = startCol;
    t.start = static_cast<int>(start); t.end = static_cast<int>(pos_);
    if (isFloat) t.doubleValue = std::stod(text);
    else t.intValue = std::stoll(text);
    return t;
}

Token Tokenizer::readString() {
    int startLine = line_, startCol = col_;
    size_t start = pos_;
    advance();  // opening '
    std::string value;
    while (true) {
        if (pos_ >= text_.size()) error("Adhoora string literal (khatam nahi hua)");
        char c = advance();
        if (c == '\'') {
            if (peek() == '\'') { value += '\''; advance(); continue; }  // '' -> '
            break;
        }
        value += c;
    }
    Token t;
    t.type = TokenType::String;
    t.textValue = value;
    t.line = startLine; t.col = startCol;
    t.start = static_cast<int>(start); t.end = static_cast<int>(pos_);
    return t;
}

std::vector<Token> Tokenizer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        skipWhitespaceAndComments();
        if (pos_ >= text_.size()) {
            Token eof;
            eof.type = TokenType::Eof;
            eof.line = line_; eof.col = col_;
            eof.start = static_cast<int>(pos_); eof.end = static_cast<int>(pos_);
            tokens.push_back(eof);
            break;
        }
        char c = peek();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(readWord());
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            tokens.push_back(readNumber());
        } else if (c == '\'') {
            tokens.push_back(readString());
        } else {
            int startLine = line_, startCol = col_;
            size_t start = pos_;
            std::string two = text_.substr(pos_, 2);
            static const std::set<std::string> twoChar = {"<=", ">=", "!=", "<>"};
            std::string symbol;
            if (twoChar.count(two)) {
                advance(); advance();
                symbol = (two == "<>") ? "!=" : two;
            } else {
                static const std::string oneChar = "(),;*=<>+-/%.";
                if (oneChar.find(c) == std::string::npos) {
                    error(std::string("Anjaan character: '") + c + "'");
                }
                advance();
                symbol = std::string(1, c);
            }
            Token t;
            t.type = TokenType::Symbol;
            t.textValue = symbol;
            t.line = startLine; t.col = startCol;
            t.start = static_cast<int>(start); t.end = static_cast<int>(pos_);
            tokens.push_back(t);
        }
    }
    return tokens;
}

std::vector<Token> tokenize(const std::string& text) {
    return Tokenizer(text).tokenize();
}

}  // namespace meradb
```

- [ ] **Step 5: Add `src/tokenizer.cpp` to `meradb_core` in `cpp/CMakeLists.txt`**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R tokenizer`
Expected: all `[tokenizer]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/tokenizer.h cpp/src/tokenizer.cpp cpp/tests/test_tokenizer.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ tokenizer"
```

---

### Task 5: AST node definitions

**Files:**
- Create: `cpp/include/meradb/ast.h`
- Test: `cpp/tests/test_ast.cpp`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Produces: `meradb::ast::Expr` and `meradb::ast::Statement` abstract base
  classes (each with a virtual destructor only — no other virtual methods
  needed yet; dispatch happens later in the parser/evaluator/engine via
  `dynamic_cast` checks or an explicit `kind()` tag, decided in Task 6),
  plus every concrete node listed below, each owning child nodes via
  `std::unique_ptr`. All parser/planner/evaluator/engine tasks depend on
  every type defined here.
- This header is data-only (no `.cpp` file needed).

This is a direct, one-to-one transcription of every dataclass in
`meradb/ast_nodes.py` (see Explore-agent research above for the source
enumeration). Every field name and optionality is preserved so later
tasks can cross-reference the Python source unambiguously.

- [ ] **Step 1: Write a compile/construction test**

AST nodes are pure data — there's no independent "behavior" to unit test
here, so the test instead exercises that every node compiles and can be
constructed and moved, since the parser (Task 6+) will build large owned
trees of these under `std::unique_ptr` and any missing move-constructor
or slicing bug would silently corrupt query results.

```cpp
// cpp/tests/test_ast.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/ast.h"

using namespace meradb::ast;

TEST_CASE("expression nodes construct and nest", "[ast]") {
    auto left = std::make_unique<Literal>(Value(int64_t{1}));
    auto right = std::make_unique<Literal>(Value(int64_t{2}));
    BinaryOp add("+", std::move(left), std::move(right));
    REQUIRE(add.op == "+");
    REQUIRE(std::get<int64_t>(static_cast<Literal*>(add.left.get())->value.data) == 1);
}

TEST_CASE("ColumnRef defaults table to nullopt", "[ast]") {
    ColumnRef ref("naam");
    REQUIRE(ref.name == "naam");
    REQUIRE_FALSE(ref.table.has_value());
}

TEST_CASE("Select statement holds joins and order items", "[ast]") {
    Select sel;
    sel.table = "students";
    sel.columns.push_back(std::make_unique<Star>());
    Join j;
    j.table = "courses"; j.alias = "c"; j.kind = "LEFT";
    sel.joins.push_back(std::move(j));
    REQUIRE(sel.joins.size() == 1);
    REQUIRE(sel.joins[0].kind == "LEFT");
}

TEST_CASE("CreateTable holds ColumnDef list and composite constraints", "[ast]") {
    CreateTable ct;
    ct.name = "students";
    ColumnDef col;
    col.name = "id"; col.typeName = "INT"; col.primaryKey = true;
    ct.columns.push_back(col);
    ct.compositeUnique.push_back({"student_id", "course_id"});
    REQUIRE(ct.columns[0].primaryKey);
    REQUIRE(ct.compositeUnique[0].size() == 2);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_ast.cpp` to `cpp/tests/CMakeLists.txt`. Run:
```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/ast.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/ast.h
#pragma once
#include "meradb/datatypes.h"
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace meradb::ast {

using meradb::Value;

// ---- base classes ----
struct Expr { virtual ~Expr() = default; };
struct Statement { virtual ~Statement() = default; };

// ---- expressions ----
struct Literal : Expr {
    Value value;
    explicit Literal(Value v) : value(std::move(v)) {}
};

struct ColumnRef : Expr {
    std::string name;
    std::optional<std::string> table;
    explicit ColumnRef(std::string n, std::optional<std::string> t = std::nullopt)
        : name(std::move(n)), table(std::move(t)) {}
};

struct Star : Expr {
    std::optional<std::string> table;
};

struct BinaryOp : Expr {
    std::string op;
    std::unique_ptr<Expr> left, right;
    BinaryOp(std::string o, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r)
        : op(std::move(o)), left(std::move(l)), right(std::move(r)) {}
};

struct UnaryOp : Expr {
    std::string op;  // "-" or "NAHI"
    std::unique_ptr<Expr> operand;
};

struct IsNull : Expr {
    std::unique_ptr<Expr> expr;
    bool negated = false;
};

struct FuncCall : Expr {
    std::string name;          // aggregate name, upper-case
    std::unique_ptr<Expr> arg; // Star for "*"
};

struct Select;  // forward decl — Subquery wraps a full Select statement

struct Subquery : Expr {
    std::unique_ptr<Select> statement;
};

struct InSubquery : Expr {
    std::unique_ptr<Expr> left;
    std::unique_ptr<Subquery> subquery;
    bool negated = false;
};

struct Coalesce : Expr {
    std::vector<std::unique_ptr<Expr>> args;
};

struct CaseWhen : Expr {
    std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> branches;
    std::unique_ptr<Expr> elseExpr;  // nullptr if absent
};

// ---- database-level statements ----
struct CreateDatabase : Statement { std::string name; };
struct DropDatabase : Statement { std::string name; };
struct UseDatabase : Statement { std::string name; };
struct ShowTables : Statement {};
struct Describe : Statement { std::string table; };
struct CreateView : Statement { std::string name; std::string queryText; };
struct DropView : Statement { std::string name; };
struct ShowViews : Statement {};

// ---- DDL ----
struct ColumnDef {
    std::string name;
    std::string typeName;
    bool primaryKey = false;
    bool notNull = false;
    bool unique = false;
    std::optional<Value> defaultValue;
    std::optional<int> maxLength;
    std::optional<std::string> refTable;
    std::optional<std::string> refColumn;
    std::optional<std::string> check;  // CHECK source text
};

struct CreateTable : Statement {
    std::string name;
    std::vector<ColumnDef> columns;
    std::vector<std::vector<std::string>> compositeUnique;
    std::optional<std::vector<std::string>> compositePk;
};

struct AlterAddComposite : Statement {
    std::string table;
    std::string kind;  // "ANOKHA" | "MUKHYA"
    std::vector<std::string> columns;
};

struct DropTable : Statement { std::string name; };
struct AlterAddColumn : Statement { std::string table; ColumnDef column; };
struct AlterDropColumn : Statement { std::string table; std::string column; };
struct RenameTable : Statement { std::string table; std::string newName; };
struct RenameColumn : Statement { std::string table; std::string column; std::string newName; };
struct TruncateTable : Statement { std::string name; };
struct CompactTable : Statement { std::string name; };

// ---- transactions ----
struct Begin : Statement {};
struct Commit : Statement {};
struct Rollback : Statement {};
struct Explain : Statement { std::unique_ptr<Statement> statement; };

// ---- DML ----
struct Insert : Statement {
    std::string table;
    std::optional<std::vector<std::string>> columns;
    std::vector<std::vector<std::unique_ptr<Expr>>> rows;  // empty if `select` is used instead
    std::unique_ptr<Select> select;                         // nullptr if `rows` is used instead
    std::optional<std::vector<std::pair<std::string, std::unique_ptr<Expr>>>> onConflictUpdate;
};

struct OrderItem { std::unique_ptr<Expr> expr; bool descending = false; };

struct Join {
    std::string table;
    std::string alias;
    std::unique_ptr<Expr> on;  // nullptr for NATURAL (synthesized later by the planner)
    std::string kind = "INNER";  // INNER|LEFT|RIGHT|FULL|NATURAL
};

struct Select : Statement {
    std::vector<std::unique_ptr<Expr>> columns;
    std::string table;
    std::optional<std::string> alias;
    std::vector<Join> joins;
    std::unique_ptr<Expr> where;
    std::vector<std::unique_ptr<Expr>> groupBy;
    std::unique_ptr<Expr> having;
    std::vector<OrderItem> orderBy;
    std::optional<int> limit;
    bool distinct = false;
    std::vector<std::optional<std::string>> aliases;  // parallel to `columns`
};

struct SetOp : Statement {
    std::string op;  // SANYUKT | SAAJHA | CHHODKAR
    std::unique_ptr<Statement> left;
    std::unique_ptr<Statement> right;
};

struct Update : Statement {
    std::string table;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments;
    std::unique_ptr<Expr> where;
};

struct Delete : Statement {
    std::string table;
    std::unique_ptr<Expr> where;
};

}  // namespace meradb::ast
```

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R ast`
Expected: all `[ast]` tests PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/include/meradb/ast.h cpp/tests/test_ast.cpp cpp/tests/CMakeLists.txt
git commit -m "Add C++ AST node definitions"
```

---

### Task 6: Parser — expressions

**Files:**
- Create: `cpp/include/meradb/parser.h`
- Create: `cpp/src/parser.cpp`
- Test: `cpp/tests/test_parser_expressions.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::Token`/`TokenType` (Task 4), every `meradb::ast::*`
  type (Task 5), `meradb::ParseError` (Task 2).
- Produces: `meradb::Parser` class with `parseScript() ->
  std::vector<std::unique_ptr<ast::Statement>>`, and free functions
  `meradb::parseScript(text) -> ...` / `meradb::parseExpression(text) ->
  std::unique_ptr<ast::Expr>` (the latter used later by the engine to
  re-parse stored CHECK/view source text — no caching in Phase 1; add an
  LRU cache only if profiling later shows it matters). This task
  implements only the **expression** grammar (`_parse_or` down to
  `_parse_primary` in the Python source); Task 7/8 add statement-level
  parsing that calls into these expression methods for `JAHAN`/`RAKHO`/
  column-list expressions.
- This task's `Parser` class is extended in-place by Tasks 7 and 8 (same
  class, more methods) — it is not a separate parser per task.

Precedence, lowest to highest (mirrors `parser.py` exactly):
`parseOr` (YA) → `parseAnd` (AUR) → `parseNot` (prefix NAHI) →
`parseComparison` (`= != < <= > >=`, `HAI [NAHI] KHALI`, `JAISA`,
`BEECH...AUR`, `MEIN (...)`) → `parseAdditive` (`+ -`) → `parseTerm`
(`* / %`) → `parseUnary` (prefix `-`) → `parsePrimary` (literals, idents,
function calls, parenthesized expr/subquery, `AGAR...KHATAM` CASE,
`PEHLA(...)` COALESCE).

Desugaring to replicate exactly:
- `x BEECH a AUR b` → `BinaryOp("AUR", BinaryOp(">=",x,a), BinaryOp("<=",x,b))`
- `x MEIN (a, b, c)` (literal list) → chain of `=`/`YA` BinaryOps
- `x MEIN (DIKHAO ...)` → `InSubquery`
- Unary minus directly on a numeric literal folds into a negative `Literal`
  immediately (not a runtime `UnaryOp`) — matches Python's parser-time fold.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_parser_expressions.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/parser.h"
#include "meradb/errors.h"

using namespace meradb;
using namespace meradb::ast;

TEST_CASE("parseExpression parses arithmetic with correct precedence", "[parser][expr]") {
    auto e = parseExpression("1 + 2 * 3");
    auto* bin = dynamic_cast<BinaryOp*>(e.get());
    REQUIRE(bin != nullptr);
    REQUIRE(bin->op == "+");
    REQUIRE(dynamic_cast<Literal*>(bin->left.get()) != nullptr);
    REQUIRE(dynamic_cast<BinaryOp*>(bin->right.get()) != nullptr);  // "2 * 3" binds tighter
}

TEST_CASE("parseExpression folds unary minus on numeric literals", "[parser][expr]") {
    auto e = parseExpression("-5");
    auto* lit = dynamic_cast<Literal*>(e.get());
    REQUIRE(lit != nullptr);
    REQUIRE(std::get<int64_t>(lit->value.data) == -5);
}

TEST_CASE("parseExpression desugars BEECH into AND of range comparisons", "[parser][expr]") {
    auto e = parseExpression("umar BEECH 18 AUR 25");
    auto* outer = dynamic_cast<BinaryOp*>(e.get());
    REQUIRE(outer != nullptr);
    REQUIRE(outer->op == "AUR");
    REQUIRE(dynamic_cast<BinaryOp*>(outer->left.get())->op == ">=");
    REQUIRE(dynamic_cast<BinaryOp*>(outer->right.get())->op == "<=");
}

TEST_CASE("parseExpression desugars MEIN literal list into OR chain", "[parser][expr]") {
    auto e = parseExpression("shehar MEIN ('Delhi', 'Pune')");
    auto* outer = dynamic_cast<BinaryOp*>(e.get());
    REQUIRE(outer != nullptr);
    REQUIRE(outer->op == "YA");
}

TEST_CASE("parseExpression parses IS NULL and IS NOT NULL", "[parser][expr]") {
    auto e1 = parseExpression("umar HAI KHALI");
    REQUIRE(dynamic_cast<IsNull*>(e1.get())->negated == false);
    auto e2 = parseExpression("umar HAI NAHI KHALI");
    REQUIRE(dynamic_cast<IsNull*>(e2.get())->negated == true);
}

TEST_CASE("parseExpression parses CASE WHEN with WARNA else and KHATAM end", "[parser][expr]") {
    auto e = parseExpression("AGAR umar < 18 TAB 'minor' WARNA 'adult' KHATAM");
    auto* cw = dynamic_cast<CaseWhen*>(e.get());
    REQUIRE(cw != nullptr);
    REQUIRE(cw->branches.size() == 1);
    REQUIRE(cw->elseExpr != nullptr);
}

TEST_CASE("parseExpression parses PEHLA as COALESCE", "[parser][expr]") {
    auto e = parseExpression("PEHLA(grade, 'Ungraded')");
    auto* c = dynamic_cast<Coalesce*>(e.get());
    REQUIRE(c != nullptr);
    REQUIRE(c->args.size() == 2);
}

TEST_CASE("parseExpression parses aggregate function calls", "[parser][expr]") {
    auto e = parseExpression("GINO(*)");
    auto* f = dynamic_cast<FuncCall*>(e.get());
    REQUIRE(f != nullptr);
    REQUIRE(f->name == "GINO");
    REQUIRE(dynamic_cast<Star*>(f->arg.get()) != nullptr);
}

TEST_CASE("parseExpression throws ParseError on malformed input", "[parser][expr]") {
    REQUIRE_THROWS_AS(parseExpression("1 + "), ParseError);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_parser_expressions.cpp` to `cpp/tests/CMakeLists.txt`. Run:
```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/parser.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
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
    // (this task) is usable standalone via parseExpressionEntry() for
    // parseExpression() below, and internally by every statement parser.
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
    [[noreturn]] void error(const std::string& msg) const;

    // expression grammar, lowest to highest precedence
    std::unique_ptr<ast::Expr> parseOr();
    std::unique_ptr<ast::Expr> parseAnd();
    std::unique_ptr<ast::Expr> parseNot();
    std::unique_ptr<ast::Expr> parseComparison();
    std::unique_ptr<ast::Expr> parseAdditive();
    std::unique_ptr<ast::Expr> parseTerm();
    std::unique_ptr<ast::Expr> parseUnary();
    std::unique_ptr<ast::Expr> parsePrimary();

    // helpers used across Task 7/8 too (declared here, defined here or there)
    std::unique_ptr<ast::Select> parseSelectBody();  // implemented in Task 8
    std::unique_ptr<ast::Statement> parseStatement(); // implemented in Task 7/8

    friend std::unique_ptr<ast::Expr> ::meradb::parseExpression(const std::string&);
};

std::vector<std::unique_ptr<ast::Statement>> parseScript(const std::string& text);
std::unique_ptr<ast::Expr> parseExpression(const std::string& text);

}  // namespace meradb
```

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/parser.cpp  (expression-parsing portion; parseScript/parseStatement
// bodies are filled in by Task 7/8 — this task defines them minimally so the
// file links, throwing if a non-expression entry point is hit prematurely)
#include "meradb/parser.h"
#include "meradb/errors.h"

namespace meradb {

using namespace ast;

Parser::Parser(std::vector<Token> tokens, std::string sourceText)
    : tokens_(std::move(tokens)), sourceText_(std::move(sourceText)) {}

const Token& Parser::peek(int offset) const {
    size_t i = pos_ + static_cast<size_t>(offset);
    return i < tokens_.size() ? tokens_[i] : tokens_.back();  // back() is Eof
}
const Token& Parser::advance() { return tokens_[pos_ < tokens_.size() - 1 ? pos_++ : pos_]; }

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
[[noreturn]] void Parser::error(const std::string& msg) const {
    throw ParseError(msg + " (line " + std::to_string(peek().line) + ")");
}

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
        auto operand = parseNot();
        auto u = std::make_unique<UnaryOp>();
        u->op = "NAHI"; u->operand = std::move(operand);
        return u;
    }
    return parseComparison();
}

std::unique_ptr<Expr> Parser::parseComparison() {
    auto left = parseAdditive();
    if (matchSymbol("=")) return std::make_unique<BinaryOp>("=", std::move(left), parseAdditive());
    if (matchSymbol("!=")) return std::make_unique<BinaryOp>("!=", std::move(left), parseAdditive());
    if (matchSymbol("<=")) return std::make_unique<BinaryOp>("<=", std::move(left), parseAdditive());
    if (matchSymbol(">=")) return std::make_unique<BinaryOp>(">=", std::move(left), parseAdditive());
    if (matchSymbol("<")) return std::make_unique<BinaryOp>("<", std::move(left), parseAdditive());
    if (matchSymbol(">")) return std::make_unique<BinaryOp>(">", std::move(left), parseAdditive());
    if (matchKeyword("JAISA")) return std::make_unique<BinaryOp>("JAISA", std::move(left), parseAdditive());
    if (matchKeyword("HAI")) {
        bool negated = matchKeyword("NAHI");
        expectKeyword("KHALI");
        auto isnull = std::make_unique<IsNull>();
        isnull->expr = std::move(left);
        isnull->negated = negated;
        return isnull;
    }
    if (matchKeyword("BEECH")) {
        auto lo = parseAdditive();
        expectKeyword("AUR");
        auto hi = parseAdditive();
        // Range desugar: needs two independent copies of `left`. Since Expr
        // trees are move-only (unique_ptr), clone `left` via a small
        // deep-copy helper (cloneExpr, added in this task alongside the
        // parser) rather than trying to share ownership.
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
            return inSub;
        }
        // literal list -> OR chain of equality comparisons
        std::unique_ptr<Expr> chain;
        do {
            auto leftCopy = cloneExpr(*left);
            auto eq = std::make_unique<BinaryOp>("=", std::move(leftCopy), parseAdditive());
            chain = chain ? std::make_unique<BinaryOp>("YA", std::move(chain), std::move(eq)) : std::move(eq);
        } while (matchSymbol(","));
        expectSymbol(")");
        return chain;
    }
    return left;
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
        // Fold unary minus directly on a numeric literal at parse time.
        if (auto* lit = dynamic_cast<Literal*>(operand.get())) {
            if (std::holds_alternative<int64_t>(lit->value.data))
                return std::make_unique<Literal>(Value(-std::get<int64_t>(lit->value.data)));
            if (std::holds_alternative<double>(lit->value.data))
                return std::make_unique<Literal>(Value(-std::get<double>(lit->value.data)));
        }
        auto u = std::make_unique<UnaryOp>();
        u->op = "-"; u->operand = std::move(operand);
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
    if (matchSymbol("*")) return std::make_unique<Star>();
    if (matchKeyword("AGAR")) {
        auto cw = std::make_unique<CaseWhen>();
        do {
            auto cond = parseOr();
            expectKeyword("TAB");
            auto result = parseOr();
            cw->branches.emplace_back(std::move(cond), std::move(result));
        } while (checkKeyword("AGAR") && matchKeyword("AGAR"));  // Python grammar: repeated WHEN via re-entering at "AGAR"? see note
        if (matchKeyword("WARNA")) cw->elseExpr = parseOr();
        expectKeyword("KHATAM");
        return cw;
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
    if (t.type == TokenType::Ident) {
        std::string name = t.textValue;
        advance();
        if (matchSymbol("(")) {
            // function call: aggregate (GINO/KUL/AUSAT/NYUNTAM/ADHIKTAM/
            // aliases) or PEHLA (COALESCE).
            std::string upper = name;
            for (auto& c : upper) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
            if (upper == "PEHLA") {
                auto co = std::make_unique<Coalesce>();
                co->args.push_back(parseOr());
                while (matchSymbol(",")) co->args.push_back(parseOr());
                expectSymbol(")");
                return co;
            }
            auto fc = std::make_unique<FuncCall>();
            fc->name = upper;
            fc->arg = checkSymbol("*") ? (advance(), std::make_unique<Star>()) : parseOr();
            expectSymbol(")");
            return fc;
        }
        if (matchSymbol(".")) {
            std::string col = advance().textValue;
            return std::make_unique<ColumnRef>(col, name);
        }
        return std::make_unique<ColumnRef>(name);
    }
    error("Anjaan expression");
}

// Deep-copy helper needed for BEECH/MEIN desugaring, which must reference
// the same sub-expression twice in an owned (unique_ptr) tree.
std::unique_ptr<Expr> cloneExpr(const Expr& e);  // defined alongside AST utilities in this file;
                                                  // implementation: a dynamic_cast chain mirroring
                                                  // parsePrimary's node types, copy-constructing each.

std::unique_ptr<Expr> Parser::parseExpressionEntry() { return parseOr(); }

std::unique_ptr<Expr> parseExpression(const std::string& text) {
    Parser p(tokenize(text), text);
    auto e = p.parseExpressionEntry();
    return e;
}

}  // namespace meradb
```

Note on the `cloneExpr` helper: implement it as a straightforward
`dynamic_cast` chain (check `Literal`, then `ColumnRef`, then `Star`, then
`BinaryOp` recursively, etc.) that deep-copies whatever expression shape
it's given — this is only ever called on the left-hand side of `BEECH`/
`MEIN`, which in every real query is a simple `ColumnRef` or `Literal`,
but implement it generally (recursing into `BinaryOp`/`UnaryOp`/etc. by
cloning both sides) so it doesn't silently break if a more complex
expression appears there.

Also note: the `AGAR...KHATAM` CASE-WHEN loop condition above
(`checkKeyword("AGAR") && matchKeyword("AGAR")`) is a placeholder shape —
cross-check the actual Python `_parse_primary`'s CASE-WHEN loop (it
repeats on a per-branch keyword, not by re-matching `AGAR`) and correct
this to loop on whatever keyword actually introduces each subsequent
`WHEN` branch in `parser.py` before considering this task done. This is
the one piece of this task where the Explore-agent research did not
capture the exact repetition keyword, so **re-read
`meradb/parser.py`'s CASE-WHEN parsing method directly** before writing
the final version of this loop, rather than guessing.

- [ ] **Step 5: Add `src/parser.cpp` to `meradb_core` in `cpp/CMakeLists.txt`**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "parser.*expr"`
Expected: all `[parser][expr]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/parser.h cpp/src/parser.cpp cpp/tests/test_parser_expressions.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ expression parser"
```

---

### Task 7: Parser — DDL & database statements

**Files:**
- Modify: `cpp/src/parser.cpp` (add methods to the same `Parser` class)
- Test: `cpp/tests/test_parser_ddl.cpp`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Task 6 (`Parser` class, expression parsing).
- Produces: `Parser::parseStatement()` filled in for every DDL/database
  form; `parseScript()` implemented (loop calling `parseStatement()`,
  consuming trailing `;`, stopping at `Eof`).

Statement dispatch table (leading keyword → what gets parsed), matching
`parser.py`'s `_parse_statement`:

| Leading keyword(s) | Produces |
|---|---|
| `BANAO DATABASE <name>` | `CreateDatabase` |
| `HATAO DATABASE <name>` | `DropDatabase` |
| `ISTEMAL <name>` | `UseDatabase` |
| `BANAO TABLE <name> ( <column-defs...> )` | `CreateTable` (see column-def grammar below) |
| `BANAO VIEW <name> DIKHAO ...` | `CreateView` (query text = raw slice from the `DIKHAO` token's `start` to the statement's terminating `;`'s `start`, using `Token::start`/`end` recorded by the tokenizer) |
| `HATAO VIEW <name>` | `DropView` |
| `HATAO TABLE <name>` | `DropTable` |
| `SAAF TABLE <name>` | `TruncateTable` |
| `SIKODO TABLE <name>` | `CompactTable` |
| `SUDHARO TABLE <name> JODO COLUMN <col-def>` | `AlterAddColumn` |
| `SUDHARO TABLE <name> HATAO COLUMN <col>` | `AlterDropColumn` |
| `SUDHARO TABLE <name> JODO ANOKHA (<cols...>)` or `JODO MUKHYA KUNJI (<cols...>)` | `AlterAddComposite` |
| `SUDHARO TABLE <name> NAYA_NAAM <new_name>` | `RenameTable` |
| `SUDHARO TABLE <name> COLUMN <col> NAYA_NAAM <new_name>` | `RenameColumn` |
| `BATAO <name>` (bare table name, no `TABLE` keyword) | `Describe` |
| `BATAO TABLES` | `ShowTables` |
| `BATAO VIEWS` | `ShowViews` |

Column-def grammar (`<column-defs...>` inside `BANAO TABLE (...)`):
`<name> <type>[(<len>)] [MUKHYA KUNJI] [ZAROORI] [ANOKHA] [WARNA <literal>]
[SANDARBH <table>(<col>)] [SHART (<expr>)]`, comma-separated; a
comma-separated list of bare identifiers wrapped in `ANOKHA (...)` or
preceded by `MUKHYA KUNJI (...)` at the top level (not attached to a
single column) becomes `compositeUnique`/`compositePk` on `CreateTable`
instead of a `ColumnDef`.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_parser_ddl.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/parser.h"

using namespace meradb;
using namespace meradb::ast;

TEST_CASE("parseScript parses CREATE TABLE with constraints", "[parser][ddl]") {
    auto stmts = parseScript(
        "BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT ZAROORI, "
        "umar ANK SHART (umar >= 0 AUR umar < 150), active BOOL WARNA SACH);");
    REQUIRE(stmts.size() == 1);
    auto* ct = dynamic_cast<CreateTable*>(stmts[0].get());
    REQUIRE(ct != nullptr);
    REQUIRE(ct->name == "students");
    REQUIRE(ct->columns.size() == 4);
    REQUIRE(ct->columns[0].primaryKey);
    REQUIRE(ct->columns[1].notNull);
    REQUIRE(ct->columns[2].check.has_value());
    REQUIRE(ct->columns[3].defaultValue.has_value());
}

TEST_CASE("parseScript parses composite UNIQUE constraint", "[parser][ddl]") {
    auto stmts = parseScript(
        "BANAO TABLE enroll (student_id INT, course_id INT, ANOKHA (student_id, course_id));");
    auto* ct = dynamic_cast<CreateTable*>(stmts[0].get());
    REQUIRE(ct->compositeUnique.size() == 1);
    REQUIRE(ct->compositeUnique[0] == std::vector<std::string>{"student_id", "course_id"});
}

TEST_CASE("parseScript parses ALTER TABLE ADD/DROP COLUMN", "[parser][ddl]") {
    auto stmts = parseScript("SUDHARO TABLE students JODO COLUMN city TEXT;");
    REQUIRE(dynamic_cast<AlterAddColumn*>(stmts[0].get()) != nullptr);
    auto stmts2 = parseScript("SUDHARO TABLE students HATAO COLUMN city;");
    REQUIRE(dynamic_cast<AlterDropColumn*>(stmts2[0].get()) != nullptr);
}

TEST_CASE("parseScript parses RENAME TABLE and RENAME COLUMN", "[parser][ddl]") {
    auto stmts = parseScript("SUDHARO TABLE students NAYA_NAAM learners;");
    REQUIRE(dynamic_cast<RenameTable*>(stmts[0].get())->newName == "learners");
    auto stmts2 = parseScript("SUDHARO TABLE students COLUMN naam NAYA_NAAM full_name;");
    auto* rc = dynamic_cast<RenameColumn*>(stmts2[0].get());
    REQUIRE(rc->column == "naam");
    REQUIRE(rc->newName == "full_name");
}

TEST_CASE("parseScript captures CREATE VIEW body as raw source text", "[parser][ddl]") {
    auto stmts = parseScript("BANAO VIEW toppers DIKHAO naam SE students JAHAN cgpa > 9;");
    auto* cv = dynamic_cast<CreateView*>(stmts[0].get());
    REQUIRE(cv->name == "toppers");
    REQUIRE(cv->queryText.find("DIKHAO") == 0);
}

TEST_CASE("parseScript parses multiple ;-separated statements", "[parser][ddl]") {
    auto stmts = parseScript("BANAO DATABASE d1; ISTEMAL d1; BATAO TABLES;");
    REQUIRE(stmts.size() == 3);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_parser_ddl.cpp` to `cpp/tests/CMakeLists.txt`. Run and confirm
compile/link failure (methods not yet implemented / statement dispatch
throws).

- [ ] **Step 3: Implement `parseStatement()`, `parseScript()`, and a
  `parseColumnDef()` / `parseCreateTable()` helper family in
  `cpp/src/parser.cpp`, following the dispatch table above.**

Use a series of `if (checkKeyword(...))` branches in `parseStatement()`,
each calling a dedicated private method (`parseCreateStatement()`,
`parseAlterStatement()`, `parseDropStatement()`, `parseDescribeOrShow()`,
etc.), matching the branching structure `parser.py`'s `_parse_statement`
already uses (re-read that method directly when implementing — the
table above gives every *outcome* but the exact keyword-lookahead order
matters for correctness, e.g. distinguishing `BATAO <table>` from `BATAO
TABLES` requires peeking one token past `BATAO` before deciding).

For `CreateView`'s raw source-text capture: record
`size_t bodyStart = peek().start;` right after consuming `BANAO VIEW
<name>`, parse-and-discard the nested `Select` via `parseSelectBody()`
(Task 8) purely to consume the right number of tokens, then slice
`sourceText_.substr(bodyStart, peek(-1).end - bodyStart)` — this exact
"parse to find the boundary, then re-slice raw text" trick is also needed
later for `SHART`/CHECK (Task 7, column-level) source capture.

`parseScript()`:
```cpp
std::vector<std::unique_ptr<Statement>> Parser::parseScript() {
    std::vector<std::unique_ptr<Statement>> result;
    while (peek().type != TokenType::Eof) {
        result.push_back(parseStatement());
        matchSymbol(";");
    }
    return result;
}
std::vector<std::unique_ptr<Statement>> parseScript(const std::string& text) {
    Parser p(tokenize(text), text);
    return p.parseScript();
}
```

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "parser.*ddl"`
Expected: all `[parser][ddl]` tests PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/src/parser.cpp cpp/tests/test_parser_ddl.cpp cpp/tests/CMakeLists.txt
git commit -m "Add C++ parser support for DDL and database statements"
```

---

### Task 8: Parser — DML statements (INSERT/UPDATE/DELETE/SELECT/transactions)

**Files:**
- Modify: `cpp/src/parser.cpp`
- Test: `cpp/tests/test_parser_dml.cpp`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 6/7's `Parser` methods.
- Produces: `Parser::parseSelectBody()` fully implemented (used by Task 6's
  subquery parsing and Task 7's `CREATE VIEW`, both of which stubbed a
  call to it), plus `parseStatement()` branches for `DAALO`, `DIKHAO`
  (top-level, including `SANYUKT`/`SAAJHA`/`CHHODKAR` combining two
  `Select`/`SetOp` results into `SetOp`), `BADLO`, `MITAO`, `SHURU`,
  `PAKKA`, `WAPAS`, `SAMJHAO`.

Grammar per statement:
- `DAALO MEIN <table> [(<cols...>)] MAAN (<expr...>) [, (<expr...>)...]
  [TAKRAAV PAR BADLO <col> = <expr> [, ...]]` → `Insert` with `rows` set.
- `DAALO MEIN <table> [(<cols...>)] <select-body>` → `Insert` with
  `select` set instead of `rows`.
- `DIKHAO [ALAG] <col-expr> [KAHO <alias>] [, ...] SE <table> [<alias>]
  [<joins...>] [JAHAN <expr>] [SAMOOH <expr> [, ...]] [JINKA <expr>]
  [KRAM <expr> [SEEDHA|ULTA] [, ...]] [SIRF <n>]` → `Select`.
  `<joins...>` is zero or more of:
  `[BAAYAN|DAHINA|DONO|SAMAAN] MILAO <table> [<alias>] [PAR <expr>]`
  (`PAR` omitted only for `SAMAAN` NATURAL — the planner synthesizes the
  join condition later, see Task 12).
- Two `Select`/`SetOp` bodies joined by `SANYUKT`/`SAAJHA`/`CHHODKAR`
  combine (left-associatively) into nested `SetOp` nodes.
- `BADLO <table> RAKHO <col> = <expr> [, ...] [JAHAN <expr>]` → `Update`.
- `MITAO SE <table> [JAHAN <expr>]` → `Delete`.
- `SHURU` / `PAKKA` / `WAPAS` (bare, no arguments) → `Begin`/`Commit`/`Rollback`.
- `SAMJHAO <any-statement>` → `Explain` wrapping the recursively-parsed
  inner statement.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_parser_dml.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/parser.h"

using namespace meradb;
using namespace meradb::ast;

TEST_CASE("parseScript parses INSERT with explicit columns", "[parser][dml]") {
    auto stmts = parseScript("DAALO MEIN students (id, naam) MAAN (1, 'Ravi');");
    auto* ins = dynamic_cast<Insert*>(stmts[0].get());
    REQUIRE(ins->table == "students");
    REQUIRE(ins->columns.has_value());
    REQUIRE(ins->columns->size() == 2);
    REQUIRE(ins->rows.size() == 1);
    REQUIRE(ins->rows[0].size() == 2);
}

TEST_CASE("parseScript parses multi-row INSERT", "[parser][dml]") {
    auto stmts = parseScript("DAALO MEIN t MAAN (1), (2), (3);");
    REQUIRE(dynamic_cast<Insert*>(stmts[0].get())->rows.size() == 3);
}

TEST_CASE("parseScript parses INSERT ... SELECT", "[parser][dml]") {
    auto stmts = parseScript("DAALO MEIN backup DIKHAO * SE students;");
    auto* ins = dynamic_cast<Insert*>(stmts[0].get());
    REQUIRE(ins->select != nullptr);
    REQUIRE(ins->rows.empty());
}

TEST_CASE("parseScript parses upsert TAKRAAV PAR BADLO", "[parser][dml]") {
    auto stmts = parseScript("DAALO MEIN t MAAN (1, 'x') TAKRAAV PAR BADLO naam = naam;");
    auto* ins = dynamic_cast<Insert*>(stmts[0].get());
    REQUIRE(ins->onConflictUpdate.has_value());
    REQUIRE(ins->onConflictUpdate->size() == 1);
}

TEST_CASE("parseScript parses SELECT with WHERE/ORDER/LIMIT", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO naam, cgpa SE students JAHAN cgpa > 8 KRAM cgpa ULTA SIRF 5;");
    auto* sel = dynamic_cast<Select*>(stmts[0].get());
    REQUIRE(sel->columns.size() == 2);
    REQUIRE(sel->where != nullptr);
    REQUIRE(sel->orderBy.size() == 1);
    REQUIRE(sel->orderBy[0].descending);
    REQUIRE(sel->limit == 5);
}

TEST_CASE("parseScript parses LEFT/RIGHT/FULL/NATURAL JOIN", "[parser][dml]") {
    auto s1 = parseScript("DIKHAO * SE a BAAYAN MILAO b PAR a.id = b.id;");
    REQUIRE(dynamic_cast<Select*>(s1[0].get())->joins[0].kind == "LEFT");
    auto s2 = parseScript("DIKHAO * SE a DAHINA MILAO b PAR a.id = b.id;");
    REQUIRE(dynamic_cast<Select*>(s2[0].get())->joins[0].kind == "RIGHT");
    auto s3 = parseScript("DIKHAO * SE a DONO MILAO b PAR a.id = b.id;");
    REQUIRE(dynamic_cast<Select*>(s3[0].get())->joins[0].kind == "FULL");
    auto s4 = parseScript("DIKHAO * SE a SAMAAN MILAO b;");
    REQUIRE(dynamic_cast<Select*>(s4[0].get())->joins[0].kind == "NATURAL");
}

TEST_CASE("parseScript parses GROUP BY / HAVING", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO shehar, GINO(*) SE students SAMOOH shehar JINKA GINO(*) > 1;");
    auto* sel = dynamic_cast<Select*>(stmts[0].get());
    REQUIRE(sel->groupBy.size() == 1);
    REQUIRE(sel->having != nullptr);
}

TEST_CASE("parseScript parses set operations into SetOp nodes", "[parser][dml]") {
    auto stmts = parseScript("DIKHAO id SE a SANYUKT DIKHAO id SE b;");
    auto* so = dynamic_cast<SetOp*>(stmts[0].get());
    REQUIRE(so != nullptr);
    REQUIRE(so->op == "SANYUKT");
}

TEST_CASE("parseScript parses UPDATE and DELETE", "[parser][dml]") {
    auto s1 = parseScript("BADLO students RAKHO umar = umar + 1 JAHAN umar HAI NAHI KHALI;");
    auto* upd = dynamic_cast<Update*>(s1[0].get());
    REQUIRE(upd->assignments.size() == 1);
    REQUIRE(upd->where != nullptr);
    auto s2 = parseScript("MITAO SE students JAHAN umar < 0;");
    REQUIRE(dynamic_cast<Delete*>(s2[0].get()) != nullptr);
}

TEST_CASE("parseScript parses bare transaction statements", "[parser][dml]") {
    auto stmts = parseScript("SHURU; PAKKA;");
    REQUIRE(dynamic_cast<Begin*>(stmts[0].get()) != nullptr);
    REQUIRE(dynamic_cast<Commit*>(stmts[1].get()) != nullptr);
}

TEST_CASE("parseScript parses SAMJHAO wrapping a SELECT", "[parser][dml]") {
    auto stmts = parseScript("SAMJHAO DIKHAO * SE students;");
    auto* ex = dynamic_cast<Explain*>(stmts[0].get());
    REQUIRE(ex != nullptr);
    REQUIRE(dynamic_cast<Select*>(ex->statement.get()) != nullptr);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_parser_dml.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure.

- [ ] **Step 3: Implement `parseSelectBody()` and the remaining
  `parseStatement()` branches in `cpp/src/parser.cpp`.**

`parseSelectBody()` structure (called both for top-level `DIKHAO` and for
subqueries/views):
```cpp
std::unique_ptr<Select> Parser::parseSelectBody() {
    expectKeyword("DIKHAO");
    auto sel = std::make_unique<Select>();
    sel->distinct = matchKeyword("ALAG");
    do {
        sel->columns.push_back(parseOr());
        sel->aliases.push_back(matchKeyword("KAHO")
            ? std::optional<std::string>(advance().textValue)
            : std::nullopt);
    } while (matchSymbol(","));
    expectKeyword("SE");
    sel->table = advance().textValue;
    if (peek().type == TokenType::Ident) sel->alias = advance().textValue;  // bare alias, no KAHO
    while (checkKeyword("BAAYAN") || checkKeyword("DAHINA") || checkKeyword("DONO") ||
           checkKeyword("SAMAAN") || checkKeyword("MILAO")) {
        Join j;
        j.kind = matchKeyword("BAAYAN") ? "LEFT" : matchKeyword("DAHINA") ? "RIGHT" :
                 matchKeyword("DONO") ? "FULL" : matchKeyword("SAMAAN") ? "NATURAL" : "INNER";
        expectKeyword("MILAO");
        j.table = advance().textValue;
        if (peek().type == TokenType::Ident) j.alias = advance().textValue; else j.alias = j.table;
        if (matchKeyword("PAR")) j.on = parseOr();
        sel->joins.push_back(std::move(j));
    }
    if (matchKeyword("JAHAN")) sel->where = parseOr();
    if (matchKeyword("SAMOOH")) {
        do { sel->groupBy.push_back(parseOr()); } while (matchSymbol(","));
    }
    if (matchKeyword("JINKA")) sel->having = parseOr();
    if (matchKeyword("KRAM")) {
        do {
            OrderItem oi;
            oi.expr = parseOr();
            oi.descending = matchKeyword("ULTA");
            if (!oi.descending) matchKeyword("SEEDHA");
            sel->orderBy.push_back(std::move(oi));
        } while (matchSymbol(","));
    }
    if (matchKeyword("SIRF")) sel->limit = static_cast<int>(advance().intValue);
    return sel;
}
```
Then, at the top level, after parsing one `Select` (or a parenthesized
`SetOp`), check for a trailing `SANYUKT`/`SAAJHA`/`CHHODKAR` keyword and
wrap left-associatively into `SetOp` nodes before returning from
`parseStatement()`'s `DIKHAO` branch.

For `Insert`: after `DAALO MEIN <table>`, optionally parse `(<cols...>)`,
then branch on whether the next keyword is `MAAN` (parse one-or-more
comma-separated `(<expr...>)` tuples into `rows`) or `DIKHAO` (parse a
full `parseSelectBody()` into `select`); finally check for trailing
`TAKRAAV PAR BADLO <col> = <expr> [, ...]` into `onConflictUpdate`.

For `Update`/`Delete`/`Begin`/`Commit`/`Rollback`/`Explain`: straightforward
single-branch translations per the grammar table above — re-check
`parser.py`'s corresponding methods for exact keyword order (e.g. whether
`MITAO` requires `SE` or allows a bare table name) before finalizing.

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "parser.*dml"`
Expected: all `[parser][dml]` tests PASS. Also re-run the full suite
(`ctest --test-dir cpp/build`) to confirm Tasks 1-8 all still pass
together.

- [ ] **Step 5: Commit**

```bash
git add cpp/src/parser.cpp cpp/tests/test_parser_dml.cpp cpp/tests/CMakeLists.txt
git commit -m "Complete C++ parser: DML, SELECT, joins, set ops, transactions"
```

---

### Task 9: Catalog module

**Files:**
- Create: `cpp/include/meradb/catalog.h`
- Create: `cpp/src/catalog.cpp`
- Test: `cpp/tests/test_catalog.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::ExecutionError`/`StorageError` (Task 2), nlohmann/json
  (Task 1 dependency).
- Produces: `meradb::Column`, `meradb::TableSchema`, `meradb::Catalog`
  classes. `Catalog::get(table) -> TableSchema&` (throws if missing),
  `Catalog::find(table) -> TableSchema*` (nullptr if missing),
  `Catalog::add(TableSchema)`, `Catalog::remove(table)`,
  `Catalog::addView(name, queryText)`, `Catalog::removeView(name)`,
  `Catalog::views -> const std::unordered_map<std::string,std::string>&`,
  `Catalog::save()`, `Catalog::tablePath(table) -> std::string`. Every
  Table/Engine task in this plan depends on this.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_catalog.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/catalog.h"
#include "meradb/errors.h"
#include <filesystem>

using namespace meradb;

namespace {
std::string tempDbDir() {
    auto dir = std::filesystem::temp_directory_path() / ("meradb_catalog_test_" + std::to_string(rand()));
    std::filesystem::create_directories(dir);
    return dir.string();
}
}

TEST_CASE("Catalog creates and persists a table schema", "[catalog]") {
    auto dir = tempDbDir();
    {
        Catalog cat(dir);
        TableSchema schema;
        schema.name = "students";
        Column id; id.name = "id"; id.typeName = "INT"; id.primaryKey = true;
        Column naam; naam.name = "naam"; naam.typeName = "TEXT"; naam.notNull = true;
        schema.columns = {id, naam};
        cat.add(schema);
    }
    // reload from disk in a fresh Catalog instance
    Catalog cat2(dir);
    REQUIRE(cat2.find("students") != nullptr);
    REQUIRE(cat2.get("students").columns.size() == 2);
    REQUIRE(cat2.get("students").columns[0].primaryKey);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Catalog::get throws ExecutionError for unknown table", "[catalog]") {
    auto dir = tempDbDir();
    Catalog cat(dir);
    REQUIRE_THROWS_AS(cat.get("nope"), ExecutionError);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Catalog persists and reloads views", "[catalog]") {
    auto dir = tempDbDir();
    {
        Catalog cat(dir);
        cat.addView("toppers", "DIKHAO naam SE students JAHAN cgpa > 9;");
    }
    Catalog cat2(dir);
    REQUIRE(cat2.views.count("toppers") == 1);
    REQUIRE(cat2.views.at("toppers").find("DIKHAO") == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("TableSchema::indexOf resolves column position and rejects unknowns", "[catalog]") {
    TableSchema schema;
    schema.name = "t";
    Column a; a.name = "a"; schema.columns.push_back(a);
    Column b; b.name = "b"; schema.columns.push_back(b);
    REQUIRE(schema.indexOf("b") == 1);
    REQUIRE_THROWS_AS(schema.indexOf("z"), ExecutionError);
}

TEST_CASE("Catalog::save writes atomically (no partial file left behind)", "[catalog]") {
    auto dir = tempDbDir();
    Catalog cat(dir);
    TableSchema schema; schema.name = "t";
    cat.add(schema);
    REQUIRE(std::filesystem::exists(std::filesystem::path(dir) / "catalog.json"));
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(dir) / "catalog.json.tmp"));
    std::filesystem::remove_all(dir);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_catalog.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure
(`meradb/catalog.h` missing).

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/catalog.h
#pragma once
#include "meradb/datatypes.h"
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace meradb {

struct Column {
    std::string name;
    std::string typeName;
    bool primaryKey = false;
    bool notNull = false;
    bool unique = false;
    std::optional<Value> defaultValue;
    std::optional<int> maxLength;
    std::optional<std::string> refTable;
    std::optional<std::string> refColumn;
    std::optional<std::string> check;

    bool isUnique() const { return primaryKey || unique; }
    bool isRequired() const { return primaryKey || notNull; }

    nlohmann::json toJson() const;
    static Column fromJson(const nlohmann::json& j);
};

struct TableSchema {
    std::string name;
    std::vector<Column> columns;
    std::vector<std::vector<std::string>> compositeUnique;
    std::optional<std::vector<std::string>> compositePk;

    std::vector<std::string> columnNames() const;
    std::vector<std::string> types() const;
    size_t indexOf(const std::string& column) const;   // throws ExecutionError if missing
    const Column& getColumn(const std::string& column) const;

    nlohmann::json toJson() const;
    static TableSchema fromJson(const nlohmann::json& j);
};

class Catalog {
public:
    static constexpr const char* kFileName = "catalog.json";

    explicit Catalog(std::string dbDir);

    std::unordered_map<std::string, TableSchema> tables;
    std::unordered_map<std::string, std::string> views;  // name -> raw SELECT source

    void save();
    std::string tablePath(const std::string& table) const;
    TableSchema& get(const std::string& table);                 // throws ExecutionError
    TableSchema* find(const std::string& table);                 // nullptr if missing
    void add(const TableSchema& schema);                          // sets + saves
    void remove(const std::string& table);
    void addView(const std::string& name, const std::string& queryText);
    void removeView(const std::string& name);

private:
    std::string dbDir_;
    void load();
};

}  // namespace meradb
```

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/catalog.cpp
#include "meradb/catalog.h"
#include "meradb/errors.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace meradb {

namespace {
json valueToJson(const std::optional<Value>& v) {
    if (!v.has_value()) return nullptr;
    if (v->isNull()) return nullptr;
    if (std::holds_alternative<int64_t>(v->data)) return std::get<int64_t>(v->data);
    if (std::holds_alternative<double>(v->data)) return std::get<double>(v->data);
    if (std::holds_alternative<bool>(v->data)) return std::get<bool>(v->data);
    if (std::holds_alternative<std::string>(v->data)) return std::get<std::string>(v->data);
    if (std::holds_alternative<Date>(v->data)) return std::get<Date>(v->data).isoFormat();
    return nullptr;
}
}  // namespace

json Column::toJson() const {
    json j;
    j["name"] = name; j["type_name"] = typeName;
    j["primary_key"] = primaryKey; j["not_null"] = notNull; j["unique"] = unique;
    j["default"] = valueToJson(defaultValue);
    j["max_length"] = maxLength.has_value() ? json(*maxLength) : json(nullptr);
    j["ref_table"] = refTable.has_value() ? json(*refTable) : json(nullptr);
    j["ref_column"] = refColumn.has_value() ? json(*refColumn) : json(nullptr);
    j["check"] = check.has_value() ? json(*check) : json(nullptr);
    return j;
}

Column Column::fromJson(const json& j) {
    Column c;
    c.name = j.at("name"); c.typeName = j.at("type_name");
    c.primaryKey = j.value("primary_key", false);
    c.notNull = j.value("not_null", false);
    c.unique = j.value("unique", false);
    if (!j.at("max_length").is_null()) c.maxLength = j.at("max_length").get<int>();
    if (!j.at("ref_table").is_null()) c.refTable = j.at("ref_table").get<std::string>();
    if (!j.at("ref_column").is_null()) c.refColumn = j.at("ref_column").get<std::string>();
    if (!j.at("check").is_null()) c.check = j.at("check").get<std::string>();
    // `default` is intentionally left as std::nullopt here; the engine
    // (Task 16) re-derives a typed Value from the raw JSON default using
    // datatypes::coerce against typeName, the same way Python's Column
    // stores the raw literal and coerces at insert time.
    return c;
}

std::vector<std::string> TableSchema::columnNames() const {
    std::vector<std::string> names;
    for (auto& c : columns) names.push_back(c.name);
    return names;
}
std::vector<std::string> TableSchema::types() const {
    std::vector<std::string> t;
    for (auto& c : columns) t.push_back(c.typeName);
    return t;
}
size_t TableSchema::indexOf(const std::string& column) const {
    for (size_t i = 0; i < columns.size(); ++i) if (columns[i].name == column) return i;
    throw ExecutionError("'" + column + "' naam ka column '" + name + "' mein nahi mila");
}
const Column& TableSchema::getColumn(const std::string& column) const {
    return columns[indexOf(column)];
}

json TableSchema::toJson() const {
    json j;
    j["name"] = name;
    j["columns"] = json::array();
    for (auto& c : columns) j["columns"].push_back(c.toJson());
    j["composite_unique"] = compositeUnique;
    j["composite_pk"] = compositePk.has_value() ? json(*compositePk) : json(nullptr);
    return j;
}
TableSchema TableSchema::fromJson(const json& j) {
    TableSchema s;
    s.name = j.at("name");
    for (auto& cj : j.at("columns")) s.columns.push_back(Column::fromJson(cj));
    s.compositeUnique = j.value("composite_unique", std::vector<std::vector<std::string>>{});
    if (j.contains("composite_pk") && !j.at("composite_pk").is_null())
        s.compositePk = j.at("composite_pk").get<std::vector<std::string>>();
    return s;
}

Catalog::Catalog(std::string dbDir) : dbDir_(std::move(dbDir)) { load(); }

void Catalog::load() {
    fs::path path = fs::path(dbDir_) / kFileName;
    if (!fs::exists(path)) return;  // fresh database: empty catalog
    std::ifstream in(path);
    json j; in >> j;
    for (auto& [name, tj] : j.at("tables").items()) tables[name] = TableSchema::fromJson(tj);
    for (auto& [name, vj] : j.value("views", json::object()).items()) views[name] = vj.get<std::string>();
}

void Catalog::save() {
    json j;
    j["tables"] = json::object();
    for (auto& [name, schema] : tables) j["tables"][name] = schema.toJson();
    j["views"] = views;
    fs::path path = fs::path(dbDir_) / kFileName;
    fs::path tmp = path; tmp += ".tmp";
    { std::ofstream out(tmp); out << j.dump(2); }
    fs::rename(tmp, path);  // atomic on the same filesystem, matches os.replace()
}

std::string Catalog::tablePath(const std::string& table) const {
    return (fs::path(dbDir_) / (table + ".tbl")).string();
}

TableSchema& Catalog::get(const std::string& table) {
    auto it = tables.find(table);
    if (it == tables.end()) throw ExecutionError("Table '" + table + "' nahi mila");
    return it->second;
}
TableSchema* Catalog::find(const std::string& table) {
    auto it = tables.find(table);
    return it == tables.end() ? nullptr : &it->second;
}
void Catalog::add(const TableSchema& schema) { tables[schema.name] = schema; save(); }
void Catalog::remove(const std::string& table) { tables.erase(table); save(); }
void Catalog::addView(const std::string& name, const std::string& queryText) { views[name] = queryText; save(); }
void Catalog::removeView(const std::string& name) { views.erase(name); save(); }

}  // namespace meradb
```

- [ ] **Step 5: Add `src/catalog.cpp` to `meradb_core` in `cpp/CMakeLists.txt`**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R catalog`
Expected: all `[catalog]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/catalog.h cpp/src/catalog.cpp cpp/tests/test_catalog.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ catalog module"
```

---

### Task 10: Storage module (row codec + heap file)

**Files:**
- Create: `cpp/include/meradb/storage.h`
- Create: `cpp/src/storage.cpp`
- Test: `cpp/tests/test_storage.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::Value`/`Date` (Task 3), `meradb::StorageError` (Task
  2).
- Produces: `meradb::encodeRow(values, types) -> std::vector<uint8_t>`,
  `meradb::decodeRow(payload, types) -> std::vector<Value>`,
  `meradb::HeapFile` class with `create()`, `destroy()`, `truncate()`,
  `insert(payload) -> int64_t` (row id = byte offset), `insertMany(...)`,
  `deleteOne(offset)`, `deleteMany(offsets)`, `read(offset) ->
  std::optional<std::vector<uint8_t>>`, `scan() -> std::vector<std::pair<
  int64_t, std::vector<uint8_t>>>` (materialized, not a lazy iterator, to
  keep Phase 1 simple — the Python version's generator-based `scan()` can
  be revisited for a streaming version later if a huge-table test ever
  demands it), `rewrite(payloads)`, `compact()`. This is byte-for-byte
  compatible with `storage.py`'s format (per the Global Constraints).

Row encoding (per value): 1-byte tag (`0`=NULL, alone; `1`=VALUE followed
by payload). Payload by type: INT = 8-byte little-endian signed
(`int64_t`); FLOAT = 8-byte little-endian IEEE-754 double; BOOL = 1 byte
(0/1); TEXT = 4-byte little-endian unsigned length + UTF-8 bytes; DATE =
4-byte little-endian signed ordinal. Column types are NOT stored per-row
— `decodeRow` needs the schema's type list, matching Python exactly.

Heap file layout (`<table>.tbl`): 8-byte magic header `"MERADB01"`, then
repeated records: 1-byte status (`1`=live, `0`=deleted tombstone) +
4-byte little-endian unsigned length + payload bytes. Row id = byte
offset of the record's status byte.

Atomic write pattern (used by `rewrite`/`compact`, and mirrored in Task 9
for the catalog): write the complete new file to `<path>.tmp`, flush and
close it, then rename over the original — a crash mid-write leaves the
old file untouched.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_storage.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/storage.h"
#include <filesystem>

using namespace meradb;

namespace {
std::string tempTablePath() {
    return (std::filesystem::temp_directory_path() /
            ("meradb_storage_test_" + std::to_string(rand()) + ".tbl")).string();
}
}

TEST_CASE("encodeRow/decodeRow round-trips every type including NULL", "[storage]") {
    std::vector<std::string> types = {"INT", "FLOAT", "TEXT", "BOOL", "DATE"};
    std::vector<Value> values = {
        Value(int64_t{42}), Value(3.5), Value(std::string("hi")), Value(true), Value()};
    auto bytes = encodeRow(values, types);
    auto decoded = decodeRow(bytes, types);
    REQUIRE(std::get<int64_t>(decoded[0].data) == 42);
    REQUIRE(std::get<double>(decoded[1].data) == 3.5);
    REQUIRE(std::get<std::string>(decoded[2].data) == "hi");
    REQUIRE(std::get<bool>(decoded[3].data) == true);
    REQUIRE(decoded[4].isNull());
}

TEST_CASE("HeapFile inserts and reads rows back by row id", "[storage]") {
    auto path = tempTablePath();
    HeapFile hf(path);
    hf.create();
    auto id1 = hf.insert({1, 2, 3});
    auto id2 = hf.insert({4, 5});
    REQUIRE(hf.read(id1).value() == std::vector<uint8_t>{1, 2, 3});
    REQUIRE(hf.read(id2).value() == std::vector<uint8_t>{4, 5});
    std::filesystem::remove(path);
}

TEST_CASE("HeapFile.scan yields only live records", "[storage]") {
    auto path = tempTablePath();
    HeapFile hf(path);
    hf.create();
    auto id1 = hf.insert({1});
    auto id2 = hf.insert({2});
    hf.deleteOne(id1);
    auto rows = hf.scan();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].first == id2);
    std::filesystem::remove(path);
}

TEST_CASE("HeapFile.read returns nullopt for a deleted row", "[storage]") {
    auto path = tempTablePath();
    HeapFile hf(path);
    hf.create();
    auto id = hf.insert({9});
    hf.deleteOne(id);
    REQUIRE_FALSE(hf.read(id).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("HeapFile.compact drops tombstones and keeps live rows readable", "[storage]") {
    auto path = tempTablePath();
    HeapFile hf(path);
    hf.create();
    auto id1 = hf.insert({1});
    hf.insert({2});
    hf.deleteOne(id1);
    hf.compact();
    auto rows = hf.scan();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].second == std::vector<uint8_t>{2});
    std::filesystem::remove(path);
}

TEST_CASE("HeapFile.rewrite is atomic: no .tmp left behind after success", "[storage]") {
    auto path = tempTablePath();
    HeapFile hf(path);
    hf.create();
    hf.insert({1});
    hf.rewrite({{7, 7, 7}});
    REQUIRE_FALSE(std::filesystem::exists(path + ".tmp"));
    auto rows = hf.scan();
    REQUIRE(rows[0].second == std::vector<uint8_t>{7, 7, 7});
    std::filesystem::remove(path);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_storage.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/storage.h
#pragma once
#include "meradb/datatypes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace meradb {

std::vector<uint8_t> encodeRow(const std::vector<Value>& values, const std::vector<std::string>& types);
std::vector<Value> decodeRow(const std::vector<uint8_t>& payload, const std::vector<std::string>& types);

class HeapFile {
public:
    explicit HeapFile(std::string path);
    void create();
    void destroy();
    void truncate();
    int64_t insert(const std::vector<uint8_t>& payload);
    std::vector<int64_t> insertMany(const std::vector<std::vector<uint8_t>>& payloads);
    void deleteOne(int64_t offset);
    void deleteMany(const std::vector<int64_t>& offsets);
    std::optional<std::vector<uint8_t>> read(int64_t offset) const;
    std::vector<std::pair<int64_t, std::vector<uint8_t>>> scan() const;
    void rewrite(const std::vector<std::vector<uint8_t>>& payloads);
    void compact();

private:
    std::string path_;
};

}  // namespace meradb
```

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/storage.cpp
#include "meradb/storage.h"
#include "meradb/errors.h"
#include <cstring>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace meradb {

namespace {
constexpr const char* kMagic = "MERADB01";
constexpr size_t kMagicLen = 8;

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
uint32_t getU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void putI64(std::vector<uint8_t>& out, int64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(v) >> (8 * i)) & 0xFF));
}
int64_t getI64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return static_cast<int64_t>(v);
}
void putI32(std::vector<uint8_t>& out, int32_t v) { putU32(out, static_cast<uint32_t>(v)); }
int32_t getI32(const uint8_t* p) { return static_cast<int32_t>(getU32(p)); }
void putDouble(std::vector<uint8_t>& out, double v) {
    uint64_t bits; std::memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
}
double getDouble(const uint8_t* p) {
    uint64_t bits = static_cast<uint64_t>(getI64(p));
    double v; std::memcpy(&v, &bits, 8);
    return v;
}
}  // namespace

std::vector<uint8_t> encodeRow(const std::vector<Value>& values, const std::vector<std::string>& types) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < values.size(); ++i) {
        const Value& v = values[i];
        if (v.isNull()) { out.push_back(0); continue; }
        out.push_back(1);
        const std::string& type = types[i];
        if (type == "INT") putI64(out, std::get<int64_t>(v.data));
        else if (type == "FLOAT") putDouble(out, std::get<double>(v.data));
        else if (type == "BOOL") out.push_back(std::get<bool>(v.data) ? 1 : 0);
        else if (type == "TEXT") {
            const std::string& s = std::get<std::string>(v.data);
            putU32(out, static_cast<uint32_t>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
        } else if (type == "DATE") putI32(out, std::get<Date>(v.data).toOrdinal());
        else throw StorageError("Anjaan column type encode karte waqt: " + type);
    }
    return out;
}

std::vector<Value> decodeRow(const std::vector<uint8_t>& payload, const std::vector<std::string>& types) {
    std::vector<Value> values;
    size_t pos = 0;
    for (auto& type : types) {
        uint8_t tag = payload[pos++];
        if (tag == 0) { values.push_back(Value()); continue; }
        if (type == "INT") { values.push_back(Value(getI64(&payload[pos]))); pos += 8; }
        else if (type == "FLOAT") { values.push_back(Value(getDouble(&payload[pos]))); pos += 8; }
        else if (type == "BOOL") { values.push_back(Value(payload[pos] != 0)); pos += 1; }
        else if (type == "TEXT") {
            uint32_t len = getU32(&payload[pos]); pos += 4;
            values.push_back(Value(std::string(reinterpret_cast<const char*>(&payload[pos]), len)));
            pos += len;
        } else if (type == "DATE") { values.push_back(Value(Date::fromOrdinal(getI32(&payload[pos])))); pos += 4; }
        else throw StorageError("Anjaan column type decode karte waqt: " + type);
    }
    return values;
}

HeapFile::HeapFile(std::string path) : path_(std::move(path)) {}

void HeapFile::create() {
    std::ofstream out(path_, std::ios::binary);
    out.write(kMagic, static_cast<std::streamsize>(kMagicLen));
}
void HeapFile::destroy() { fs::remove(path_); }
void HeapFile::truncate() { destroy(); create(); }

int64_t HeapFile::insert(const std::vector<uint8_t>& payload) {
    return insertMany({payload})[0];
}

std::vector<int64_t> HeapFile::insertMany(const std::vector<std::vector<uint8_t>>& payloads) {
    std::vector<int64_t> ids;
    std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0, std::ios::end);
    for (auto& payload : payloads) {
        int64_t offset = static_cast<int64_t>(f.tellp());
        ids.push_back(offset);
        std::vector<uint8_t> header;
        header.push_back(1);  // status = live
        putU32(header, static_cast<uint32_t>(payload.size()));
        f.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        f.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    return ids;
}

void HeapFile::deleteOne(int64_t offset) { deleteMany({offset}); }

void HeapFile::deleteMany(const std::vector<int64_t>& offsets) {
    std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
    for (auto offset : offsets) {
        f.seekp(offset, std::ios::beg);
        char zero = 0;
        f.write(&zero, 1);  // flip status byte to 0 (tombstone), in place
    }
}

std::optional<std::vector<uint8_t>> HeapFile::read(int64_t offset) const {
    std::ifstream f(path_, std::ios::binary);
    f.seekg(offset, std::ios::beg);
    char status; f.read(&status, 1);
    if (!f || status == 0) return std::nullopt;
    uint8_t lenBuf[4]; f.read(reinterpret_cast<char*>(lenBuf), 4);
    uint32_t len = getU32(lenBuf);
    std::vector<uint8_t> payload(len);
    f.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(len));
    return payload;
}

std::vector<std::pair<int64_t, std::vector<uint8_t>>> HeapFile::scan() const {
    std::vector<std::pair<int64_t, std::vector<uint8_t>>> result;
    std::ifstream f(path_, std::ios::binary);
    f.seekg(static_cast<std::streamoff>(kMagicLen), std::ios::beg);
    while (true) {
        int64_t offset = static_cast<int64_t>(f.tellg());
        char status;
        if (!f.read(&status, 1)) break;
        uint8_t lenBuf[4]; f.read(reinterpret_cast<char*>(lenBuf), 4);
        uint32_t len = getU32(lenBuf);
        std::vector<uint8_t> payload(len);
        f.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(len));
        if (status != 0) result.emplace_back(offset, std::move(payload));
    }
    return result;
}

void HeapFile::rewrite(const std::vector<std::vector<uint8_t>>& payloads) {
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write(kMagic, static_cast<std::streamsize>(kMagicLen));
        for (auto& payload : payloads) {
            std::vector<uint8_t> header;
            header.push_back(1);
            putU32(header, static_cast<uint32_t>(payload.size()));
            out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
            out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        }
    }
    fs::rename(tmp, path_);  // atomic, matches os.replace()
}

void HeapFile::compact() {
    std::vector<std::vector<uint8_t>> live;
    for (auto& [offset, payload] : scan()) { (void)offset; live.push_back(payload); }
    rewrite(live);
}

}  // namespace meradb
```

- [ ] **Step 5: Add `src/storage.cpp` to `meradb_core` in `cpp/CMakeLists.txt`**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R storage`
Expected: all `[storage]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/storage.h cpp/src/storage.cpp cpp/tests/test_storage.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ storage module (row codec + heap file)"
```

---

### Task 11: Table module (rows + hash indexes)

**Files:**
- Create: `cpp/include/meradb/table.h`
- Create: `cpp/src/table.cpp`
- Test: `cpp/tests/test_table.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::HeapFile`/`encodeRow`/`decodeRow` (Task 10),
  `meradb::TableSchema` (Task 9), `meradb::Value` (Task 3).
- Produces: `meradb::Table` class — `rows() -> std::vector<std::pair<
  int64_t, std::vector<Value>>>` (full scan, decoded), `get(rowId) ->
  std::optional<std::vector<Value>>`, `insertMany(rows)`,
  `deleteMany(rows)` (needs old values, matching Python, to locate index
  entries), `indexes() -> std::unordered_map<size_t, std::unordered_map<
  ValueKey, int64_t>>&` (lazy-built), `lookup(col, value) ->
  std::vector<std::pair<int64_t, std::vector<Value>>>`,
  `invalidateIndexes()`. Also `meradb::MaterializedTable` (same `rows()`/
  `get()` shape, backed by an in-memory vector, `isView() -> true`, no
  insert/delete). Every planner/evaluator/engine SELECT/DML task depends
  on this.

Since `Value` wraps a `std::variant` and isn't directly hashable via
`std::hash`, add a small `ValueKey` wrapper (or a `struct ValueHash` /
`struct ValueEq` pair usable with `std::unordered_map<Value, int64_t,
ValueHash, ValueEq>`) that hashes/compares by the active alternative —
implement this in `table.h` alongside `Table`, since it's only needed
here (the planner and evaluator compare `Value`s with `==` semantics
that already fall out of `std::variant`'s built-in comparison, no
special hashing needed there).

Index scope, matching `table.py`: one hash index per column with
`isUnique()` true (primary key or `UNIQUE`), built lazily on first
`indexes()` call via one full scan, plus one composite index per
`compositeUnique` group and per `compositePk`, keyed by a
`std::vector<Value>` tuple. Any full-file rewrite (`ALTER`, compact,
truncate, rollback) must call `invalidateIndexes()`; next access rebuilds
via scan. Hash indexes answer only `=`; range predicates always fall back
to full scan (enforced by the planner in Task 12, not here).

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_table.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/table.h"
#include "meradb/storage.h"
#include <filesystem>

using namespace meradb;

namespace {
TableSchema makeSchema() {
    TableSchema s; s.name = "t";
    Column id; id.name = "id"; id.typeName = "INT"; id.primaryKey = true;
    Column naam; naam.name = "naam"; naam.typeName = "TEXT";
    s.columns = {id, naam};
    return s;
}
std::string tempPath() {
    return (std::filesystem::temp_directory_path() /
            ("meradb_table_test_" + std::to_string(rand()) + ".tbl")).string();
}
}

TEST_CASE("Table inserts and scans rows", "[table]") {
    auto path = tempPath();
    HeapFile(path).create();
    auto schema = makeSchema();
    Table t(schema, path);
    t.insertMany({{Value(int64_t{1}), Value(std::string("Ravi"))},
                  {Value(int64_t{2}), Value(std::string("Priya"))}});
    auto rows = t.rows();
    REQUIRE(rows.size() == 2);
    std::filesystem::remove(path);
}

TEST_CASE("Table.lookup uses the primary-key hash index", "[table]") {
    auto path = tempPath();
    HeapFile(path).create();
    auto schema = makeSchema();
    Table t(schema, path);
    t.insertMany({{Value(int64_t{1}), Value(std::string("Ravi"))},
                  {Value(int64_t{2}), Value(std::string("Priya"))}});
    auto hits = t.lookup(0, Value(int64_t{2}));
    REQUIRE(hits.size() == 1);
    REQUIRE(std::get<std::string>(hits[0].second[1].data) == "Priya");
    REQUIRE(t.lookup(0, Value(int64_t{999})).empty());
}

TEST_CASE("Table.deleteMany removes rows and updates the index", "[table]") {
    auto path = tempPath();
    HeapFile(path).create();
    auto schema = makeSchema();
    Table t(schema, path);
    t.insertMany({{Value(int64_t{1}), Value(std::string("Ravi"))}});
    auto rows = t.rows();
    t.deleteMany(rows);
    REQUIRE(t.rows().empty());
    REQUIRE(t.lookup(0, Value(int64_t{1})).empty());
    std::filesystem::remove(path);
}

TEST_CASE("Table.invalidateIndexes forces a rebuild on next access", "[table]") {
    auto path = tempPath();
    HeapFile(path).create();
    auto schema = makeSchema();
    Table t(schema, path);
    t.insertMany({{Value(int64_t{1}), Value(std::string("Ravi"))}});
    t.lookup(0, Value(int64_t{1}));  // builds the index
    t.invalidateIndexes();
    REQUIRE(t.lookup(0, Value(int64_t{1})).size() == 1);  // rebuilds transparently
    std::filesystem::remove(path);
}

TEST_CASE("MaterializedTable behaves like Table for reads, is flagged as a view", "[table]") {
    std::vector<std::vector<Value>> data = {{Value(int64_t{1})}, {Value(int64_t{2})}};
    MaterializedTable mt(data);
    REQUIRE(mt.isView());
    REQUIRE(mt.rows().size() == 2);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_table.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/table.h
#pragma once
#include "meradb/catalog.h"
#include "meradb/datatypes.h"
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace meradb {

// Hashes/compares a Value (or a tuple of Values for composite indexes) by
// its active variant alternative — needed because std::variant itself
// isn't usable as an unordered_map key without this.
struct ValueVecHash {
    size_t operator()(const std::vector<Value>& key) const;
};
struct ValueVecEq {
    bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const;
};

class Table {
public:
    Table(TableSchema schema, std::string path);

    virtual ~Table() = default;
    virtual std::vector<std::pair<int64_t, std::vector<Value>>> rows() const;
    virtual std::optional<std::vector<Value>> get(int64_t rowId) const;
    void insertMany(const std::vector<std::vector<Value>>& newRows);
    void deleteMany(const std::vector<std::pair<int64_t, std::vector<Value>>>& oldRows);

    // indexes[columnPositions] -> {key -> rowId}; columnPositions.size()==1
    // for a plain column index, >1 for a composite one.
    using IndexMap = std::unordered_map<std::vector<size_t>, std::unordered_map<
        std::vector<Value>, int64_t, ValueVecHash, ValueVecEq>, /* hashed by a
        simple vector<size_t> key via a small local helper, see .cpp */ struct VecSizeTHash>;

    std::vector<std::pair<int64_t, std::vector<Value>>> lookup(size_t column, const Value& value);
    void invalidateIndexes();
    virtual bool isView() const { return false; }

    const TableSchema& schema() const { return schema_; }

private:
    TableSchema schema_;
    std::string path_;
    std::optional<IndexMap> indexCache_;
    void buildIndexes();
};

class MaterializedTable : public Table {
public:
    explicit MaterializedTable(std::vector<std::vector<Value>> data);
    std::vector<std::pair<int64_t, std::vector<Value>>> rows() const override;
    std::optional<std::vector<Value>> get(int64_t rowId) const override;
    bool isView() const override { return true; }

private:
    std::vector<std::vector<Value>> data_;
};

}  // namespace meradb
```

Note: the `IndexMap` type alias above has an awkward inline `struct
VecSizeTHash` reference that doesn't actually compile as written — when
implementing, define `struct VecSizeTHash { size_t operator()(const
std::vector<size_t>&) const; };` as its own named struct next to
`ValueVecHash`/`ValueVecEq` (same file), then write `IndexMap` as:
```cpp
using IndexMap = std::unordered_map<
    std::vector<size_t>,
    std::unordered_map<std::vector<Value>, int64_t, ValueVecHash, ValueVecEq>,
    VecSizeTHash>;
```
This is called out explicitly because it's the one spot in this task
where the header sketch above needs a small correction before it will
compile — not a design decision left open, just a syntax fix.

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/table.cpp
#include "meradb/table.h"
#include "meradb/storage.h"
#include <functional>

namespace meradb {

size_t ValueVecHash::operator()(const std::vector<Value>& key) const {
    size_t h = key.size();
    for (auto& v : key) {
        size_t part = 0;
        if (std::holds_alternative<int64_t>(v.data)) part = std::hash<int64_t>{}(std::get<int64_t>(v.data));
        else if (std::holds_alternative<double>(v.data)) part = std::hash<double>{}(std::get<double>(v.data));
        else if (std::holds_alternative<bool>(v.data)) part = std::hash<bool>{}(std::get<bool>(v.data));
        else if (std::holds_alternative<std::string>(v.data)) part = std::hash<std::string>{}(std::get<std::string>(v.data));
        else if (std::holds_alternative<Date>(v.data)) part = std::hash<int32_t>{}(std::get<Date>(v.data).toOrdinal());
        h ^= part + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
}
bool ValueVecEq::operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!(a[i].data == b[i].data)) return false;
    return true;
}
struct VecSizeTHash { size_t operator()(const std::vector<size_t>& v) const {
    size_t h = v.size();
    for (auto x : v) h ^= std::hash<size_t>{}(x) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
} };

Table::Table(TableSchema schema, std::string path) : schema_(std::move(schema)), path_(std::move(path)) {}

std::vector<std::pair<int64_t, std::vector<Value>>> Table::rows() const {
    std::vector<std::pair<int64_t, std::vector<Value>>> result;
    HeapFile hf(path_);
    for (auto& [id, payload] : hf.scan()) result.emplace_back(id, decodeRow(payload, schema_.types()));
    return result;
}

std::optional<std::vector<Value>> Table::get(int64_t rowId) const {
    HeapFile hf(path_);
    auto payload = hf.read(rowId);
    if (!payload.has_value()) return std::nullopt;
    return decodeRow(*payload, schema_.types());
}

void Table::insertMany(const std::vector<std::vector<Value>>& newRows) {
    HeapFile hf(path_);
    std::vector<std::vector<uint8_t>> payloads;
    for (auto& row : newRows) payloads.push_back(encodeRow(row, schema_.types()));
    auto ids = hf.insertMany(payloads);
    if (indexCache_.has_value()) {
        for (size_t i = 0; i < newRows.size(); ++i) {
            for (auto& [cols, map] : *indexCache_) {
                std::vector<Value> key;
                for (auto c : cols) key.push_back(newRows[i][c]);
                map[key] = ids[i];
            }
        }
    }
}

void Table::deleteMany(const std::vector<std::pair<int64_t, std::vector<Value>>>& oldRows) {
    HeapFile hf(path_);
    std::vector<int64_t> offsets;
    for (auto& [id, values] : oldRows) offsets.push_back(id);
    hf.deleteMany(offsets);
    if (indexCache_.has_value()) {
        for (auto& [id, values] : oldRows) {
            for (auto& [cols, map] : *indexCache_) {
                std::vector<Value> key;
                for (auto c : cols) key.push_back(values[c]);
                map.erase(key);
            }
        }
    }
}

void Table::buildIndexes() {
    IndexMap idx;
    std::vector<std::vector<size_t>> groups;
    for (size_t i = 0; i < schema_.columns.size(); ++i)
        if (schema_.columns[i].isUnique()) groups.push_back({i});
    for (auto& g : schema_.compositeUnique) {
        std::vector<size_t> positions;
        for (auto& col : g) positions.push_back(schema_.indexOf(col));
        groups.push_back(positions);
    }
    if (schema_.compositePk.has_value()) {
        std::vector<size_t> positions;
        for (auto& col : *schema_.compositePk) positions.push_back(schema_.indexOf(col));
        groups.push_back(positions);
    }
    for (auto& g : groups) idx[g] = {};
    for (auto& [id, values] : rows()) {
        for (auto& g : groups) {
            std::vector<Value> key;
            for (auto c : g) key.push_back(values[c]);
            idx[g][key] = id;
        }
    }
    indexCache_ = std::move(idx);
}

std::vector<std::pair<int64_t, std::vector<Value>>> Table::lookup(size_t column, const Value& value) {
    if (!indexCache_.has_value()) buildIndexes();
    std::vector<size_t> key = {column};
    auto it = indexCache_->find(key);
    if (it == indexCache_->end()) return {};  // column isn't indexed: caller should fall back to a full scan
    auto hit = it->second.find({value});
    if (hit == it->second.end()) return {};
    auto row = get(hit->second);
    if (!row.has_value()) return {};
    return {{hit->second, *row}};
}

void Table::invalidateIndexes() { indexCache_.reset(); }

MaterializedTable::MaterializedTable(std::vector<std::vector<Value>> data)
    : Table(TableSchema{}, ""), data_(std::move(data)) {}

std::vector<std::pair<int64_t, std::vector<Value>>> MaterializedTable::rows() const {
    std::vector<std::pair<int64_t, std::vector<Value>>> result;
    for (size_t i = 0; i < data_.size(); ++i) result.emplace_back(static_cast<int64_t>(i), data_[i]);
    return result;
}
std::optional<std::vector<Value>> MaterializedTable::get(int64_t rowId) const {
    if (rowId < 0 || static_cast<size_t>(rowId) >= data_.size()) return std::nullopt;
    return data_[static_cast<size_t>(rowId)];
}

}  // namespace meradb
```

- [ ] **Step 5: Add `src/table.cpp` to `meradb_core` in `cpp/CMakeLists.txt`**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R table`
Expected: all `[table]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/table.h cpp/src/table.cpp cpp/tests/test_table.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ table module (rows + hash indexes)"
```

---

### Task 12: Planner module (Scope, binding, access/join strategy)

**Files:**
- Create: `cpp/include/meradb/planner.h`
- Create: `cpp/src/planner.cpp`
- Test: `cpp/tests/test_planner.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::ast::*` (Task 5), `meradb::TableSchema`/`Column`
  (Task 9), `meradb::Table` (Task 11), `meradb::ExecutionError` (Task 2).
- Produces: `meradb::Scope` class (`keys(i)`, `row(i, values) ->
  std::unordered_map<std::string, Value>`, `nullRow(i)`, `allKeys()`,
  `sourceIndex(key)`, `display(key)`, `resolve(ColumnRef&) ->
  std::string`, `expandStar(Star&) -> std::vector<std::pair<std::string,
  ast::ColumnRef>>`); `meradb::bind(Expr&, Scope&) ->
  std::unique_ptr<ast::Expr>` (recursive copy-rewrite: every `ColumnRef`
  becomes a fully-qualified `"alias.col"` `ColumnRef`); `meradb::
  naturalJoinCondition(Scope&, size_t rightIndex) -> std::unique_ptr<
  ast::Expr>`; `meradb::conjuncts(const ast::Expr&) -> std::vector<const
  ast::Expr*>` (splits an `AUR`-chain); `meradb::IndexLookup` struct +
  `meradb::chooseAccess(Table&, Scope&, const ast::Expr* where) ->
  std::optional<IndexLookup>`; `meradb::chooseJoin(const ast::Expr* on,
  Scope&, size_t rightIndex) -> std::optional<std::pair<std::string,
  std::string>>` (hash-join keys, else nested-loop).

This task is the most subtle part of the whole port to get right, since
correctness depends entirely on the planner never *changing* query
results — every access-path/join-strategy decision here is purely a
performance optimization on top of what a full scan + nested-loop join
would already produce correctly. If in doubt about a specific rule,
re-read `meradb/planner.py` directly rather than guessing; the shapes
below are the ones the Explore-agent research already confirmed.

`chooseAccess`: walks `conjuncts(where)`; if any conjunct is
`ColumnRef(unique, table=firstSource) = Literal`, and that column
`isUnique()`, return an `IndexLookup{column, columnName, value}` — but
the engine (Task 18) must still re-check the FULL `where` afterward
(this can only speed things up, never change the answer). Returns
`std::nullopt` unconditionally when the first source `isView()` (views
are always full-scanned).

`chooseJoin`: walks `conjuncts(on)` for an equality between a column of
an already-joined source and a column of the source currently being
joined; if found, returns `(leftKey, rightKey)` so the engine can hash-join
(build a `std::unordered_map` from one side keyed by that column, probe
from the other); otherwise `std::nullopt` (nested loop).

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_planner.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/planner.h"
#include "meradb/parser.h"
#include "meradb/table.h"
#include <filesystem>

using namespace meradb;
using namespace meradb::ast;

namespace {
TableSchema studentsSchema() {
    TableSchema s; s.name = "students";
    Column id; id.name = "id"; id.typeName = "INT"; id.primaryKey = true;
    Column naam; naam.name = "naam"; naam.typeName = "TEXT";
    s.columns = {id, naam};
    return s;
}
}

TEST_CASE("Scope resolves an unqualified column to alias.col", "[planner]") {
    Scope scope({{"s", studentsSchema()}});
    ColumnRef ref("naam");
    REQUIRE(scope.resolve(ref) == "s.naam");
}

TEST_CASE("Scope::resolve throws on unknown column", "[planner]") {
    Scope scope({{"s", studentsSchema()}});
    ColumnRef ref("nope");
    REQUIRE_THROWS(scope.resolve(ref));
}

TEST_CASE("bind rewrites every ColumnRef to its qualified key", "[planner]") {
    Scope scope({{"s", studentsSchema()}});
    auto expr = parseExpression("naam");
    auto bound = bind(*expr, scope);
    auto* ref = dynamic_cast<ColumnRef*>(bound.get());
    REQUIRE(ref->table.value() == "s");
    REQUIRE(ref->name == "naam");
}

TEST_CASE("conjuncts splits an AND-chain into individual predicates", "[planner]") {
    auto expr = parseExpression("a = 1 AUR b = 2 AUR c = 3");
    auto parts = conjuncts(*expr);
    REQUIRE(parts.size() == 3);
}

TEST_CASE("chooseAccess finds an index lookup on a unique-column equality", "[planner]") {
    auto path = (std::filesystem::temp_directory_path() / "meradb_planner_test.tbl").string();
    HeapFile(path).create();
    auto schema = studentsSchema();
    Table t(schema, path);
    t.insertMany({{Value(int64_t{1}), Value(std::string("Ravi"))}});
    Scope scope({{"s", schema}});
    auto where = parseExpression("id = 1");
    auto access = chooseAccess(t, scope, where.get());
    REQUIRE(access.has_value());
    REQUIRE(access->columnName == "id");
    std::filesystem::remove(path);
}

TEST_CASE("chooseAccess returns nullopt when no unique-column equality exists", "[planner]") {
    auto path = (std::filesystem::temp_directory_path() / "meradb_planner_test2.tbl").string();
    HeapFile(path).create();
    auto schema = studentsSchema();
    Table t(schema, path);
    Scope scope({{"s", schema}});
    auto where = parseExpression("naam = 'Ravi'");  // naam isn't unique
    REQUIRE_FALSE(chooseAccess(t, scope, where.get()).has_value());
    std::filesystem::remove(path);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_planner.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/planner.h
#pragma once
#include "meradb/ast.h"
#include "meradb/catalog.h"
#include "meradb/table.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace meradb {

class Scope {
public:
    explicit Scope(std::vector<std::pair<std::string, TableSchema>> sources);
    std::vector<std::string> keys(size_t i) const;
    std::unordered_map<std::string, Value> row(size_t i, const std::vector<Value>& values) const;
    std::unordered_map<std::string, Value> nullRow(size_t i) const;
    std::vector<std::string> allKeys() const;
    size_t sourceIndex(const std::string& key) const;  // "alias.col" -> source position
    std::string display(const std::string& key) const;
    std::string resolve(const ast::ColumnRef& ref) const;  // throws ExecutionError if unknown/ambiguous
    std::vector<std::pair<std::string, ast::ColumnRef>> expandStar(const ast::Star& star) const;

private:
    std::vector<std::pair<std::string, TableSchema>> sources_;
};

std::unique_ptr<ast::Expr> cloneExpr(const ast::Expr& e);  // shared with the parser (Task 6)
std::unique_ptr<ast::Expr> bind(const ast::Expr& expr, const Scope& scope);
std::unique_ptr<ast::Expr> naturalJoinCondition(const Scope& scope, size_t rightIndex);
std::vector<const ast::Expr*> conjuncts(const ast::Expr& expr);

struct IndexLookup {
    size_t column;
    std::string columnName;
    Value value;
    std::string describe() const;
};

std::optional<IndexLookup> chooseAccess(Table& table, const Scope& scope, const ast::Expr* where);
void checkJoinCondition(const ast::Expr* on, const Scope& scope, size_t rightIndex);  // throws if `on` references a not-yet-joined table
std::optional<std::pair<std::string, std::string>> chooseJoin(const ast::Expr* on, const Scope& scope, size_t rightIndex);

}  // namespace meradb
```

- [ ] **Step 4: Write the implementation**

Implement `Scope` by storing `sources_` and building `"alias.col"` keys
on demand (`keys(i)` = `sources_[i].first + "." + colName` for every
column in `sources_[i].second`). `resolve()` scans every source's
`keys()` for an unqualified match (error if zero or more-than-one match,
mirroring Python's ambiguous-column check), or looks up
`ref.table + "." + ref.name` directly when `ref.table` is set.

```cpp
// cpp/src/planner.cpp
#include "meradb/planner.h"
#include "meradb/errors.h"

namespace meradb {
using namespace ast;

Scope::Scope(std::vector<std::pair<std::string, TableSchema>> sources) : sources_(std::move(sources)) {}

std::vector<std::string> Scope::keys(size_t i) const {
    std::vector<std::string> ks;
    for (auto& c : sources_[i].second.columns) ks.push_back(sources_[i].first + "." + c.name);
    return ks;
}
std::unordered_map<std::string, Value> Scope::row(size_t i, const std::vector<Value>& values) const {
    std::unordered_map<std::string, Value> r;
    auto ks = keys(i);
    for (size_t j = 0; j < ks.size(); ++j) r[ks[j]] = values[j];
    return r;
}
std::unordered_map<std::string, Value> Scope::nullRow(size_t i) const {
    std::unordered_map<std::string, Value> r;
    for (auto& k : keys(i)) r[k] = Value();
    return r;
}
std::vector<std::string> Scope::allKeys() const {
    std::vector<std::string> all;
    for (size_t i = 0; i < sources_.size(); ++i) for (auto& k : keys(i)) all.push_back(k);
    return all;
}
size_t Scope::sourceIndex(const std::string& key) const {
    auto dot = key.find('.');
    std::string alias = key.substr(0, dot);
    for (size_t i = 0; i < sources_.size(); ++i) if (sources_[i].first == alias) return i;
    throw ExecutionError("Anjaan table alias: " + alias);
}
std::string Scope::display(const std::string& key) const { return key.substr(key.find('.') + 1); }

std::string Scope::resolve(const ColumnRef& ref) const {
    if (ref.table.has_value()) {
        std::string key = *ref.table + "." + ref.name;
        for (size_t i = 0; i < sources_.size(); ++i)
            if (sources_[i].first == *ref.table) {
                for (auto& k : keys(i)) if (k == key) return key;
                throw ExecutionError("'" + ref.name + "' column '" + *ref.table + "' mein nahi mila");
            }
        throw ExecutionError("Anjaan table alias: " + *ref.table);
    }
    std::string match;
    for (size_t i = 0; i < sources_.size(); ++i) {
        for (auto& c : sources_[i].second.columns) {
            if (c.name == ref.name) {
                if (!match.empty()) throw ExecutionError("'" + ref.name + "' column ambiguous hai");
                match = sources_[i].first + "." + ref.name;
            }
        }
    }
    if (match.empty()) throw ExecutionError("'" + ref.name + "' naam ka column kahin nahi mila");
    return match;
}

std::vector<std::pair<std::string, ColumnRef>> Scope::expandStar(const Star& star) const {
    std::vector<std::pair<std::string, ColumnRef>> result;
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (star.table.has_value() && *star.table != sources_[i].first) continue;
        for (auto& c : sources_[i].second.columns)
            result.emplace_back(sources_[i].first + "." + c.name, ColumnRef(c.name, sources_[i].first));
    }
    return result;
}

std::unique_ptr<Expr> bind(const Expr& expr, const Scope& scope) {
    if (auto* c = dynamic_cast<const ColumnRef*>(&expr)) {
        std::string key = scope.resolve(*c);
        auto dot = key.find('.');
        return std::make_unique<ColumnRef>(key.substr(dot + 1), key.substr(0, dot));
    }
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr))
        return std::make_unique<BinaryOp>(b->op, bind(*b->left, scope), bind(*b->right, scope));
    if (auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
        auto out = std::make_unique<UnaryOp>(); out->op = u->op; out->operand = bind(*u->operand, scope);
        return out;
    }
    if (auto* isn = dynamic_cast<const IsNull*>(&expr)) {
        auto out = std::make_unique<IsNull>(); out->expr = bind(*isn->expr, scope); out->negated = isn->negated;
        return out;
    }
    if (auto* f = dynamic_cast<const FuncCall*>(&expr)) {
        auto out = std::make_unique<FuncCall>(); out->name = f->name;
        out->arg = dynamic_cast<const Star*>(f->arg.get()) ? cloneExpr(*f->arg) : bind(*f->arg, scope);
        return out;
    }
    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) {
        auto out = std::make_unique<Coalesce>();
        for (auto& a : co->args) out->args.push_back(bind(*a, scope));
        return out;
    }
    if (auto* cw = dynamic_cast<const CaseWhen*>(&expr)) {
        auto out = std::make_unique<CaseWhen>();
        for (auto& [cond, res] : cw->branches) out->branches.emplace_back(bind(*cond, scope), bind(*res, scope));
        if (cw->elseExpr) out->elseExpr = bind(*cw->elseExpr, scope);
        return out;
    }
    // Literal, Star, Subquery, InSubquery: no ColumnRef inside needs
    // rewriting at THIS scope (Subquery/InSubquery get their own inner
    // Scope when the engine executes them, in Task 18) — clone as-is.
    return cloneExpr(expr);
}

std::vector<const Expr*> conjuncts(const Expr& expr) {
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr); b && b->op == "AUR") {
        auto left = conjuncts(*b->left);
        auto right = conjuncts(*b->right);
        left.insert(left.end(), right.begin(), right.end());
        return left;
    }
    return {&expr};
}

std::optional<IndexLookup> chooseAccess(Table& table, const Scope& scope, const Expr* where) {
    if (table.isView() || where == nullptr) return std::nullopt;
    for (auto* conjunct : conjuncts(*where)) {
        auto* b = dynamic_cast<const BinaryOp*>(conjunct);
        if (!b || b->op != "=") continue;
        auto* colRef = dynamic_cast<const ColumnRef*>(b->left.get());
        auto* lit = dynamic_cast<const Literal*>(b->right.get());
        if (!colRef || !lit) continue;
        // `where` here is already bound (qualified), so colRef->table is set
        // and must refer to source 0 (chooseAccess is only ever called for
        // the FROM table, not a JOINed one).
        size_t col;
        try { col = table.schema().indexOf(colRef->name); } catch (const ExecutionError&) { continue; }
        if (!table.schema().columns[col].isUnique()) continue;
        return IndexLookup{col, colRef->name, lit->value};
    }
    return std::nullopt;
}

void checkJoinCondition(const Expr* on, const Scope& scope, size_t rightIndex) {
    // Walks `on` for any ColumnRef and verifies scope.sourceIndex(...) <=
    // rightIndex (a JOIN's ON may only reference tables joined so far).
    // Implement via a small recursive Expr walker mirroring `bind`'s
    // dispatch shape, calling scope.resolve/sourceIndex on every ColumnRef
    // found and throwing ExecutionError if any resolves to an index >
    // rightIndex.
    (void)on; (void)scope; (void)rightIndex;
}

std::optional<std::pair<std::string, std::string>> chooseJoin(const Expr* on, const Scope& scope, size_t rightIndex) {
    if (on == nullptr) return std::nullopt;
    for (auto* conjunct : conjuncts(*on)) {
        auto* b = dynamic_cast<const BinaryOp*>(conjunct);
        if (!b || b->op != "=") continue;
        auto* l = dynamic_cast<const ColumnRef*>(b->left.get());
        auto* r = dynamic_cast<const ColumnRef*>(b->right.get());
        if (!l || !r) continue;
        std::string lKey = *l->table + "." + l->name, rKey = *r->table + "." + r->name;
        size_t lIdx = scope.sourceIndex(lKey), rIdx = scope.sourceIndex(rKey);
        if (lIdx == rightIndex && rIdx != rightIndex) return {{rKey, lKey}};
        if (rIdx == rightIndex && lIdx != rightIndex) return {{lKey, rKey}};
    }
    return std::nullopt;
}

}  // namespace meradb
```

Note: `checkJoinCondition`'s body above is intentionally left as a
described-but-not-yet-written recursive walker — implement it for real
(don't skip it) by copying `bind`'s dispatch structure but calling
`scope.sourceIndex(scope.resolve(colRef))` on every `ColumnRef` found and
comparing against `rightIndex`, rather than building the rewritten tree
`bind` builds. Write this out fully before considering the task done;
it's flagged here only because it reuses `bind`'s traversal shape rather
than introducing a new one, not because the logic itself is optional.

- [ ] **Step 5: Add `src/planner.cpp` to `meradb_core` in `cpp/CMakeLists.txt`.
  Also move `cloneExpr`'s real implementation (stubbed in Task 6) into
  this file (or a shared `ast_util.cpp`) now that both the parser and the
  planner need it — pick one location and have both include it.**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R planner`
Expected: all `[planner]` tests PASS. Also re-run the full suite to
confirm nothing in Tasks 1-11 regressed.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/planner.h cpp/src/planner.cpp cpp/tests/test_planner.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ planner module (scope, binding, access/join strategy)"
```

---

### Task 13: Evaluator module

**Files:**
- Create: `cpp/include/meradb/evaluator.h`
- Create: `cpp/src/evaluator.cpp`
- Test: `cpp/tests/test_evaluator.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::ast::*` (Task 5), `meradb::Value` (Task 3).
- Produces: `meradb::evaluate(const ast::Expr&, const
  std::unordered_map<std::string, Value>& row, const
  std::unordered_map<const ast::Expr*, Value>* subqueries = nullptr) ->
  Value`; `meradb::isTrue(const Value&) -> bool`; helper functions
  `columnRefs`, `findAggregates`, `findSubqueries`, `aggKey`,
  `exprLabel` (used by Task 14/17/18/19).

Three-valued logic: `KHALI` (a null `Value`) propagates through
arithmetic/comparisons; `AUR`/`YA` implement SQL's null-aware
short-circuit truth tables; `isTrue(v)` returns true only when `v` holds
a `bool` equal to `true` — used by WHERE/HAVING to decide keep-vs-drop
(an "unknown" row is dropped, matching Python's `value is True` check).

Dispatch is a sequence of `dynamic_cast` checks (mirroring the Python
evaluator's `isinstance` chain, not a virtual-visitor — kept as plain
`if`/`dynamic_cast` here since evaluator dispatch has no need for the
extensibility a visitor buys, and the Python source itself uses a linear
chain): `Literal` → `Subquery`/`InSubquery` (looked up by `Expr*` identity
in `subqueries`, matching Python's `id(expr)` lookup — the evaluator
itself never executes a subquery, the engine precomputes results in Task
18) → `Coalesce` (first non-null arg) → `CaseWhen` → `ColumnRef` (map
lookup, throws if missing) → `IsNull` → `UnaryOp` → `BinaryOp` (short-
circuit `AUR`/`YA`; else evaluate both sides, KHALI-propagate, dispatch
to comparison/`JAISA`/arithmetic) → `FuncCall` (looked up via `aggKey`
in the row map — group-by pre-computation stores aggregate results
there, see Task 14/18) → `Star` (throws — `*` isn't a value).

`JAISA` (LIKE): `%` → `.*`, `_` → `.`, case-insensitive, anchored
full-match, using `std::regex` (no LRU cache needed in Phase 1 — add one
later only if profiling shows repeated-pattern compilation is a hot
path).

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_evaluator.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/evaluator.h"
#include "meradb/parser.h"
#include "meradb/errors.h"

using namespace meradb;

namespace {
Value ev(const std::string& exprText, std::unordered_map<std::string, Value> row = {}) {
    auto expr = parseExpression(exprText);
    return evaluate(*expr, row);
}
}

TEST_CASE("evaluate computes arithmetic", "[evaluator]") {
    REQUIRE(std::get<int64_t>(ev("2 + 3 * 4").data) == 14);
}

TEST_CASE("evaluate resolves ColumnRef from the row map", "[evaluator]") {
    REQUIRE(std::get<int64_t>(ev("s.umar", {{"s.umar", Value(int64_t{20})}}).data) == 20);
}

TEST_CASE("evaluate propagates KHALI through arithmetic", "[evaluator]") {
    REQUIRE(ev("s.umar + 1", {{"s.umar", Value()}}).isNull());
}

TEST_CASE("evaluate implements three-valued AUR/YA", "[evaluator]") {
    // KHALI AUR JHOOTH -> JHOOTH (short-circuits to false regardless of the unknown side)
    auto v = ev("s.x AUR JHOOTH", {{"s.x", Value()}});
    REQUIRE(std::holds_alternative<bool>(v.data));
    REQUIRE(std::get<bool>(v.data) == false);
}

TEST_CASE("isTrue treats KHALI and JHOOTH both as not-true", "[evaluator]") {
    REQUIRE_FALSE(isTrue(Value()));
    REQUIRE_FALSE(isTrue(Value(false)));
    REQUIRE(isTrue(Value(true)));
}

TEST_CASE("evaluate handles IS NULL / IS NOT NULL", "[evaluator]") {
    REQUIRE(std::get<bool>(ev("s.x HAI KHALI", {{"s.x", Value()}}).data) == true);
    REQUIRE(std::get<bool>(ev("s.x HAI NAHI KHALI", {{"s.x", Value(int64_t{1})}}).data) == true);
}

TEST_CASE("evaluate matches JAISA (LIKE) patterns", "[evaluator]") {
    REQUIRE(std::get<bool>(ev("s.naam JAISA 'R%'", {{"s.naam", Value(std::string("Ravi"))}}).data));
    REQUIRE_FALSE(std::get<bool>(ev("s.naam JAISA 'X%'", {{"s.naam", Value(std::string("Ravi"))}}).data));
}

TEST_CASE("evaluate resolves PEHLA/COALESCE to first non-null argument", "[evaluator]") {
    REQUIRE(std::get<std::string>(ev("PEHLA(s.x, 'fallback')", {{"s.x", Value()}}).data) == "fallback");
}

TEST_CASE("evaluate resolves CASE WHEN branches in order", "[evaluator]") {
    auto v = ev("AGAR s.n < 18 TAB 'minor' WARNA 'adult' KHATAM", {{"s.n", Value(int64_t{10})}});
    REQUIRE(std::get<std::string>(v.data) == "minor");
}

TEST_CASE("evaluate throws when Star is evaluated as a value", "[evaluator]") {
    ast::Star star;
    REQUIRE_THROWS_AS(evaluate(star, {}), ExecutionError);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_evaluator.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/evaluator.h
#pragma once
#include "meradb/ast.h"
#include "meradb/datatypes.h"
#include <unordered_map>
#include <string>
#include <vector>

namespace meradb {

using Row = std::unordered_map<std::string, Value>;

Value evaluate(const ast::Expr& expr, const Row& row,
                const std::unordered_map<const ast::Expr*, Value>* subqueries = nullptr);
bool isTrue(const Value& v);

void columnRefs(const ast::Expr& expr, std::vector<const ast::ColumnRef*>& out, bool skipAggregates = false);
void findAggregates(const ast::Expr& expr, std::vector<const ast::FuncCall*>& out);
void findSubqueries(const ast::Expr& expr, std::vector<const ast::Expr*>& out);  // Subquery or InSubquery nodes
std::string aggKey(const ast::FuncCall& func);      // e.g. "GINO(*)" — used as the Row map key for a precomputed aggregate
std::string exprLabel(const ast::Expr& expr);        // human-readable header text, for result columns and EXPLAIN

}  // namespace meradb
```

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/evaluator.cpp
#include "meradb/evaluator.h"
#include "meradb/errors.h"
#include <cmath>
#include <regex>

namespace meradb {
using namespace ast;

namespace {
bool bothNumeric(const Value& a, const Value& b) {
    auto isNum = [](const Value& v) { return std::holds_alternative<int64_t>(v.data) || std::holds_alternative<double>(v.data); };
    return isNum(a) && isNum(b);
}
double asDouble(const Value& v) {
    return std::holds_alternative<int64_t>(v.data) ? static_cast<double>(std::get<int64_t>(v.data)) : std::get<double>(v.data);
}
Value numericBinOp(const std::string& op, const Value& a, const Value& b) {
    bool bothInt = std::holds_alternative<int64_t>(a.data) && std::holds_alternative<int64_t>(b.data);
    if (bothInt) {
        int64_t x = std::get<int64_t>(a.data), y = std::get<int64_t>(b.data);
        if (op == "+") return Value(x + y);
        if (op == "-") return Value(x - y);
        if (op == "*") return Value(x * y);
        if (op == "/") { if (y == 0) throw ExecutionError("0 se division nahi ho sakta"); return Value(x / y); }
        if (op == "%") { if (y == 0) throw ExecutionError("0 se modulo nahi ho sakta"); return Value(x % y); }
    }
    double x = asDouble(a), y = asDouble(b);
    if (op == "+") return Value(x + y);
    if (op == "-") return Value(x - y);
    if (op == "*") return Value(x * y);
    if (op == "/") { if (y == 0) throw ExecutionError("0 se division nahi ho sakta"); return Value(x / y); }
    if (op == "%") { if (y == 0) throw ExecutionError("0 se modulo nahi ho sakta"); return Value(std::fmod(x, y)); }
    throw ExecutionError("Anjaan operator: " + op);
}
int compareValues(const Value& a, const Value& b) {
    if (bothNumeric(a, b)) { double x = asDouble(a), y = asDouble(b); return x < y ? -1 : x > y ? 1 : 0; }
    if (std::holds_alternative<std::string>(a.data) && std::holds_alternative<std::string>(b.data)) {
        const auto& x = std::get<std::string>(a.data); const auto& y = std::get<std::string>(b.data);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (std::holds_alternative<Date>(a.data) && std::holds_alternative<Date>(b.data)) {
        int32_t x = std::get<Date>(a.data).toOrdinal(), y = std::get<Date>(b.data).toOrdinal();
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (std::holds_alternative<bool>(a.data) && std::holds_alternative<bool>(b.data)) {
        bool x = std::get<bool>(a.data), y = std::get<bool>(b.data);
        return x == y ? 0 : (!x && y ? -1 : 1);
    }
    throw ExecutionError("In values ko compare nahi kiya ja sakta");
}
std::regex likeToRegex(const std::string& pattern) {
    std::string out = "^";
    for (char c : pattern) {
        if (c == '%') out += ".*";
        else if (c == '_') out += ".";
        else if (std::strchr(".^$|()[]{}*+?\\", c)) { out += '\\'; out += c; }
        else out += c;
    }
    out += "$";
    return std::regex(out, std::regex::icase);
}
}  // namespace

bool isTrue(const Value& v) { return std::holds_alternative<bool>(v.data) && std::get<bool>(v.data); }

Value evaluate(const Expr& expr, const Row& row, const std::unordered_map<const Expr*, Value>* subqueries) {
    if (auto* lit = dynamic_cast<const Literal*>(&expr)) return lit->value;

    if (dynamic_cast<const Subquery*>(&expr) || dynamic_cast<const InSubquery*>(&expr)) {
        if (subqueries) { auto it = subqueries->find(&expr); if (it != subqueries->end()) return it->second; }
        throw ExecutionError("Subquery ka result precompute nahi hua (engine bug)");
    }
    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) {
        for (auto& a : co->args) { auto v = evaluate(*a, row, subqueries); if (!v.isNull()) return v; }
        return Value();
    }
    if (auto* cw = dynamic_cast<const CaseWhen*>(&expr)) {
        for (auto& [cond, result] : cw->branches)
            if (isTrue(evaluate(*cond, row, subqueries))) return evaluate(*result, row, subqueries);
        return cw->elseExpr ? evaluate(*cw->elseExpr, row, subqueries) : Value();
    }
    if (auto* ref = dynamic_cast<const ColumnRef*>(&expr)) {
        std::string key = (ref->table ? *ref->table + "." : "") + ref->name;
        auto it = row.find(key);
        if (it == row.end()) throw ExecutionError("'" + key + "' row mein nahi mila");
        return it->second;
    }
    if (auto* isn = dynamic_cast<const IsNull*>(&expr)) {
        bool null = evaluate(*isn->expr, row, subqueries).isNull();
        return Value(isn->negated ? !null : null);
    }
    if (auto* u = dynamic_cast<const UnaryOp*>(&expr)) {
        auto v = evaluate(*u->operand, row, subqueries);
        if (u->op == "NAHI") { if (v.isNull()) return Value(); return Value(!isTrue(v)); }
        if (v.isNull()) return Value();
        if (std::holds_alternative<int64_t>(v.data)) return Value(-std::get<int64_t>(v.data));
        return Value(-std::get<double>(v.data));
    }
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr)) {
        if (b->op == "AUR" || b->op == "YA") {
            auto l = evaluate(*b->left, row, subqueries);
            // three-valued short-circuit: AUR is false if either side is
            // false regardless of the other being unknown; YA is true if
            // either side is true regardless of the other being unknown.
            if (b->op == "AUR" && std::holds_alternative<bool>(l.data) && !std::get<bool>(l.data)) return Value(false);
            if (b->op == "YA" && std::holds_alternative<bool>(l.data) && std::get<bool>(l.data)) return Value(true);
            auto r = evaluate(*b->right, row, subqueries);
            if (l.isNull() || r.isNull()) return Value();
            bool lb = isTrue(l), rb = isTrue(r);
            return Value(b->op == "AUR" ? (lb && rb) : (lb || rb));
        }
        auto l = evaluate(*b->left, row, subqueries);
        auto r = evaluate(*b->right, row, subqueries);
        if (b->op == "JAISA") {
            if (l.isNull() || r.isNull()) return Value();
            return Value(std::regex_match(std::get<std::string>(l.data), likeToRegex(std::get<std::string>(r.data))));
        }
        if (l.isNull() || r.isNull()) return Value();
        if (b->op == "=") return Value(compareValues(l, r) == 0);
        if (b->op == "!=") return Value(compareValues(l, r) != 0);
        if (b->op == "<") return Value(compareValues(l, r) < 0);
        if (b->op == "<=") return Value(compareValues(l, r) <= 0);
        if (b->op == ">") return Value(compareValues(l, r) > 0);
        if (b->op == ">=") return Value(compareValues(l, r) >= 0);
        return numericBinOp(b->op, l, r);
    }
    if (auto* f = dynamic_cast<const FuncCall*>(&expr)) {
        auto it = row.find(aggKey(*f));
        if (it == row.end()) throw ExecutionError("Aggregate '" + f->name + "' precompute nahi hua (engine bug)");
        return it->second;
    }
    if (dynamic_cast<const Star*>(&expr)) throw ExecutionError("'*' ko value ki tarah evaluate nahi kiya ja sakta");
    throw ExecutionError("Anjaan expression node evaluate karte waqt");
}

std::string aggKey(const FuncCall& func) {
    return func.name + "(" + (dynamic_cast<const Star*>(func.arg.get()) ? "*" : exprLabel(*func.arg)) + ")";
}

std::string exprLabel(const Expr& expr) {
    if (auto* ref = dynamic_cast<const ColumnRef*>(&expr)) return (ref->table ? *ref->table + "." : "") + ref->name;
    if (dynamic_cast<const Star*>(&expr)) return "*";
    if (auto* lit = dynamic_cast<const Literal*>(&expr)) return formatValue(lit->value);
    if (auto* f = dynamic_cast<const FuncCall*>(&expr)) return aggKey(*f);
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr)) return exprLabel(*b->left) + " " + b->op + " " + exprLabel(*b->right);
    return "expr";
}

void columnRefs(const Expr& expr, std::vector<const ColumnRef*>& out, bool skipAggregates) {
    if (auto* ref = dynamic_cast<const ColumnRef*>(&expr)) { out.push_back(ref); return; }
    if (skipAggregates && dynamic_cast<const FuncCall*>(&expr)) return;
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr)) { columnRefs(*b->left, out, skipAggregates); columnRefs(*b->right, out, skipAggregates); return; }
    if (auto* u = dynamic_cast<const UnaryOp*>(&expr)) { columnRefs(*u->operand, out, skipAggregates); return; }
    if (auto* isn = dynamic_cast<const IsNull*>(&expr)) { columnRefs(*isn->expr, out, skipAggregates); return; }
    if (auto* f = dynamic_cast<const FuncCall*>(&expr)) { if (!skipAggregates) columnRefs(*f->arg, out, skipAggregates); return; }
    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) { for (auto& a : co->args) columnRefs(*a, out, skipAggregates); return; }
    if (auto* cw = dynamic_cast<const CaseWhen*>(&expr)) {
        for (auto& [c, r] : cw->branches) { columnRefs(*c, out, skipAggregates); columnRefs(*r, out, skipAggregates); }
        if (cw->elseExpr) columnRefs(*cw->elseExpr, out, skipAggregates);
    }
}

void findAggregates(const Expr& expr, std::vector<const FuncCall*>& out) {
    if (auto* f = dynamic_cast<const FuncCall*>(&expr)) { out.push_back(f); return; }
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr)) { findAggregates(*b->left, out); findAggregates(*b->right, out); return; }
    if (auto* u = dynamic_cast<const UnaryOp*>(&expr)) { findAggregates(*u->operand, out); return; }
    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) { for (auto& a : co->args) findAggregates(*a, out); return; }
    if (auto* cw = dynamic_cast<const CaseWhen*>(&expr)) {
        for (auto& [c, r] : cw->branches) { findAggregates(*c, out); findAggregates(*r, out); }
        if (cw->elseExpr) findAggregates(*cw->elseExpr, out);
    }
}

void findSubqueries(const Expr& expr, std::vector<const Expr*>& out) {
    if (dynamic_cast<const Subquery*>(&expr) || dynamic_cast<const InSubquery*>(&expr)) { out.push_back(&expr); return; }
    if (auto* b = dynamic_cast<const BinaryOp*>(&expr)) { findSubqueries(*b->left, out); findSubqueries(*b->right, out); return; }
    if (auto* u = dynamic_cast<const UnaryOp*>(&expr)) { findSubqueries(*u->operand, out); return; }
    if (auto* isn = dynamic_cast<const IsNull*>(&expr)) { findSubqueries(*isn->expr, out); return; }
    if (auto* co = dynamic_cast<const Coalesce*>(&expr)) { for (auto& a : co->args) findSubqueries(*a, out); return; }
    if (auto* cw = dynamic_cast<const CaseWhen*>(&expr)) {
        for (auto& [c, r] : cw->branches) { findSubqueries(*c, out); findSubqueries(*r, out); }
        if (cw->elseExpr) findSubqueries(*cw->elseExpr, out);
    }
}

}  // namespace meradb
```

- [ ] **Step 5: Add `src/evaluator.cpp` to `meradb_core` in `cpp/CMakeLists.txt`**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R evaluator`
Expected: all `[evaluator]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/evaluator.h cpp/src/evaluator.cpp cpp/tests/test_evaluator.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ evaluator module (three-valued logic, LIKE, CASE/COALESCE)"
```

---

### Task 14: Aggregates module

**Files:**
- Create: `cpp/include/meradb/aggregates.h`
- Create: `cpp/src/aggregates.cpp`
- Test: `cpp/tests/test_aggregates.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::ast::FuncCall` (Task 5), `meradb::evaluate` (Task
  13), `meradb::Row` (Task 13), `meradb::ExecutionError` (Task 2).
- Produces: `meradb::canonicalName(const ast::FuncCall&) -> std::string`
  (throws on unknown/misused name), `meradb::computeAggregate(const
  ast::FuncCall&, const std::vector<Row>& groupRows) -> Value`.
  Group-by/HAVING partitioning itself (`_group` in Python) is implemented
  in Task 18 (Engine — SELECT), not here, matching the Python split
  between `aggregates.py` (per-group math) and `engine.py` (partitioning).

Aliases: `GINO`/`COUNT`, `KUL`/`SUM`, `AUSAT`/`AVG`, `NYUNTAM`/`MIN`,
`ADHIKTAM`/`MAX`, each canonicalizing to the Hinglish name. `GINO(*)`
counts rows including NULL columns; `GINO(x)` counts non-KHALI values of
`x`. `KUL`/`AUSAT`/`NYUNTAM`/`ADHIKTAM` ignore KHALI values in their
input and return KHALI if every value in the group was KHALI (`GINO`
returns `0` in that case instead). `KUL`/`AUSAT` reject non-numeric
(bool/string/date) arguments.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_aggregates.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/aggregates.h"
#include "meradb/errors.h"

using namespace meradb;
using namespace meradb::ast;

namespace {
FuncCall makeCall(const std::string& name, std::unique_ptr<Expr> arg) {
    FuncCall f; f.name = name; f.arg = std::move(arg); return f;
}
}

TEST_CASE("canonicalName resolves every alias", "[aggregates]") {
    REQUIRE(canonicalName(makeCall("COUNT", std::make_unique<Star>())) == "GINO");
    REQUIRE(canonicalName(makeCall("SUM", std::make_unique<ColumnRef>("cgpa"))) == "KUL");
    REQUIRE(canonicalName(makeCall("AVG", std::make_unique<ColumnRef>("cgpa"))) == "AUSAT");
    REQUIRE(canonicalName(makeCall("MIN", std::make_unique<ColumnRef>("cgpa"))) == "NYUNTAM");
    REQUIRE(canonicalName(makeCall("MAX", std::make_unique<ColumnRef>("cgpa"))) == "ADHIKTAM");
}

TEST_CASE("canonicalName throws for unknown function name", "[aggregates]") {
    REQUIRE_THROWS_AS(canonicalName(makeCall("NONSENSE", std::make_unique<Star>())), ExecutionError);
}

TEST_CASE("GINO(*) counts all rows including NULL columns", "[aggregates]") {
    auto call = makeCall("GINO", std::make_unique<Star>());
    std::vector<Row> rows = {{{"s.x", Value()}}, {{"s.x", Value(int64_t{1})}}};
    REQUIRE(std::get<int64_t>(computeAggregate(call, rows).data) == 2);
}

TEST_CASE("GINO(x) counts only non-KHALI values", "[aggregates]") {
    auto call = makeCall("GINO", std::make_unique<ColumnRef>("x", "s"));
    std::vector<Row> rows = {{{"s.x", Value()}}, {{"s.x", Value(int64_t{1})}}};
    REQUIRE(std::get<int64_t>(computeAggregate(call, rows).data) == 1);
}

TEST_CASE("KUL/AUSAT ignore KHALI and return KHALI for an all-KHALI group", "[aggregates]") {
    auto sum = makeCall("KUL", std::make_unique<ColumnRef>("x", "s"));
    std::vector<Row> allNull = {{{"s.x", Value()}}};
    REQUIRE(computeAggregate(sum, allNull).isNull());

    std::vector<Row> mixed = {{{"s.x", Value(int64_t{2})}}, {{"s.x", Value()}}, {{"s.x", Value(int64_t{3})}}};
    REQUIRE(std::get<int64_t>(computeAggregate(sum, mixed).data) == 5);
}

TEST_CASE("NYUNTAM/ADHIKTAM compute min/max ignoring KHALI", "[aggregates]") {
    auto minCall = makeCall("NYUNTAM", std::make_unique<ColumnRef>("x", "s"));
    std::vector<Row> rows = {{{"s.x", Value(int64_t{5})}}, {{"s.x", Value()}}, {{"s.x", Value(int64_t{2})}}};
    REQUIRE(std::get<int64_t>(computeAggregate(minCall, rows).data) == 2);
}

TEST_CASE("KUL rejects a non-numeric argument", "[aggregates]") {
    auto sum = makeCall("KUL", std::make_unique<ColumnRef>("x", "s"));
    std::vector<Row> rows = {{{"s.x", Value(std::string("nope"))}}};
    REQUIRE_THROWS_AS(computeAggregate(sum, rows), ExecutionError);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_aggregates.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/aggregates.h
#pragma once
#include "meradb/ast.h"
#include "meradb/evaluator.h"
#include <string>
#include <vector>

namespace meradb {

std::string canonicalName(const ast::FuncCall& func);
Value computeAggregate(const ast::FuncCall& func, const std::vector<Row>& groupRows);

}  // namespace meradb
```

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/aggregates.cpp
#include "meradb/aggregates.h"
#include "meradb/errors.h"
#include <unordered_map>

namespace meradb {
using namespace ast;

std::string canonicalName(const FuncCall& func) {
    static const std::unordered_map<std::string, std::string> aliases = {
        {"GINO", "GINO"}, {"COUNT", "GINO"},
        {"KUL", "KUL"}, {"SUM", "KUL"},
        {"AUSAT", "AUSAT"}, {"AVG", "AUSAT"},
        {"NYUNTAM", "NYUNTAM"}, {"MIN", "NYUNTAM"},
        {"ADHIKTAM", "ADHIKTAM"}, {"MAX", "ADHIKTAM"},
    };
    auto it = aliases.find(func.name);
    if (it == aliases.end()) throw ExecutionError("Anjaan function: " + func.name);
    if (it->second != "GINO" && dynamic_cast<const Star*>(func.arg.get()))
        throw ExecutionError(it->second + "(*) allowed nahi hai — sirf GINO(*)/COUNT(*)");
    return it->second;
}

Value computeAggregate(const FuncCall& func, const std::vector<Row>& groupRows) {
    std::string name = canonicalName(func);
    bool isStar = dynamic_cast<const Star*>(func.arg.get()) != nullptr;

    if (name == "GINO") {
        if (isStar) return Value(static_cast<int64_t>(groupRows.size()));
        int64_t count = 0;
        for (auto& row : groupRows) if (!evaluate(*func.arg, row).isNull()) ++count;
        return Value(count);
    }

    std::vector<Value> values;
    for (auto& row : groupRows) {
        auto v = evaluate(*func.arg, row);
        if (v.isNull()) continue;
        if ((name == "KUL" || name == "AUSAT") &&
            !std::holds_alternative<int64_t>(v.data) && !std::holds_alternative<double>(v.data))
            throw ExecutionError(name + " sirf ANK/DASHAMLAV columns par chalta hai");
        values.push_back(v);
    }
    if (values.empty()) return Value();

    auto asDouble = [](const Value& v) {
        return std::holds_alternative<int64_t>(v.data) ? static_cast<double>(std::get<int64_t>(v.data)) : std::get<double>(v.data);
    };
    bool allInt = true;
    for (auto& v : values) if (!std::holds_alternative<int64_t>(v.data)) allInt = false;

    if (name == "KUL") {
        if (allInt) { int64_t s = 0; for (auto& v : values) s += std::get<int64_t>(v.data); return Value(s); }
        double s = 0; for (auto& v : values) s += asDouble(v); return Value(s);
    }
    if (name == "AUSAT") {
        double s = 0; for (auto& v : values) s += asDouble(v);
        return Value(s / static_cast<double>(values.size()));
    }
    if (name == "NYUNTAM" || name == "ADHIKTAM") {
        Value best = values[0];
        for (auto& v : values) {
            double a = asDouble(v), b = asDouble(best);
            if ((name == "NYUNTAM" && a < b) || (name == "ADHIKTAM" && a > b)) best = v;
        }
        return best;
    }
    throw ExecutionError("Anjaan aggregate: " + name);
}

}  // namespace meradb
```

- [ ] **Step 5: Add `src/aggregates.cpp` to `meradb_core` in `cpp/CMakeLists.txt`**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R aggregates`
Expected: all `[aggregates]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/aggregates.h cpp/src/aggregates.cpp cpp/tests/test_aggregates.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ aggregates module"
```

---

### Task 15: Engine — Instance (shared state, transaction snapshots, crash recovery)

**Files:**
- Create: `cpp/include/meradb/engine.h`
- Create: `cpp/src/engine.cpp`
- Test: `cpp/tests/test_instance.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::Catalog` (Task 9), `meradb::StorageError` (Task 2).
- Produces: `meradb::Instance` class — `dbDir(name) -> std::string`,
  `databases() -> std::vector<std::string>`, `catalog(db) -> Catalog&`
  (creates+caches on first access), `forget(db)`, `takeSnapshot(db)`,
  `discardSnapshot(db)`, `restoreSnapshot(db)`, plus a `std::recursive_
  mutex lock` member and a `recover() -> std::vector<std::string>` run
  from the constructor. This is the shared, per-data-directory state that
  `Engine` (Tasks 16-19) wraps per session — in Phase 1 (no networking,
  no concurrent sessions) there is exactly one `Instance` per embedded
  `Engine`, but the class is written the same regardless since Phase 2's
  server shares one `Instance` across many `Engine`s.

Directory layout, matching `storage.py`/`engine.py`'s `Instance`:
`data/<db>/catalog.json`, `data/<db>/<table>.tbl`, and
`data/.wapas/<db>` holding an in-flight or crash-recovered transaction
snapshot (a full directory copy of `data/<db>` taken at `BEGIN`).

Transaction snapshot protocol (recheck against `engine.py`'s `Instance`
methods directly for the exact rename sequence before finalizing — this
is the one part of Phase 1 where a subtly wrong ordering could silently
corrupt data on a crash, so precision matters more than speed here):
- `takeSnapshot(db)` (BEGIN): recursively copy `data/<db>` to
  `data/.wapas/<db>.tmp`, then atomically rename to `data/.wapas/<db>`.
- `discardSnapshot(db)` (COMMIT): atomically rename `data/.wapas/<db>` to
  `data/.wapas/<db>.done`, then delete it.
- `restoreSnapshot(db)` (ROLLBACK, or startup crash recovery): delete
  `data/<db>`, then atomically rename `data/.wapas/<db>` back to
  `data/<db>`.
- `recover()` (constructor): for every entry under `data/.wapas/`, a
  `.tmp`/`.done` leftover means an incomplete BEGIN or an already-
  committed transaction — just delete it; a plain-named snapshot means a
  transaction never reached COMMIT — call `restoreSnapshot` on it. Return
  the list of database names that were rolled back, so the CLI (Task 20)
  can log it.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_instance.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include <filesystem>
#include <fstream>

using namespace meradb;
namespace fs = std::filesystem;

namespace {
std::string tempDataDir() {
    auto dir = fs::temp_directory_path() / ("meradb_instance_test_" + std::to_string(rand()));
    fs::create_directories(dir);
    return dir.string();
}
}

TEST_CASE("Instance creates a database directory on first catalog() access", "[instance]") {
    auto dir = tempDataDir();
    Instance inst(dir);
    inst.catalog("main");
    REQUIRE(fs::exists(fs::path(dir) / "main"));
    fs::remove_all(dir);
}

TEST_CASE("Instance snapshot/restore round-trips a database directory", "[instance]") {
    auto dir = tempDataDir();
    Instance inst(dir);
    inst.catalog("main");  // creates data/main/
    std::ofstream(fs::path(dir) / "main" / "marker.txt") << "before";
    inst.takeSnapshot("main");
    std::ofstream(fs::path(dir) / "main" / "marker.txt") << "after-corrupted";
    inst.restoreSnapshot("main");
    std::ifstream in(fs::path(dir) / "main" / "marker.txt");
    std::string content; in >> content;
    REQUIRE(content == "before");
    fs::remove_all(dir);
}

TEST_CASE("Instance discardSnapshot leaves the live database untouched", "[instance]") {
    auto dir = tempDataDir();
    Instance inst(dir);
    inst.catalog("main");
    inst.takeSnapshot("main");
    std::ofstream(fs::path(dir) / "main" / "marker.txt") << "committed-value";
    inst.discardSnapshot("main");
    REQUIRE_FALSE(fs::exists(fs::path(dir) / ".wapas" / "main"));
    std::ifstream in(fs::path(dir) / "main" / "marker.txt");
    std::string content; in >> content;
    REQUIRE(content == "committed-value");
    fs::remove_all(dir);
}

TEST_CASE("Instance recovers an incomplete transaction on startup", "[instance]") {
    auto dir = tempDataDir();
    {
        Instance inst(dir);
        inst.catalog("main");
        std::ofstream(fs::path(dir) / "main" / "marker.txt") << "original";
        inst.takeSnapshot("main");
        std::ofstream(fs::path(dir) / "main" / "marker.txt") << "mid-transaction";
        // simulate a crash: never call discardSnapshot or restoreSnapshot
    }
    Instance inst2(dir);  // constructor runs recover()
    std::ifstream in(fs::path(dir) / "main" / "marker.txt");
    std::string content; in >> content;
    REQUIRE(content == "original");  // rolled back automatically
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_instance.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure.

- [ ] **Step 3: Write the header** (this header also declares `Engine`,
  filled in across Tasks 16-19 — declare the full class shape now so
  later tasks only add method bodies, not new declarations)

```cpp
// cpp/include/meradb/engine.h
#pragma once
#include "meradb/ast.h"
#include "meradb/catalog.h"
#include "meradb/table.h"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace meradb {

class Instance {
public:
    explicit Instance(std::string dataDir);
    std::recursive_mutex lock;

    std::string dbDir(const std::string& name) const;
    std::vector<std::string> databases() const;
    Catalog& catalog(const std::string& db);
    void forget(const std::string& db);
    void takeSnapshot(const std::string& db);
    void discardSnapshot(const std::string& db);
    void restoreSnapshot(const std::string& db);

    // shared hash-index cache, keyed by (database, table); Table objects
    // constructed by Engine (Task 16+) should look here before building
    // their own, so index-building work isn't repeated per statement.
    std::unordered_map<std::pair<std::string, std::string>, Table::IndexMap,
        struct PairHash>* /* placeholder alias */ indexes = nullptr;

private:
    std::string dataDir_;
    std::unordered_map<std::string, std::unique_ptr<Catalog>> catalogs_;
    std::vector<std::string> recover();
};

struct Result {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    std::string message;
    std::string error;
};

class Engine {
public:
    explicit Engine(std::shared_ptr<Instance> instance);
    explicit Engine(const std::string& dataDir);  // convenience: owns its own private Instance

    std::string currentDb = "main";
    std::optional<std::string> txnDb;
    bool inTransaction() const { return txnDb.has_value(); }
    Catalog& catalog();

    std::vector<Result> execute(const std::string& text);       // raises on first error
    std::vector<Result> runScript(const std::string& text);      // never raises; failing stmt -> Result.error, continues
    Result executeStatement(const ast::Statement& stmt);
    void close();  // rolls back an unfinished transaction

private:
    std::shared_ptr<Instance> instance_;

    // dispatch, implemented across Tasks 16-19
    Result execCreateDatabase(const ast::CreateDatabase&);
    Result execDropDatabase(const ast::DropDatabase&);
    Result execUseDatabase(const ast::UseDatabase&);
    Result execShowTables(const ast::ShowTables&);
    Result execDescribe(const ast::Describe&);
    Result execCreateTable(const ast::CreateTable&);
    Result execDropTable(const ast::DropTable&);
    Result execTruncateTable(const ast::TruncateTable&);
    Result execCompactTable(const ast::CompactTable&);
    Result execAlterAddColumn(const ast::AlterAddColumn&);
    Result execAlterAddComposite(const ast::AlterAddComposite&);
    Result execAlterDropColumn(const ast::AlterDropColumn&);
    Result execRenameTable(const ast::RenameTable&);
    Result execRenameColumn(const ast::RenameColumn&);
    Result execBegin(const ast::Begin&);
    Result execCommit(const ast::Commit&);
    Result execRollback(const ast::Rollback&);
    Result execInsert(const ast::Insert&);
    Result execUpdate(const ast::Update&);
    Result execDelete(const ast::Delete&);
    Result execSelect(const ast::Select&);
    Result execSetOp(const ast::SetOp&);
    Result execCreateView(const ast::CreateView&);
    Result execDropView(const ast::DropView&);
    Result execShowViews(const ast::ShowViews&);
    Result execExplain(const ast::Explain&);
};

}  // namespace meradb
```

Fix needed before this compiles: the `indexes` member's type is written
as a placeholder above (`struct PairHash` referenced but never defined,
and the type itself is convoluted). Replace it with a clean, real
declaration when implementing:
```cpp
struct StringPairHash {
    size_t operator()(const std::pair<std::string, std::string>& p) const {
        return std::hash<std::string>{}(p.first) ^ (std::hash<std::string>{}(p.second) << 1);
    }
};
// member of Instance:
std::unordered_map<std::pair<std::string, std::string>, /* per-table index cache, same
    shape as Table::IndexMap */ std::shared_ptr<void>, StringPairHash> indexCache;
```
Whether this shared cache is actually wired into `Table` construction in
Phase 1 is a judgment call for whoever implements Task 16 — Phase 1 has
no concurrent sessions, so each `Engine::catalog()`/table-open call can
just build its own `Table` with a private index cache and it will still
be correct (only Phase 2's multi-session server needs the cache to be
*shared*, per the spec). Simplify by dropping the shared cache entirely
in Phase 1 and revisit in Phase 2 if this comment is still here.

- [ ] **Step 4: Write the `Instance` implementation** (leave every
  `Engine::exec*` method as a one-line `throw ExecutionError("not
  implemented yet — Task N")` stub for now, so the file compiles and
  links; Tasks 16-19 fill these in one by one)

```cpp
// cpp/src/engine.cpp
#include "meradb/engine.h"
#include "meradb/errors.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace meradb {

Instance::Instance(std::string dataDir) : dataDir_(std::move(dataDir)) {
    fs::create_directories(dataDir_);
    recover();
}

std::string Instance::dbDir(const std::string& name) const { return (fs::path(dataDir_) / name).string(); }

std::vector<std::string> Instance::databases() const {
    std::vector<std::string> names;
    if (!fs::exists(dataDir_)) return names;
    for (auto& entry : fs::directory_iterator(dataDir_))
        if (entry.is_directory() && entry.path().filename() != ".wapas") names.push_back(entry.path().filename().string());
    return names;
}

Catalog& Instance::catalog(const std::string& db) {
    auto it = catalogs_.find(db);
    if (it != catalogs_.end()) return *it->second;
    fs::create_directories(dbDir(db));
    auto cat = std::make_unique<Catalog>(dbDir(db));
    auto& ref = *cat;
    catalogs_[db] = std::move(cat);
    return ref;
}

void Instance::forget(const std::string& db) { catalogs_.erase(db); }

void Instance::takeSnapshot(const std::string& db) {
    fs::path wapas = fs::path(dataDir_) / ".wapas";
    fs::create_directories(wapas);
    fs::path tmp = wapas / (db + ".tmp");
    fs::remove_all(tmp);
    fs::copy(dbDir(db), tmp, fs::copy_options::recursive);
    fs::path final = wapas / db;
    fs::remove_all(final);
    fs::rename(tmp, final);
}
void Instance::discardSnapshot(const std::string& db) {
    fs::path wapas = fs::path(dataDir_) / ".wapas";
    fs::path snap = wapas / db;
    if (!fs::exists(snap)) return;
    fs::path done = wapas / (db + ".done");
    fs::rename(snap, done);
    fs::remove_all(done);
}
void Instance::restoreSnapshot(const std::string& db) {
    fs::path wapas = fs::path(dataDir_) / ".wapas";
    fs::path snap = wapas / db;
    forget(db);
    fs::remove_all(dbDir(db));
    fs::rename(snap, dbDir(db));
}

std::vector<std::string> Instance::recover() {
    std::vector<std::string> recovered;
    fs::path wapas = fs::path(dataDir_) / ".wapas";
    if (!fs::exists(wapas)) return recovered;
    for (auto& entry : fs::directory_iterator(wapas)) {
        std::string name = entry.path().filename().string();
        if (name.size() > 4 && (name.substr(name.size() - 4) == ".tmp" || name.substr(name.size() - 5) == ".done")) {
            fs::remove_all(entry.path());
            continue;
        }
        restoreSnapshot(name);
        recovered.push_back(name);
    }
    return recovered;
}

Engine::Engine(std::shared_ptr<Instance> instance) : instance_(std::move(instance)) {}
Engine::Engine(const std::string& dataDir) : instance_(std::make_shared<Instance>(dataDir)) {}

Catalog& Engine::catalog() { return instance_->catalog(txnDb.value_or(currentDb)); }

void Engine::close() { if (inTransaction()) instance_->restoreSnapshot(*txnDb); }

}  // namespace meradb
```

- [ ] **Step 5: Add `src/engine.cpp` to `meradb_core` in `cpp/CMakeLists.txt`**

- [ ] **Step 6: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R instance`
Expected: all `[instance]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/engine.h cpp/src/engine.cpp cpp/tests/test_instance.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ Instance (shared state, transaction snapshots, crash recovery)"
```

---

### Task 16: Engine — DDL execution and transactions

**Files:**
- Modify: `cpp/src/engine.cpp` (fill in the DDL/transaction `exec*`
  methods stubbed in Task 15)
- Test: `cpp/tests/test_engine_ddl.cpp`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 9-15.
- Produces: working `execCreateDatabase`, `execDropDatabase`,
  `execUseDatabase`, `execShowTables`, `execDescribe`, `execCreateTable`,
  `execDropTable`, `execTruncateTable`, `execCompactTable`,
  `execAlterAddColumn`, `execAlterAddComposite`, `execAlterDropColumn`,
  `execRenameTable`, `execRenameColumn`, `execBegin`, `execCommit`,
  `execRollback`, and `executeStatement`'s dispatch (a chain of
  `dynamic_cast` checks against `ast::Statement`, calling the matching
  `exec*` — this replaces Python's reflective `getattr(self,
  f"_exec_{type(stmt).__name__}")`, which C++ has no equivalent for
  without RTTI type-name string-matching; an explicit `dynamic_cast`
  chain is the direct, idiomatic replacement).

Key behaviors to replicate exactly (re-check `engine.py`'s corresponding
`_exec_*` methods directly for any case not spelled out below):
- `CreateTable`: validate at most one primary key (single-column
  `primaryKey` OR `compositePk`, never both); no duplicate column names;
  every `SANDARBH`/FK target column must exist, be `isUnique()`, and have
  the same `typeName` as the referencing column; every `SHART`/CHECK
  expression (parsed via `parseExpression` on its stored source text)
  may reference only this table's own unqualified columns and no
  aggregates. Only after all validation passes: create the `.tbl` heap
  file (`HeapFile(catalog().tablePath(name)).create()`) and
  `catalog().add(schema)`.
- `AlterAddColumn`: rewrite the whole table file, appending the new
  column's default (or `KHALI`) to every existing row; validate FK/CHECK
  against existing data before committing the rewrite.
- `DropTable`/`AlterDropColumn`: refuse (throw `ExecutionError`) if any
  other table's FK still references this table (or, for a dropped
  column, if any CHECK still mentions it).
- `RenameTable`/`RenameColumn`: rename the `.tbl` file via
  `std::filesystem::rename` (atomic); cascade the rename into every
  other table's stored FK metadata (`refTable`/`refColumn`) and re-save
  each affected catalog entry.
- `Begin`/`Commit`/`Rollback`: `Begin` calls `instance_->takeSnapshot`
  and sets `txnDb = currentDb`; a nested `Begin` while already in a
  transaction is an error. `Commit`/`Rollback` require `inTransaction()`
  to be true, call `discardSnapshot`/`restoreSnapshot`, and clear
  `txnDb`.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_engine_ddl.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/parser.h"
#include "meradb/errors.h"
#include <filesystem>

using namespace meradb;

namespace {
std::string tempDataDir() {
    auto dir = std::filesystem::temp_directory_path() / ("meradb_ddl_test_" + std::to_string(rand()));
    std::filesystem::create_directories(dir);
    return dir.string();
}
Result runOne(Engine& e, const std::string& sql) {
    auto stmts = parseScript(sql);
    return e.executeStatement(*stmts[0]);
}
}

TEST_CASE("CREATE TABLE validates single primary key and creates the heap file", "[engine][ddl]") {
    auto dir = tempDataDir();
    Engine e(dir);
    runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT ZAROORI);");
    REQUIRE(e.catalog().find("students") != nullptr);
    REQUIRE(std::filesystem::exists(e.catalog().tablePath("students")));
    std::filesystem::remove_all(dir);
}

TEST_CASE("CREATE TABLE rejects both single and composite primary key", "[engine][ddl]") {
    auto dir = tempDataDir();
    Engine e(dir);
    REQUIRE_THROWS_AS(runOne(e,
        "BANAO TABLE t (id INT MUKHYA KUNJI, b INT, MUKHYA KUNJI (id, b));"), ExecutionError);
    std::filesystem::remove_all(dir);
}

TEST_CASE("CREATE TABLE validates FK target is unique and same type", "[engine][ddl]") {
    auto dir = tempDataDir();
    Engine e(dir);
    runOne(e, "BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT);");
    REQUIRE_NOTHROW(runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, cid INT SANDARBH courses(id));"));
    REQUIRE_THROWS_AS(runOne(e,
        "BANAO TABLE bad (id INT MUKHYA KUNJI, cid INT SANDARBH courses(title));"), ExecutionError);
    std::filesystem::remove_all(dir);
}

TEST_CASE("DROP TABLE refuses when another table's FK references it", "[engine][ddl]") {
    auto dir = tempDataDir();
    Engine e(dir);
    runOne(e, "BANAO TABLE courses (id INT MUKHYA KUNJI);");
    runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, cid INT SANDARBH courses(id));");
    REQUIRE_THROWS_AS(runOne(e, "HATAO TABLE courses;"), ExecutionError);
    std::filesystem::remove_all(dir);
}

TEST_CASE("ALTER TABLE ADD COLUMN backfills existing rows with the default", "[engine][ddl]") {
    auto dir = tempDataDir();
    Engine e(dir);
    runOne(e, "BANAO TABLE t (id INT MUKHYA KUNJI);");
    runOne(e, "DAALO MEIN t (id) MAAN (1);");
    runOne(e, "SUDHARO TABLE t JODO COLUMN active BOOL WARNA SACH;");
    auto result = runOne(e, "DIKHAO * SE t;");
    REQUIRE(std::get<bool>(result.rows[0][1].data) == true);
    std::filesystem::remove_all(dir);
}

TEST_CASE("RENAME TABLE cascades into other tables' FK metadata", "[engine][ddl]") {
    auto dir = tempDataDir();
    Engine e(dir);
    runOne(e, "BANAO TABLE courses (id INT MUKHYA KUNJI);");
    runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, cid INT SANDARBH courses(id));");
    runOne(e, "SUDHARO TABLE courses NAYA_NAAM classes;");
    REQUIRE(e.catalog().get("students").columns[1].refTable.value() == "classes");
    std::filesystem::remove_all(dir);
}

TEST_CASE("BEGIN/COMMIT/ROLLBACK toggle inTransaction and persist/discard changes", "[engine][ddl]") {
    auto dir = tempDataDir();
    Engine e(dir);
    runOne(e, "BANAO TABLE t (id INT MUKHYA KUNJI);");
    REQUIRE_FALSE(e.inTransaction());
    runOne(e, "SHURU;");
    REQUIRE(e.inTransaction());
    runOne(e, "DAALO MEIN t (id) MAAN (1);");
    runOne(e, "WAPAS;");
    REQUIRE_FALSE(e.inTransaction());
    auto result = runOne(e, "DIKHAO * SE t;");
    REQUIRE(result.rows.empty());  // insert was rolled back
    std::filesystem::remove_all(dir);
}

TEST_CASE("A nested BEGIN while already in a transaction throws", "[engine][ddl]") {
    auto dir = tempDataDir();
    Engine e(dir);
    runOne(e, "SHURU;");
    REQUIRE_THROWS_AS(runOne(e, "SHURU;"), ExecutionError);
    std::filesystem::remove_all(dir);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_engine_ddl.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure
(stubs throw "not implemented yet").

- [ ] **Step 3: Implement the DDL/transaction methods in `cpp/src/engine.cpp`**,
  following the behaviors described above. Implement `executeStatement`'s
  dispatch as:

```cpp
Result Engine::executeStatement(const ast::Statement& stmt) {
    using namespace ast;
    if (auto* s = dynamic_cast<const CreateDatabase*>(&stmt)) return execCreateDatabase(*s);
    if (auto* s = dynamic_cast<const DropDatabase*>(&stmt)) return execDropDatabase(*s);
    if (auto* s = dynamic_cast<const UseDatabase*>(&stmt)) return execUseDatabase(*s);
    if (auto* s = dynamic_cast<const ShowTables*>(&stmt)) return execShowTables(*s);
    if (auto* s = dynamic_cast<const Describe*>(&stmt)) return execDescribe(*s);
    if (auto* s = dynamic_cast<const CreateTable*>(&stmt)) return execCreateTable(*s);
    if (auto* s = dynamic_cast<const DropTable*>(&stmt)) return execDropTable(*s);
    if (auto* s = dynamic_cast<const TruncateTable*>(&stmt)) return execTruncateTable(*s);
    if (auto* s = dynamic_cast<const CompactTable*>(&stmt)) return execCompactTable(*s);
    if (auto* s = dynamic_cast<const AlterAddColumn*>(&stmt)) return execAlterAddColumn(*s);
    if (auto* s = dynamic_cast<const AlterAddComposite*>(&stmt)) return execAlterAddComposite(*s);
    if (auto* s = dynamic_cast<const AlterDropColumn*>(&stmt)) return execAlterDropColumn(*s);
    if (auto* s = dynamic_cast<const RenameTable*>(&stmt)) return execRenameTable(*s);
    if (auto* s = dynamic_cast<const RenameColumn*>(&stmt)) return execRenameColumn(*s);
    if (auto* s = dynamic_cast<const Begin*>(&stmt)) return execBegin(*s);
    if (auto* s = dynamic_cast<const Commit*>(&stmt)) return execCommit(*s);
    if (auto* s = dynamic_cast<const Rollback*>(&stmt)) return execRollback(*s);
    if (auto* s = dynamic_cast<const Insert*>(&stmt)) return execInsert(*s);          // Task 17
    if (auto* s = dynamic_cast<const Update*>(&stmt)) return execUpdate(*s);          // Task 17
    if (auto* s = dynamic_cast<const Delete*>(&stmt)) return execDelete(*s);          // Task 17
    if (auto* s = dynamic_cast<const Select*>(&stmt)) return execSelect(*s);          // Task 18
    if (auto* s = dynamic_cast<const SetOp*>(&stmt)) return execSetOp(*s);            // Task 18
    if (auto* s = dynamic_cast<const CreateView*>(&stmt)) return execCreateView(*s);  // Task 19
    if (auto* s = dynamic_cast<const DropView*>(&stmt)) return execDropView(*s);      // Task 19
    if (auto* s = dynamic_cast<const ShowViews*>(&stmt)) return execShowViews(*s);    // Task 19
    if (auto* s = dynamic_cast<const Explain*>(&stmt)) return execExplain(*s);        // Task 19
    throw ExecutionError("Anjaan statement type executeStatement mein");
}

Result Engine::execBegin(const ast::Begin&) {
    if (inTransaction()) throw ExecutionError("Transaction pehle se chal raha hai");
    instance_->takeSnapshot(currentDb);
    txnDb = currentDb;
    return {{}, {}, "Transaction shuru", ""};
}
Result Engine::execCommit(const ast::Commit&) {
    if (!inTransaction()) throw ExecutionError("Koi transaction chal nahi raha");
    instance_->discardSnapshot(*txnDb);
    txnDb.reset();
    return {{}, {}, "Transaction pakka ho gaya", ""};
}
Result Engine::execRollback(const ast::Rollback&) {
    if (!inTransaction()) throw ExecutionError("Koi transaction chal nahi raha");
    instance_->restoreSnapshot(*txnDb);
    txnDb.reset();
    return {{}, {}, "Transaction wapas ho gaya", ""};
}
```

Implement the remaining `exec*` DDL methods by directly transcribing
their `engine.py` counterparts' logic (`_check_fk_target`,
`_check_shart_expr`, `_check_no_children`, etc. — re-derive these helper
checks in C++ as private `Engine` methods with the same names/shapes
described in the Explore-agent research above). Use `Catalog::save()`
(already atomic) after every metadata change, and `HeapFile`/`Table`
(Tasks 10-11) for every file-level change.

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "engine.*ddl"`
Expected: all `[engine][ddl]` tests PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/src/engine.cpp cpp/tests/test_engine_ddl.cpp cpp/tests/CMakeLists.txt
git commit -m "Add C++ engine DDL execution and transactions"
```

---

### Task 17: Engine — DML execution (INSERT/UPDATE/DELETE)

**Files:**
- Modify: `cpp/src/engine.cpp`
- Test: `cpp/tests/test_engine_dml.cpp`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 9-16, plus `meradb::coerce` (Task 3), `meradb::planner::
  chooseAccess` (Task 12).
- Produces: working `execInsert`, `execUpdate`, `execDelete`, plus shared
  private validation helpers `validateRow`, `checkLength`, `checkShart`,
  `checkUnique`, `checkFk`, `checkNoChildren`, `findConflict` (used by
  upsert).

Key behaviors to replicate exactly:
- `Insert`: validate **every** row (`coerce` each value against its
  column's `typeName`, enforce `maxLength` for TEXT, evaluate every
  `SHART`/CHECK expression against the fully-assembled row) before
  writing **any** row — a bad 3rd row must leave the first two
  un-inserted too. Supports plain `MAAN` tuples (evaluate each `Expr` via
  `evaluate()` with an empty row context, since INSERT values can't
  reference columns) and `DAALO...DIKHAO` (`stmt.select` set: execute the
  nested `Select`, coerce each result row against the target schema).
  `TAKRAAV PAR BADLO` (upsert): for each row, `findConflict` probes every
  unique/composite index for a colliding existing row; if found, convert
  that row into an `Update` (apply `onConflictUpdate`'s assignments to
  the existing row) instead of an insert. A conflict **within the same
  incoming batch** (two new rows colliding with each other, neither
  existing yet) is still a hard error, not an upsert — upsert only
  rescues conflicts against rows that already exist in the table.
- `Update`/`Delete`: first collect the full set of matching `(rowId,
  values)` pairs via `chooseAccess`-guided lookup or full scan, THEN
  WHERE-filter that already-collected set, THEN mutate — never re-scan
  the table while mutating it (the Halloween-problem guard: an `Update`
  that increments every row's own filter column must not re-match rows
  it just changed). Both re-run `checkFk`/`checkNoChildren` (RESTRICT
  semantics: refuse if the change would orphan a child row or violate an
  FK) and `checkUnique` (excluding the rows' own old ids, so an UPDATE
  that doesn't actually change a unique column's value doesn't spuriously
  conflict with itself) before committing.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_engine_dml.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/parser.h"
#include "meradb/errors.h"
#include <filesystem>

using namespace meradb;

namespace {
std::string tempDataDir() {
    auto dir = std::filesystem::temp_directory_path() / ("meradb_dml_test_" + std::to_string(rand()));
    std::filesystem::create_directories(dir);
    return dir.string();
}
Result run(Engine& e, const std::string& sql) {
    Result last;
    for (auto& r : e.runScript(sql)) last = r;
    return last;
}
}

TEST_CASE("INSERT validates all rows before writing any", "[engine][dml]") {
    auto dir = tempDataDir();
    Engine e(dir);
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI, naam TEXT ZAROORI);");
    auto result = run(e, "DAALO MEIN t (id, naam) MAAN (1, 'a'), (2, 'b'), (3, KHALI);");
    REQUIRE_FALSE(result.error.empty());  // 3rd row violates ZAROORI/NOT NULL
    auto check = run(e, "DIKHAO * SE t;");
    REQUIRE(check.rows.empty());  // nothing from the batch was written
    std::filesystem::remove_all(dir);
}

TEST_CASE("INSERT ... SELECT copies rows through coercion", "[engine][dml]") {
    auto dir = tempDataDir();
    Engine e(dir);
    run(e, "BANAO TABLE src (id INT MUKHYA KUNJI); BANAO TABLE dst (id INT MUKHYA KUNJI);");
    run(e, "DAALO MEIN src (id) MAAN (1), (2);");
    run(e, "DAALO MEIN dst DIKHAO * SE src;");
    auto result = run(e, "DIKHAO * SE dst;");
    REQUIRE(result.rows.size() == 2);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Upsert rescues a conflict against an existing row", "[engine][dml]") {
    auto dir = tempDataDir();
    Engine e(dir);
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI, naam TEXT);");
    run(e, "DAALO MEIN t (id, naam) MAAN (1, 'old');");
    run(e, "DAALO MEIN t (id, naam) MAAN (1, 'new') TAKRAAV PAR BADLO naam = 'new';");
    auto result = run(e, "DIKHAO naam SE t;");
    REQUIRE(result.rows.size() == 1);
    REQUIRE(std::get<std::string>(result.rows[0][0].data) == "new");
    std::filesystem::remove_all(dir);
}

TEST_CASE("UPDATE avoids the Halloween problem (does not re-match rows it just changed)", "[engine][dml]") {
    auto dir = tempDataDir();
    Engine e(dir);
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI, umar INT);");
    run(e, "DAALO MEIN t (id, umar) MAAN (1, 10), (2, 20);");
    run(e, "BADLO t RAKHO umar = umar + 100 JAHAN umar < 50;");
    auto result = run(e, "DIKHAO umar SE t KRAM umar;");
    REQUIRE(std::get<int64_t>(result.rows[0][0].data) == 110);
    REQUIRE(std::get<int64_t>(result.rows[1][0].data) == 120);
    std::filesystem::remove_all(dir);
}

TEST_CASE("DELETE refuses when a child row still references it (FK RESTRICT)", "[engine][dml]") {
    auto dir = tempDataDir();
    Engine e(dir);
    run(e, "BANAO TABLE courses (id INT MUKHYA KUNJI); "
           "BANAO TABLE students (id INT MUKHYA KUNJI, cid INT SANDARBH courses(id));");
    run(e, "DAALO MEIN courses (id) MAAN (1);");
    run(e, "DAALO MEIN students (id, cid) MAAN (1, 1);");
    auto result = run(e, "MITAO SE courses JAHAN id = 1;");
    REQUIRE_FALSE(result.error.empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("UPDATE re-checks uniqueness excluding the row's own old id", "[engine][dml]") {
    auto dir = tempDataDir();
    Engine e(dir);
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI);");
    run(e, "DAALO MEIN t (id) MAAN (1);");
    auto result = run(e, "BADLO t RAKHO id = 1 JAHAN id = 1;");  // no-op change to its own value
    REQUIRE(result.error.empty());
    std::filesystem::remove_all(dir);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_engine_dml.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure.

- [ ] **Step 3: Implement `execInsert`, `execUpdate`, `execDelete`, and
  their shared validation helpers in `cpp/src/engine.cpp`**, following
  the behaviors above. Re-check `engine.py`'s `_validate_row`,
  `_find_conflict`, `_check_unique`, `_check_fk`, `_check_no_children`
  directly for exact validation-order edge cases (e.g. whether CHECK runs
  before or after FK validation) rather than re-deriving the order from
  first principles — order matters for which error message a malformed
  row produces, which the cross-engine diffing in Task 21 will catch if
  it doesn't match.

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "engine.*dml"`
Expected: all `[engine][dml]` tests PASS.

- [ ] **Step 5: Commit**

```bash
git add cpp/src/engine.cpp cpp/tests/test_engine_dml.cpp cpp/tests/CMakeLists.txt
git commit -m "Add C++ engine DML execution (insert/update/delete, upsert, FK checks)"
```

---

### Task 18: Engine — SELECT execution (joins, GROUP BY, subqueries, set ops)

**Files:**
- Modify: `cpp/src/engine.cpp`
- Test: `cpp/tests/test_engine_select.cpp`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 9-17, `meradb::Scope`/`bind`/`chooseAccess`/`chooseJoin`
  /`naturalJoinCondition` (Task 12), `meradb::evaluate`/`findSubqueries`/
  `findAggregates` (Task 13), `meradb::computeAggregate` (Task 14).
- Produces: working `execSelect`, `execSetOp`, plus private helpers
  `resolveSource`, `selectSourcesScope`, `runSubquery`, `correlate`,
  `groupRows`, `checkGrouping`.

Execution pipeline, in this exact order (matches the explicit comment in
`engine.py`'s source — re-derive any step's precise semantics from there
if a test fails in a way this summary doesn't explain):
**resolve sources → JOIN (apply each in `stmt.joins` order, hash-join
when `chooseJoin` finds an equi-condition else nested loop, LEFT/RIGHT/
FULL padding with `scope.nullRow(i)` for non-matches, NATURAL synthesizing
its ON via `naturalJoinCondition`) → WHERE (scan/`chooseAccess`-guided
lookup on source 0, then filter every joined row through the bound WHERE
via `evaluate`+`isTrue`) → GROUP BY + aggregates (partition rows by
evaluated `groupBy` expressions into buckets; for each bucket, compute
every distinct aggregate found via `findAggregates` over the SELECT
list/HAVING/ORDER BY, storing each under its `aggKey()` in a synthetic
row so `evaluate(FuncCall...)` can look it up) → HAVING → ORDER BY →
project (evaluate each SELECT column expression per resulting row) →
DISTINCT (dedupe projected rows) → LIMIT (truncate)**.

Subqueries: before evaluating WHERE/HAVING/projections for a given row,
find every `Subquery`/`InSubquery` node in the relevant expression via
`findSubqueries`, and for each: if none of its inner `ColumnRef`s resolve
outside its own inner `Scope` (uncorrelated), execute it once and cache
the result; otherwise (correlated), re-execute it **per outer row**,
first substituting any `ColumnRef` that fails to resolve against the
inner scope with a `Literal` built from the outer row's value at that key
(this is `correlate`/`correlateSelect` from the spec — implement by
attempting `bind()` against the inner scope and catching the
`ExecutionError` it throws for an unresolvable ref, then replacing that
specific node with a `Literal(outerRow.at(key))` before retrying). Store
every subquery's result value (a scalar for `Subquery`, a bool for
`InSubquery`) in a `std::unordered_map<const ast::Expr*, Value>` keyed by
node identity, and pass that map into every `evaluate()` call for that
row (matches Python's `id(expr)`-keyed dict exactly).

`SetOp` (`SANYUKT`/`SAAJHA`/`CHHODKAR` = UNION/INTERSECT/EXCEPT): execute
`left` and `right` (recursively — either may itself be a nested `SetOp`
or `Select`), verify both results have the same column count, then
dedupe-combine: UNION = union of distinct rows, INTERSECT = rows present
in both, EXCEPT = rows in `left` not present in `right`. (No `SANYUKT
SAB`/ALL variant in Phase 1 — dedup-only, matching the spec's stated
scope.)

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_engine_select.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include <filesystem>

using namespace meradb;

namespace {
std::string tempDataDir() {
    auto dir = std::filesystem::temp_directory_path() / ("meradb_select_test_" + std::to_string(rand()));
    std::filesystem::create_directories(dir);
    return dir.string();
}
Result run(Engine& e, const std::string& sql) {
    Result last;
    for (auto& r : e.runScript(sql)) last = r;
    return last;
}
void seedSchoolData(Engine& e) {
    run(e, "BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, cgpa FLOAT, cid INT SANDARBH courses(id)); "
           "BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT);");
    run(e, "DAALO MEIN courses (id, title) MAAN (10, 'DBMS'), (20, 'OS');");
    run(e, "DAALO MEIN students (id, naam, cgpa, cid) MAAN "
           "(1, 'Ravi', 8.4, 10), (2, 'Priya', 9.1, 10), (3, 'Aman', 7.0, KHALI);");
}
}

TEST_CASE("SELECT filters, orders, and limits", "[engine][select]") {
    auto dir = tempDataDir();
    Engine e(dir); seedSchoolData(e);
    auto r = run(e, "DIKHAO naam SE students JAHAN cgpa > 8 KRAM cgpa ULTA SIRF 1;");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(std::get<std::string>(r.rows[0][0].data) == "Priya");
    std::filesystem::remove_all(dir);
}

TEST_CASE("LEFT JOIN pads non-matching rows with NULL", "[engine][select]") {
    auto dir = tempDataDir();
    Engine e(dir); seedSchoolData(e);
    auto r = run(e, "DIKHAO s.naam, c.title SE students s BAAYAN MILAO courses c PAR s.cid = c.id "
                     "JAHAN s.naam = 'Aman';");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(r.rows[0][1].isNull());
    std::filesystem::remove_all(dir);
}

TEST_CASE("GROUP BY with HAVING computes per-group aggregates", "[engine][select]") {
    auto dir = tempDataDir();
    Engine e(dir); seedSchoolData(e);
    auto r = run(e, "DIKHAO cid, GINO(*) KAHO total SE students SAMOOH cid JINKA GINO(*) > 1;");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(std::get<int64_t>(r.rows[0][1].data) == 2);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Uncorrelated subquery in WHERE is computed once", "[engine][select]") {
    auto dir = tempDataDir();
    Engine e(dir); seedSchoolData(e);
    auto r = run(e, "DIKHAO naam SE students JAHAN cgpa > (DIKHAO AUSAT(cgpa) SE students);");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(std::get<std::string>(r.rows[0][0].data) == "Priya");
    std::filesystem::remove_all(dir);
}

TEST_CASE("Correlated subquery re-executes per outer row", "[engine][select]") {
    auto dir = tempDataDir();
    Engine e(dir); seedSchoolData(e);
    auto r = run(e, "DIKHAO c.title SE courses c "
                     "JAHAN (DIKHAO GINO(*) SE students s JAHAN s.cid = c.id) > 1;");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(std::get<std::string>(r.rows[0][0].data) == "DBMS");
    std::filesystem::remove_all(dir);
}

TEST_CASE("SetOp UNION dedupes combined rows", "[engine][select]") {
    auto dir = tempDataDir();
    Engine e(dir);
    run(e, "BANAO TABLE a (id INT); BANAO TABLE b (id INT);");
    run(e, "DAALO MEIN a (id) MAAN (1), (2);");
    run(e, "DAALO MEIN b (id) MAAN (2), (3);");
    auto r = run(e, "DIKHAO id SE a SANYUKT DIKHAO id SE b;");
    REQUIRE(r.rows.size() == 3);  // 1,2,3 — 2 deduped
    std::filesystem::remove_all(dir);
}

TEST_CASE("SetOp CHHODKAR (EXCEPT) returns rows only in the left side", "[engine][select]") {
    auto dir = tempDataDir();
    Engine e(dir);
    run(e, "BANAO TABLE a (id INT); BANAO TABLE b (id INT);");
    run(e, "DAALO MEIN a (id) MAAN (1), (2);");
    run(e, "DAALO MEIN b (id) MAAN (2);");
    auto r = run(e, "DIKHAO id SE a CHHODKAR DIKHAO id SE b;");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(std::get<int64_t>(r.rows[0][0].data) == 1);
    std::filesystem::remove_all(dir);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_engine_select.cpp` to `cpp/tests/CMakeLists.txt`. Confirm
failure.

- [ ] **Step 3: Implement `execSelect`, `execSetOp`, and their helpers in
  `cpp/src/engine.cpp`.** This is the largest single method in the whole
  port — build it up incrementally in this order and re-run the test
  file after each addition rather than writing it all at once: (a)
  single-table scan + projection, (b) WHERE filtering, (c) JOINs, (d)
  ORDER BY/LIMIT/DISTINCT, (e) GROUP BY/HAVING, (f) subqueries, (g)
  SetOp. This staged order matches the pipeline description above and
  means the test file's `TEST_CASE`s naturally pass in roughly the order
  they're written.

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "engine.*select"`
Expected: all `[engine][select]` tests PASS. Also re-run the FULL test
suite (`ctest --test-dir cpp/build`) — this is the point in the plan
where the core engine is functionally complete, so it's worth confirming
nothing in Tasks 1-17 regressed before moving on.

- [ ] **Step 5: Commit**

```bash
git add cpp/src/engine.cpp cpp/tests/test_engine_select.cpp cpp/tests/CMakeLists.txt
git commit -m "Add C++ engine SELECT execution (joins, group by, subqueries, set ops)"
```

---

### Task 19: Engine — Views, EXPLAIN, and Result formatting

**Files:**
- Modify: `cpp/src/engine.cpp`
- Test: `cpp/tests/test_engine_views_explain.cpp`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 9-18.
- Produces: working `execCreateView`, `execDropView`, `execShowViews`,
  `execExplain`, `resolveSource` extended to recognize a view name (Task
  18 stubbed this to tables only), and `Engine::runScript`/`execute`
  (the top-level entry points, not yet implemented in any earlier task —
  this is where they land since they need every statement type wired up
  first).

**Views:** `CreateView` stores the raw `queryText` via
`catalog().addView(name, queryText)` (no execution at creation time —
matches Python: a view's `SELECT` isn't validated until first used,
which is a real, intentional simplification worth keeping, not a gap to
fix). `resolveSource(name)`: if `name` matches a view, `parseScript(view
queryText)` fresh, execute it, and wrap the result rows in a
`MaterializedTable` (Task 11) — `isView()` on that table makes
`chooseAccess` (Task 12) correctly skip index lookup for it. A view
cannot be the target of `DAALO`/`BADLO`/`MITAO` — `execInsert`/
`execUpdate`/`execDelete` (Task 17) must check `catalog().views.count(
table) > 0` first and throw `ExecutionError` if so (this check was
deferred from Task 17 to here since it depends on `views` being wired
up — go back and add it to Task 17's methods now).

**EXPLAIN (`SAMJHAO`):** `execExplain` re-runs the SAME planning
decisions `execSelect` would make (`chooseAccess`, `chooseJoin` per join)
but only *describes* them as text, never executing the statement. Output
shape (one row, single `plan` column, matching `engine.py`'s
`_explain_select`): a line per stage — `"INDEX LOOKUP <table> ON
<column> = <value>"` or `"FULL SCAN <table>"` for the base table, then
per join `"HASH JOIN <table> ON <leftKey> = <rightKey>"` or `"NESTED
LOOP <table>"`, then (if present) `"SUBQUERY (correlated)"` or
`"SUBQUERY (uncorrelated, cached)"` per subquery found via
`findSubqueries` over the WHERE/HAVING/projection list.

**`runScript`/`execute`:**
```cpp
std::vector<Result> Engine::runScript(const std::string& text) {
    std::vector<Result> results;
    for (auto& stmt : parseScript(text)) {
        try {
            std::lock_guard<std::recursive_mutex> guard(instance_->lock);
            results.push_back(executeStatement(*stmt));
        } catch (const MeraDBError& e) {
            results.push_back(Result{{}, {}, "", e.message()});
        }
    }
    return results;
}
std::vector<Result> Engine::execute(const std::string& text) {
    auto results = runScript(text);
    for (auto& r : results) if (!r.error.empty()) throw ExecutionError(r.error);
    return results;
}
```
(`execute` re-throws using `ExecutionError` generically here since the
original per-layer stage was already folded into `r.error`'s text by
`e.message()` in `runScript` — this matches Python's `execute()`, which
re-raises via a plain string-carrying exception rather than trying to
reconstruct the original exception's exact subclass.)

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_engine_views_explain.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include <filesystem>

using namespace meradb;

namespace {
std::string tempDataDir() {
    auto dir = std::filesystem::temp_directory_path() / ("meradb_views_test_" + std::to_string(rand()));
    std::filesystem::create_directories(dir);
    return dir.string();
}
}

TEST_CASE("CREATE VIEW then SELECT FROM it re-executes the stored query", "[engine][views]") {
    auto dir = tempDataDir();
    Engine e(dir);
    e.execute("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, cgpa FLOAT);");
    e.execute("DAALO MEIN students (id, naam, cgpa) MAAN (1, 'Ravi', 9.5), (2, 'Priya', 6.0);");
    e.execute("BANAO VIEW toppers DIKHAO naam SE students JAHAN cgpa > 8;");
    auto results = e.execute("DIKHAO * SE toppers;");
    REQUIRE(results[0].rows.size() == 1);
    REQUIRE(std::get<std::string>(results[0].rows[0][0].data) == "Ravi");
}

TEST_CASE("A view reflects later changes to its underlying table", "[engine][views]") {
    auto dir = tempDataDir();
    Engine e(dir);
    e.execute("BANAO TABLE t (id INT MUKHYA KUNJI, x INT); BANAO VIEW v DIKHAO * SE t JAHAN x > 5;");
    e.execute("DAALO MEIN t (id, x) MAAN (1, 10);");
    REQUIRE(e.execute("DIKHAO * SE v;")[0].rows.size() == 1);
    e.execute("DAALO MEIN t (id, x) MAAN (2, 1);");
    REQUIRE(e.execute("DIKHAO * SE v;")[0].rows.size() == 1);  // second row still filtered out
}

TEST_CASE("INSERT/UPDATE/DELETE against a view name is refused", "[engine][views]") {
    auto dir = tempDataDir();
    Engine e(dir);
    e.execute("BANAO TABLE t (id INT); BANAO VIEW v DIKHAO * SE t;");
    auto r = e.runScript("DAALO MEIN v (id) MAAN (1);");
    REQUIRE_FALSE(r[0].error.empty());
}

TEST_CASE("EXPLAIN reports INDEX LOOKUP for a unique-column equality", "[engine][views]") {
    auto dir = tempDataDir();
    Engine e(dir);
    e.execute("BANAO TABLE t (id INT MUKHYA KUNJI);");
    e.execute("DAALO MEIN t (id) MAAN (1);");
    auto r = e.execute("SAMJHAO DIKHAO * SE t JAHAN id = 1;")[0];
    REQUIRE(r.rows[0][0].data == Value(std::string("INDEX LOOKUP t ON id = 1")).data ||
            std::get<std::string>(r.rows[0][0].data).find("INDEX LOOKUP") != std::string::npos);
}

TEST_CASE("EXPLAIN does not actually execute the statement (no rows touched)", "[engine][views]") {
    auto dir = tempDataDir();
    Engine e(dir);
    e.execute("BANAO TABLE t (id INT MUKHYA KUNJI);");
    e.execute("SAMJHAO DAALO MEIN t (id) MAAN (1);");
    // If EXPLAIN had actually executed the INSERT, this SELECT would see 1 row.
    REQUIRE(e.execute("DIKHAO * SE t;")[0].rows.empty());
}

TEST_CASE("runScript never throws; a failing statement becomes a Result.error and execution continues", "[engine][views]") {
    auto dir = tempDataDir();
    Engine e(dir);
    auto results = e.runScript("BANAO TABLE t (id INT); DIKHAO * SE anjaan_table; DIKHAO * SE t;");
    REQUIRE(results.size() == 3);
    REQUIRE(results[0].error.empty());
    REQUIRE_FALSE(results[1].error.empty());
    REQUIRE(results[2].error.empty());  // continued after the failure
}

TEST_CASE("execute throws on the first error instead of collecting it", "[engine][views]") {
    auto dir = tempDataDir();
    Engine e(dir);
    REQUIRE_THROWS_AS(e.execute("DIKHAO * SE anjaan_table;"), MeraDBError);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_engine_views_explain.cpp` to `cpp/tests/CMakeLists.txt`.
Confirm failure.

- [ ] **Step 3: Implement `execCreateView`, `execDropView`,
  `execShowViews`, `execExplain`, `runScript`, `execute` in
  `cpp/src/engine.cpp`; extend `resolveSource` (from Task 18) to check
  `catalog().views` before falling back to `catalog().get(name)`/`Table`;
  add the view-target guard to `execInsert`/`execUpdate`/`execDelete`
  (Task 17) now.**

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "engine.*views"`
Expected: all `[engine][views]` tests PASS. Then run the FULL suite:
`ctest --test-dir cpp/build --output-on-failure` — every test from every
prior task must still pass. This is the last engine task in Phase 1; the
core engine (everything except the CLI wrapper) is complete after this
commit.

- [ ] **Step 5: Commit**

```bash
git add cpp/src/engine.cpp cpp/tests/test_engine_views_explain.cpp cpp/tests/CMakeLists.txt
git commit -m "Add C++ engine views, EXPLAIN, and top-level runScript/execute"
```

---

### Task 20: CLI local-mode runner

**Files:**
- Modify: `cpp/src/main.cpp` (replace the Task 1 placeholder)
- Test: `cpp/tests/test_cli_output.cpp` (tests the output-formatting
  helper function directly, not the `main()` binary — process-level CLI
  testing is covered by Task 21's script-file verification instead)
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb::Engine`/`Result` (Tasks 15-19).
- Produces: a `meradb_cli` executable supporting `meradb_cli run <path>
  [--data <dir>]`, and a testable `formatResult(const Result&) ->
  std::string` helper used by `main()` to render each statement's output
  the way the Python shell/`meradb run` does: a column-aligned table for
  rows, a bare message line for DDL/DML confirmations, and `[Stage Galti]
  <message>` for errors — printed to stdout, continuing to the next
  statement (matching `runScript`'s never-throws contract), with the
  process's exit code set to `1` if any statement errored (matching
  the Python CLI's documented "`exit=1` is expected when a script
  contains a deliberate error" behavior that `examples/demo.mdb` and
  `examples/rdbms_lab_coverage.mdb` both rely on).

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_cli_output.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/cli_format.h"

using namespace meradb;

TEST_CASE("formatResult renders a column-aligned table for rows", "[cli]") {
    Result r;
    r.columns = {"naam", "cgpa"};
    r.rows = {{Value(std::string("Ravi")), Value(9.5)}, {Value(std::string("Priya")), Value(6.0)}};
    std::string out = formatResult(r);
    REQUIRE(out.find("naam") != std::string::npos);
    REQUIRE(out.find("Ravi") != std::string::npos);
    REQUIRE(out.find("Priya") != std::string::npos);
}

TEST_CASE("formatResult renders a bare message for DDL/DML confirmations", "[cli]") {
    Result r; r.message = "Table 'students' ban gaya";
    REQUIRE(formatResult(r) == "Table 'students' ban gaya");
}

TEST_CASE("formatResult renders errors with the Galti tag", "[cli]") {
    Result r; r.error = "[Execution Galti] Table 'x' nahi mila";
    REQUIRE(formatResult(r) == "[Execution Galti] Table 'x' nahi mila");
}

TEST_CASE("formatResult renders KHALI for null cells in a table", "[cli]") {
    Result r; r.columns = {"x"}; r.rows = {{Value()}};
    REQUIRE(formatResult(r).find("KHALI") != std::string::npos);
}
```

- [ ] **Step 2: Add test file to build and verify it fails**

Add `test_cli_output.cpp` to `cpp/tests/CMakeLists.txt`. Confirm failure
(`meradb/cli_format.h` missing).

- [ ] **Step 3: Write `cpp/include/meradb/cli_format.h` and
  `cpp/src/cli_format.cpp`:**

```cpp
// cpp/include/meradb/cli_format.h
#pragma once
#include "meradb/engine.h"
#include <string>

namespace meradb {
std::string formatResult(const Result& r);
}
```

```cpp
// cpp/src/cli_format.cpp
#include "meradb/cli_format.h"
#include "meradb/datatypes.h"
#include <sstream>
#include <vector>

namespace meradb {

std::string formatResult(const Result& r) {
    if (!r.error.empty()) return r.error;
    if (r.columns.empty()) return r.message;

    std::vector<std::vector<std::string>> cells;
    cells.push_back(r.columns);
    for (auto& row : r.rows) {
        std::vector<std::string> cellRow;
        for (auto& v : row) cellRow.push_back(formatValue(v));
        cells.push_back(cellRow);
    }
    std::vector<size_t> widths(r.columns.size(), 0);
    for (auto& row : cells) for (size_t i = 0; i < row.size(); ++i) widths[i] = std::max(widths[i], row[i].size());

    std::ostringstream out;
    for (size_t rowI = 0; rowI < cells.size(); ++rowI) {
        for (size_t i = 0; i < cells[rowI].size(); ++i) {
            out << cells[rowI][i];
            out << std::string(widths[i] - cells[rowI][i].size() + 2, ' ');
        }
        out << "\n";
        if (rowI == 0) {
            for (size_t i = 0; i < widths.size(); ++i) out << std::string(widths[i], '-') << "  ";
            out << "\n";
        }
    }
    std::string s = out.str();
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

}  // namespace meradb
```

- [ ] **Step 4: Add `src/cli_format.cpp` to `meradb_core` in
  `cpp/CMakeLists.txt`; run the `[cli]` tests and verify they pass.**

Run: `cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R cli`

- [ ] **Step 5: Write `cpp/src/main.cpp`:**

```cpp
// cpp/src/main.cpp
#include "meradb/engine.h"
#include "meradb/cli_format.h"
#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    std::string dataDir = "data";
    std::string scriptPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "run" && i + 1 < argc) scriptPath = argv[++i];
        else if (arg == "--data" && i + 1 < argc) dataDir = argv[++i];
    }
    if (scriptPath.empty()) {
        std::cerr << "Usage: meradb_cli run <script.mdb> [--data <dir>]\n";
        return 1;
    }
    std::ifstream file(scriptPath);
    if (!file) {
        std::cerr << "Script file nahi mili: " << scriptPath << "\n";
        return 1;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    meradb::Engine engine(dataDir);
    bool hadError = false;
    for (auto& result : engine.runScript(buffer.str())) {
        std::cout << meradb::formatResult(result) << "\n";
        if (!result.error.empty()) hadError = true;
    }
    return hadError ? 1 : 0;
}
```

- [ ] **Step 6: Build and manually smoke-test against a real script**

Run:
```bash
cmake --build cpp/build
./cpp/build/meradb_cli run examples/demo.mdb --data /tmp/meradb_cpp_smoke
```
Expected: runs to completion, printing table output and some `[...
Galti]` lines for the script's deliberate errors, exiting with code 1
(same documented contract as the Python `meradb run`). This will likely
surface a handful of small bugs across Tasks 6-19 that the unit tests
didn't happen to cover — fix them here rather than deferring to Task 21,
since Task 21 assumes the CLI already basically works and focuses on
byte-for-byte comparison against the Python engine, not first-time
debugging.

- [ ] **Step 7: Run the full test suite one more time and commit**

```bash
ctest --test-dir cpp/build --output-on-failure
git add cpp/include/meradb/cli_format.h cpp/src/cli_format.cpp cpp/src/main.cpp cpp/tests/test_cli_output.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add C++ CLI local-mode runner"
```

---

### Task 21: Cross-engine verification against example scripts, and docs

**Files:**
- Create: `cpp/tests/cross_engine_diff.py` (a small verification script,
  Python, since it needs to drive BOTH the existing Python engine and
  the new C++ CLI binary — not a Catch2 test)
- Create: `docs/CPP.md`
- Modify: `README.md` (add a short section pointing at `docs/CPP.md` and
  noting both implementations exist)

**Interfaces:**
- Consumes: `meradb_cli` (Task 20), the existing Python `meradb` package
  (unchanged, used only as a comparison oracle).
- Produces: a repeatable verification script and a build/run guide for
  the C++ version. Nothing here is consumed by later Phase-1 code — this
  is the phase's final acceptance check.

Phase 2 (server/networking) is a separate plan, written after this one
is executed and this task's diffing confirms the core engine is solid —
per the spec's phased-delivery section, don't start Phase 2 work under
this plan.

- [ ] **Step 1: Write the cross-engine diff script**

```python
# cpp/tests/cross_engine_diff.py
"""
Runs the same .mdb script through the Python engine and the C++ CLI,
and diffs their output line-for-line. Exits non-zero (with a diff
printed) on any mismatch, so this can be wired into a CI-style check
later if desired -- for now it's a manual verification step for Task 21.

Usage:
    python cpp/tests/cross_engine_diff.py examples/demo.mdb
    python cpp/tests/cross_engine_diff.py examples/rdbms_lab_coverage.mdb
"""
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CPP_CLI = REPO_ROOT / "cpp" / "build" / "meradb_cli"


def run_python(script_path: str, data_dir: str) -> str:
    result = subprocess.run(
        [sys.executable, "-m", "meradb", "run", "--local", "--data", data_dir, script_path],
        cwd=REPO_ROOT, capture_output=True, text=True,
    )
    return result.stdout


def run_cpp(script_path: str, data_dir: str) -> str:
    result = subprocess.run(
        [str(CPP_CLI), "run", script_path, "--data", data_dir],
        cwd=REPO_ROOT, capture_output=True, text=True,
    )
    return result.stdout


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: cross_engine_diff.py <script.mdb>")
        return 2
    script_path = sys.argv[1]

    with tempfile.TemporaryDirectory() as py_dir, tempfile.TemporaryDirectory() as cpp_dir:
        python_output = run_python(script_path, py_dir)
        cpp_output = run_cpp(script_path, cpp_dir)

    py_lines = python_output.splitlines()
    cpp_lines = cpp_output.splitlines()
    if py_lines == cpp_lines:
        print(f"MATCH: {script_path} ({len(py_lines)} lines identical)")
        return 0

    print(f"MISMATCH: {script_path}")
    import difflib
    diff = difflib.unified_diff(py_lines, cpp_lines, fromfile="python", tofile="cpp", lineterm="")
    print("\n".join(diff))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
```

Note: this script assumes the Python CLI's output format for `meradb run
--local` and the C++ CLI's `formatResult` (Task 20) produce visually
identical table formatting (same column widths/separators). If the first
run shows only cosmetic formatting differences (not data differences),
that's a real finding — go fix `formatResult` in Task 20's file to match
the Python shell's exact table-rendering rules (check `meradb/repl.py`'s
output formatting) rather than loosening this diff script to ignore
formatting, since exact-output parity is the whole point of this
verification step.

- [ ] **Step 2: Build the C++ CLI in release mode and run the diff
  script against both example scripts**

```bash
cmake --build cpp/build --config Release
python cpp/tests/cross_engine_diff.py examples/demo.mdb
python cpp/tests/cross_engine_diff.py examples/rdbms_lab_coverage.mdb
```
Expected: `MATCH` for both. If either mismatches, the printed diff points
at exactly which output line differs — trace it back to the responsible
engine/planner/evaluator method and fix it there (not by special-casing
the CLI output), then re-run this step. Repeat until both scripts match.

Note: `examples/demo.mdb` and `examples/rdbms_lab_coverage.mdb` both
currently exercise Phase 2 features too (users/privileges, triggers,
procedures — see the spec's phase breakdown). Since Phase 1 doesn't
implement those statement types at all, the C++ parser will throw a
`ParseError` on any such statement. For this task, **create trimmed
copies** — `cpp/tests/demo_phase1.mdb` and
`cpp/tests/rdbms_lab_coverage_phase1.mdb` — by copying the originals and
deleting only the sections explicitly marked as Phase B (users,
triggers, procedures) in their comments, and diff against those trimmed
copies instead of the originals. Phase 2's own plan will do the full,
untrimmed diff once those statement types exist in C++ too.

- [ ] **Step 3: Write `docs/CPP.md`**

Cover: prerequisites (CMake 3.20+, a C++17 compiler), build instructions
(`cmake -S cpp -B cpp/build && cmake --build cpp/build`), running the
test suite (`ctest --test-dir cpp/build`), running a script
(`./cpp/build/meradb_cli run <script.mdb> --data <dir>`), a short note
that this is Phase 1 (core engine only — no server/shell/workbench yet,
those are later phases per
`docs/cpp-port/specs/2026-09-27-cpp-port-design.md`), and a table
mapping every Python module to its C++ header/source pair (reuse the
"Layer-by-layer mapping" table from the spec).

- [ ] **Step 4: Add a short pointer section to `README.md`**

A few sentences: two implementations now exist (`meradb/` Python,
`cpp/` C++), link to `docs/CPP.md`, note Phase 1 status (core engine
only).

- [ ] **Step 5: Run the full C++ test suite one final time, commit**

```bash
ctest --test-dir cpp/build --output-on-failure
git add cpp/tests/cross_engine_diff.py cpp/tests/demo_phase1.mdb cpp/tests/rdbms_lab_coverage_phase1.mdb docs/CPP.md README.md
git commit -m "Add cross-engine verification and C++ build docs; Phase 1 complete"
```

---

## Phase 1 completion checklist

- [ ] All 21 tasks' tests pass individually and as a full suite
      (`ctest --test-dir cpp/build`).
- [ ] `meradb_cli run examples/../cpp/tests/demo_phase1.mdb` produces
      output that matches the Python engine's output line-for-line
      (Task 21).
- [ ] `meradb_cli run examples/../cpp/tests/rdbms_lab_coverage_phase1.mdb`
      matches likewise.
- [ ] `docs/CPP.md` exists and its build instructions work on a fresh
      clone (no manually-installed dependencies beyond a C++17 compiler
      and CMake).
- [ ] The existing Python implementation is untouched — `git diff
      meradb/` against the commit before this plan started shows nothing.
- [ ] Phase 2 (server/client/protocol/users/triggers/procedures) is
      planned separately, after this checklist is confirmed complete —
      see `docs/cpp-port/specs/2026-09-27-cpp-port-design.md`'s phase
      breakdown.

