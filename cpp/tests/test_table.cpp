// cpp/tests/test_table.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/storage.h"
#include "meradb/table.h"
#include "test_util.h"

using namespace meradb;
using meradb_test::TempDir;

namespace {
TableSchema makeSchema() {
    TableSchema s;
    s.name = "t";
    Column id; id.name = "id"; id.typeName = "INT"; id.primaryKey = true;
    Column naam; naam.name = "naam"; naam.typeName = "TEXT";
    s.columns = {id, naam};
    return s;
}

// enroll(sid INT, cid INT, email TEXT ANOKHA, MUKHYA KUNJI (sid, cid))
TableSchema makeCompositeSchema() {
    TableSchema s;
    s.name = "enroll";
    Column sid; sid.name = "sid"; sid.typeName = "INT";
    Column cid; cid.name = "cid"; cid.typeName = "INT";
    Column email; email.name = "email"; email.typeName = "TEXT"; email.unique = true;
    s.columns = {sid, cid, email};
    s.compositePk = std::vector<std::string>{"sid", "cid"};
    return s;
}

Value I(int64_t v) { return Value(v); }
Value S(const char* v) { return Value(std::string(v)); }
}  // namespace

TEST_CASE("Table inserts and scans rows", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}, {I(2), S("Priya")}});
    auto rows = t.rows();
    REQUIRE(rows.size() == 2);
    REQUIRE(std::get<std::string>(rows[1].second[1].data) == "Priya");
    REQUIRE(std::get<std::string>(t.get(rows[0].first).value()[1].data) == "Ravi");
}

TEST_CASE("Table.lookup uses the primary-key hash index", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}, {I(2), S("Priya")}});
    auto hits = t.lookup(0, I(2));
    REQUIRE(hits.size() == 1);
    REQUIRE(std::get<std::string>(hits[0].second[1].data) == "Priya");
    REQUIRE(t.lookup(0, I(999)).empty());
    REQUIRE(t.lookup(0, Value()).empty());
    // unindexed column: an error, never a silently empty answer
    REQUIRE_THROWS_AS(t.lookup(1, S("Ravi")), ExecutionError);
}

TEST_CASE("Index keys follow Python dict semantics: 2 == 2.0 == value, SACH == 1", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}, {I(2), S("Priya")}});
    REQUIRE(t.lookup(0, Value(2.0)).size() == 1);
    REQUIRE(t.lookup(0, Value(true)).size() == 1);
    REQUIRE(t.lookup(0, Value(1.5)).empty());
    REQUIRE(t.lookup(0, S("2")).empty());

    ValueVecHash h;
    ValueVecEq eq;
    REQUIRE(eq({I(2)}, {Value(2.0)}));
    REQUIRE(h({I(2)}) == h({Value(2.0)}));
    REQUIRE(eq({Value(-0.0)}, {I(0)}));
    REQUIRE_FALSE(eq({I(9007199254740993)}, {Value(9007199254740992.0)}));
    REQUIRE_FALSE(eq({S("a")}, {Value(parseDate("2024-01-01"))}));
    REQUIRE(eq({Value(parseDate("2024-01-01"))}, {Value(parseDate("2024-01-01"))}));
}

TEST_CASE("Table.deleteMany removes rows and updates the index", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}});
    t.indexes();  // built before the delete, so deleteMany must keep it in sync
    t.deleteMany(t.rows());
    REQUIRE(t.rows().empty());
    REQUIRE(t.lookup(0, I(1)).empty());
    REQUIRE(t.indexes().at({0}).empty());
}

TEST_CASE("deleteMany keeps an index entry that points at a newer row (UPDATE)", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}});
    t.indexes();
    auto old = t.rows();
    t.insertMany({{I(1), S("Ravi Kumar")}});  // new version of the row first...
    t.deleteMany(old);                          // ...then the old one goes
    auto hits = t.lookup(0, I(1));
    REQUIRE(hits.size() == 1);
    REQUIRE(std::get<std::string>(hits[0].second[1].data) == "Ravi Kumar");
}

TEST_CASE("Table.invalidateIndexes forces a rebuild on next access", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}});
    REQUIRE(t.lookup(0, I(1)).size() == 1);  // builds the index
    t.heap().rewrite({encodeRow({I(5), S("Asha")}, t.schema().types())});  // row ids change
    t.invalidateIndexes();
    REQUIRE(t.lookup(0, I(1)).empty());
    REQUIRE(t.lookup(0, I(5)).size() == 1);  // rebuilt transparently
}

TEST_CASE("Composite and UNIQUE indexes skip KHALI and track inserts", "[table]") {
    TempDir dir;
    std::string path = dir.file("enroll.tbl");
    HeapFile(path).create();
    Table t(makeCompositeSchema(), path);
    t.insertMany({{I(1), I(10), S("a@x")}, {I(1), I(11), Value()}, {I(2), Value(), S("c@x")}});
    IndexMap& idx = t.indexes();
    REQUIRE(idx.size() == 2);  // email {2} + composite pk {0, 1}
    REQUIRE(idx.at({2}).size() == 2);     // the KHALI email is not indexed
    REQUIRE(idx.at({0, 1}).size() == 2);  // (2, KHALI) is not indexed
    REQUIRE(idx.at({0, 1}).count({I(1), I(11)}) == 1);

    t.insertMany({{I(3), I(30), S("d@x")}});  // index already built: updated in place
    REQUIRE(idx.at({0, 1}).count({I(3), I(30)}) == 1);
    REQUIRE(idx.at({2}).count({S("d@x")}) == 1);
}

TEST_CASE("Tables sharing an IndexCache see one index", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    auto cache = std::make_shared<std::optional<IndexMap>>();
    {
        Table t(makeSchema(), path, cache);
        t.insertMany({{I(1), S("Ravi")}});
        t.indexes();
    }
    REQUIRE(cache->has_value());
    Table t2(makeSchema(), path, cache);
    t2.insertMany({{I(2), S("Priya")}});
    REQUIRE((*cache)->at({0}).size() == 2);
    t2.invalidateIndexes();
    REQUIRE_FALSE(cache->has_value());
}

TEST_CASE("MaterializedTable behaves like Table for reads, is flagged as a view", "[table]") {
    std::vector<RowValues> data = {{I(1)}, {I(2)}};
    MaterializedTable mt(data);
    REQUIRE(mt.isView());
    REQUIRE(mt.rows().size() == 2);
    REQUIRE(std::get<int64_t>(mt.get(1).value()[0].data) == 2);
    REQUIRE_FALSE(mt.get(2).has_value());
    REQUIRE_FALSE(mt.get(-1).has_value());
    REQUIRE_THROWS_AS(mt.insertMany({{I(3)}}), ExecutionError);
    REQUIRE_THROWS_AS(mt.lookup(0, I(1)), ExecutionError);

    Table t(makeSchema(), "unused.tbl");
    REQUIRE_FALSE(t.isView());
}

namespace {
// u(id INT ANOKHA, a INT ANOKHA, b TEXT ANOKHA, c INT, d INT,
//   e INT ANOKHA, f INT ANOKHA, ANOKHA (c, d), MUKHYA KUNJI (e, f))
TableSchema makeManyUniqueSchema() {
    TableSchema s;
    s.name = "u";
    auto col = [](const char* name, const char* type) {
        Column c; c.name = name; c.typeName = type;
        return c;
    };
    Column id = col("id", "INT"); id.unique = true;
    Column a = col("a", "INT"); a.unique = true;
    Column b = col("b", "TEXT"); b.unique = true;
    Column e = col("e", "INT"); e.unique = true;
    Column f = col("f", "INT"); f.unique = true;
    s.columns = {id, a, b, col("c", "INT"), col("d", "INT"), e, f};
    s.compositeUnique = {{"c", "d"}};
    s.compositePk = std::vector<std::string>{"e", "f"};
    return s;
}

std::string pyReprOf(const Value& v) {
    if (std::holds_alternative<std::string>(v.data)) return pyRepr(std::get<std::string>(v.data));
    return formatValue(v);
}

// A port of engine.py's _check_unique first-match loop (Task 16 will own
// the real one): the FIRST index, in indexes() order, holding one of the
// row's keys names the error.
std::string firstDuplicateError(Table& t, const RowValues& values) {
    const auto& cols = t.schema().columns;
    for (auto& [positions, index] : t.indexes()) {
        std::vector<Value> key;
        bool hasNull = false;
        for (size_t p : positions) {
            hasNull = hasNull || values[p].isNull();
            key.push_back(values[p]);
        }
        if (hasNull || index.count(key) == 0) continue;
        if (positions.size() == 1)
            return "Duplicate value " + pyReprOf(key[0]) + " column '" + cols[positions[0]].name +
                   "' mein -- is column mein har value alag honi chahiye";
        std::string tuple = "(", names = "[";
        for (size_t i = 0; i < positions.size(); ++i) {
            tuple += (i ? ", " : "") + pyReprOf(key[i]);
            names += (i ? ", " : "") + pyRepr(cols[positions[i]].name);
        }
        return "Duplicate value " + tuple + ") columns " + names + "] mein -- ye combination alag hona chahiye";
    }
    return "";
}
}  // namespace

TEST_CASE("indexes() iterates in Python's order: unique columns, composite ANOKHA, composite PK", "[table]") {
    TempDir dir;
    std::string path = dir.file("u.tbl");
    HeapFile(path).create();
    Table t(makeManyUniqueSchema(), path);
    std::vector<std::vector<size_t>> order;
    for (auto& [positions, index] : t.indexes()) order.push_back(positions);
    REQUIRE(order == std::vector<std::vector<size_t>>{{0}, {1}, {2}, {5}, {6}, {3, 4}, {5, 6}});
}

TEST_CASE("First-match duplicate error names the same column Python does", "[table]") {
    TempDir dir;
    std::string path = dir.file("u.tbl");
    HeapFile(path).create();
    Table t(makeManyUniqueSchema(), path);
    t.insertMany({{I(1), I(10), S("x"), I(5), I(6), I(7), I(8)}});

    // Expected strings are the Python engine's actual messages for
    //   BANAO TABLE u (id INT ANOKHA, a INT ANOKHA, b TEXT ANOKHA, c INT, d INT,
    //                  e INT ANOKHA, f INT ANOKHA, ANOKHA (c, d), MUKHYA KUNJI (e, f));
    //   DAALO MEIN u MAAN (1, 10, 'x', 5, 6, 7, 8);
    // followed by each clashing DAALO below (same values as the rows here).
    // (2, 10, 'x', 5, 6, ...): clashes on a, b and (c, d) -> a wins
    REQUIRE(firstDuplicateError(t, {I(2), I(10), S("x"), I(5), I(6), I(70), I(80)}) ==
            "Duplicate value 10 column 'a' mein -- is column mein har value alag honi chahiye");
    // (3, 11, 'x', 5, 6): clashes on b and (c, d) -> b wins
    REQUIRE(firstDuplicateError(t, {I(3), I(11), S("x"), I(5), I(6), I(71), I(81)}) ==
            "Duplicate value 'x' column 'b' mein -- is column mein har value alag honi chahiye");
    // (1, 12, 'y', 7, 8): clashes on id only
    REQUIRE(firstDuplicateError(t, {I(1), I(12), S("y"), I(7), I(8), I(72), I(82)}) ==
            "Duplicate value 1 column 'id' mein -- is column mein har value alag honi chahiye");
    // (4, 13, 'z', 5, 6): clashes on (c, d) only
    REQUIRE(firstDuplicateError(t, {I(4), I(13), S("z"), I(5), I(6), I(73), I(83)}) ==
            "Duplicate value (5, 6) columns ['c', 'd'] mein -- ye combination alag hona chahiye");
}

TEST_CASE("TAKRAAV first-match picks the row of the earliest-declared index", "[table]") {
    TempDir dir;
    std::string path = dir.file("u.tbl");
    HeapFile(path).create();
    Table t(makeManyUniqueSchema(), path);
    t.insertMany({{I(1), I(10), S("x"), I(5), I(6), I(7), I(8)}, {I(2), I(20), S("w"), I(1), I(1), I(9), I(9)}});
    auto rows = t.rows();
    // (9, 20, 'x', ...) clashes with row 2 on `a` and with row 1 on `b`:
    // Python's _find_conflict returns row 2, because `a` comes first.
    RowValues incoming = {I(9), I(20), S("x"), I(0), I(0), I(0), I(0)};
    std::optional<int64_t> hit;
    for (auto& [positions, index] : t.indexes()) {
        std::vector<Value> key;
        for (size_t p : positions) key.push_back(incoming[p]);
        auto it = index.find(key);
        if (it != index.end()) {
            hit = it->second;
            break;
        }
    }
    REQUIRE(hit == rows[1].first);
}

TEST_CASE("insertMany refuses a row with the wrong number of values", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.indexes();
    REQUIRE_THROWS_AS(t.insertMany({{I(1)}}), ExecutionError);
    REQUIRE(t.rows().empty());  // nothing reached the file
    REQUIRE_THROWS_AS(t.deleteMany({{8, {I(1)}}}), ExecutionError);  // short old row: no out-of-bounds read
}
